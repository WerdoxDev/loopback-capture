#include "LoopbackCapture.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
namespace
{
   struct AudioDataPacket
   {
      std::vector<std::byte> data;
   };
   const pw_stream_events kStreamEvents = []
   {
      pw_stream_events events{};
      events.version = PW_VERSION_STREAM_EVENTS;
      events.state_changed = &CLoopbackCapture::OnStreamStateChanged;
      events.process = &CLoopbackCapture::OnStreamProcess;
      return events;
   }();
   const pw_registry_events kRegistryEvents = []
   {
      pw_registry_events events{};
      events.version = PW_VERSION_REGISTRY_EVENTS;
      events.global = &CLoopbackCapture::OnRegistryGlobal;
      return events;
   }();
   void EnsurePipeWireInitialized()
   {
      static std::once_flag initialized;
      std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
   }
   // How long we're willing to wait for a matching stream node to appear
   // in the graph, and separately for the capture stream itself to reach
   // PAUSED/STREAMING once connected.
   constexpr std::int64_t kWaitTimeoutNanos = 5'000'000'000LL;
   spa_audio_info_raw MakeStandardFormat()
   {
      spa_audio_info_raw format{};
      format.format = SPA_AUDIO_FORMAT_S16_LE;
      format.rate = 48000;
      format.channels = 2;
      format.position[0] = SPA_AUDIO_CHANNEL_FL;
      format.position[1] = SPA_AUDIO_CHANNEL_FR;
      return format;
   }
   // Parses the ppid field out of /proc/<pid>/stat. The comm field (2nd
   // column) is user-controlled and wrapped in parens, so we find the
   // *last* ')' before parsing the remaining whitespace-separated fields
   // rather than naively splitting on spaces.
   std::optional<pid_t> ReadParentPid(pid_t pid)
   {
      std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
      if (!file.is_open())
      {
         return std::nullopt;
      }
      std::string line;
      if (!std::getline(file, line))
      {
         return std::nullopt;
      }
      const auto closeParen = line.rfind(')');
      if (closeParen == std::string::npos || closeParen + 2 >= line.size())
      {
         return std::nullopt;
      }
      std::istringstream rest(line.substr(closeParen + 2));
      char state = '\0';
      long ppid = 0;
      rest >> state >> ppid;
      if (rest.fail())
      {
         return std::nullopt;
      }
      return static_cast<pid_t>(ppid);
   }

   std::optional<std::uint64_t> ParsePositiveInteger(const char *value)
   {
      if (value == nullptr || *value == '\0')
      {
         return std::nullopt;
      }

      errno = 0;
      char *end = nullptr;
      const unsigned long long parsed = std::strtoull(value, &end, 10);
      if (errno != 0 || end == value || *end != '\0' || parsed == 0)
      {
         return std::nullopt;
      }
      return static_cast<std::uint64_t>(parsed);
   }

   std::optional<pid_t> ReadProcessId(const spa_dict *props)
   {
      const char *value = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
      if (value == nullptr)
      {
         value = spa_dict_lookup(props, PW_KEY_SEC_PID);
      }
      const auto parsed = ParsePositiveInteger(value);
      if (!parsed.has_value() || *parsed > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
      {
         return std::nullopt;
      }
      return static_cast<pid_t>(*parsed);
   }

   std::optional<std::uint32_t> ReadClientId(const spa_dict *props)
   {
      const auto parsed = ParsePositiveInteger(spa_dict_lookup(props, PW_KEY_CLIENT_ID));
      if (!parsed.has_value() || *parsed > std::numeric_limits<std::uint32_t>::max())
      {
         return std::nullopt;
      }
      return static_cast<std::uint32_t>(*parsed);
   }
   // Walks /proc to find every process descended from rootPid (children,
   // grandchildren, ...). Always includes rootPid itself. On any failure
   // to read /proc, degrades to just {rootPid}.
   std::unordered_set<pid_t> CollectProcessTree(pid_t rootPid)
   {
      std::unordered_set<pid_t> result{rootPid};
      DIR *procDir = opendir("/proc");
      if (procDir == nullptr)
      {
         return result;
      }
      std::unordered_map<pid_t, std::vector<pid_t>> childrenByParent;
      dirent *entry = nullptr;
      while ((entry = readdir(procDir)) != nullptr)
      {
         char *end = nullptr;
         const long value = std::strtol(entry->d_name, &end, 10);
         if (end == entry->d_name || *end != '\0' || value <= 0)
         {
            continue;
         }
         const pid_t candidate = static_cast<pid_t>(value);
         if (const auto parent = ReadParentPid(candidate); parent.has_value())
         {
            childrenByParent[*parent].push_back(candidate);
         }
      }
      closedir(procDir);
      std::vector<pid_t> pending{rootPid};
      while (!pending.empty())
      {
         const pid_t current = pending.back();
         pending.pop_back();
         const auto it = childrenByParent.find(current);
         if (it == childrenByParent.end())
         {
            continue;
         }
         for (pid_t child : it->second)
         {
            if (result.insert(child).second)
            {
               pending.push_back(child);
            }
         }
      }
      return result;
   }
}
CLoopbackCapture::~CLoopbackCapture()
{
   Shutdown();
}
bool CLoopbackCapture::InitializeCore(std::string &error)
{
   EnsurePipeWireInitialized();
   m_loop = pw_thread_loop_new("loopback-capture", nullptr);
   if (m_loop == nullptr)
   {
      error = "Failed to create the PipeWire thread loop";
      Shutdown();
      return false;
   }
   m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
   if (m_context == nullptr)
   {
      error = "Failed to create the PipeWire context";
      Shutdown();
      return false;
   }
   m_core = pw_context_connect(m_context, nullptr, 0);
   if (m_core == nullptr)
   {
      error = std::string("Failed to connect to PipeWire: ") + std::strerror(errno);
      Shutdown();
      return false;
   }
   // The loop must be running before we can wait on either registry
   // events (process lookup) or stream state changes (connect below).
   if (pw_thread_loop_start(m_loop) < 0)
   {
      error = "Failed to start the PipeWire thread loop";
      Shutdown();
      return false;
   }
   return true;
}
bool CLoopbackCapture::FindProcessNodeTarget(pid_t rootPid, bool includeProcessTree, std::string &targetObject,
                                              std::string &error)
{
   m_lookupRootPid = rootPid;
   m_lookupIncludesProcessTree = includeProcessTree;
   m_lookupPids = includeProcessTree ? CollectProcessTree(rootPid) : std::unordered_set<pid_t>{rootPid};
   m_clientPids.clear();
   m_targetsByClientId.clear();
   m_lookupFound = false;
   m_lookupTargetObject.clear();
   pw_thread_loop_lock(m_loop);
   m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
   if (m_registry == nullptr)
   {
      pw_thread_loop_unlock(m_loop);
      error = "Failed to get the PipeWire registry";
      return false;
   }
   pw_registry_add_listener(m_registry, &m_registryListener, &kRegistryEvents, this);
   timespec deadline{};
   pw_thread_loop_get_time(m_loop, &deadline, kWaitTimeoutNanos);
   int waitResult = 0;
   while (!m_lookupFound && waitResult == 0)
   {
      waitResult = pw_thread_loop_timed_wait_full(m_loop, &deadline);
   }
   spa_hook_remove(&m_registryListener);
   pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
   m_registry = nullptr;
   pw_thread_loop_unlock(m_loop);
   if (!m_lookupFound)
   {
      error = "No active audio stream was found for the given process (or its process tree). "
              "Make sure the target application is currently playing audio.";
      return false;
   }
   targetObject = m_lookupTargetObject;
   return true;
}

bool CLoopbackCapture::PidMatchesLookup(pid_t pid) const
{
   if (pid <= 0)
   {
      return false;
   }
   if (m_lookupPids.contains(pid))
   {
      return true;
   }
   if (!m_lookupIncludesProcessTree)
   {
      return false;
   }

   // Also resolve ancestry when a registry event arrives. This covers
   // audio helper processes spawned after the initial /proc snapshot.
   std::unordered_set<pid_t> visited;
   pid_t current = pid;
   while (current > 1 && visited.insert(current).second)
   {
      const auto parent = ReadParentPid(current);
      if (!parent.has_value())
      {
         break;
      }
      if (*parent == m_lookupRootPid || m_lookupPids.contains(*parent))
      {
         return true;
      }
      current = *parent;
   }
   return false;
}

void CLoopbackCapture::ConsiderProcessTarget(pid_t pid, const std::string &targetObject)
{
   if (m_lookupFound || targetObject.empty() || !PidMatchesLookup(pid))
   {
      return;
   }

   m_lookupTargetObject = targetObject;
   m_lookupFound = true;
   if (m_loop != nullptr)
   {
      pw_thread_loop_signal(m_loop, false);
   }
}
bool CLoopbackCapture::ConnectAndWait(pw_properties *properties, const spa_pod **params, uint32_t nParams,
                                       std::string &error)
{
   pw_thread_loop_lock(m_loop);
   m_stream = pw_stream_new(m_core, "Loopback capture", properties);
   if (m_stream == nullptr)
   {
      pw_thread_loop_unlock(m_loop);
      error = "Failed to create the PipeWire capture stream";
      Shutdown();
      return false;
   }
   pw_stream_add_listener(m_stream, &m_streamListener, &kStreamEvents, this);
   const auto flags = static_cast<pw_stream_flags>(
       PW_STREAM_FLAG_AUTOCONNECT |
       PW_STREAM_FLAG_MAP_BUFFERS);
   const int connectResult = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, nParams);
   if (connectResult < 0)
   {
      pw_thread_loop_unlock(m_loop);
      error = std::string("Failed to connect the PipeWire capture stream: ") + spa_strerror(connectResult);
      Shutdown();
      return false;
   }
   const char *streamError = nullptr;
   pw_stream_state state = pw_stream_get_state(m_stream, &streamError);
   timespec deadline{};
   pw_thread_loop_get_time(m_loop, &deadline, kWaitTimeoutNanos);
   int waitResult = 0;
   while (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING &&
          state != PW_STREAM_STATE_ERROR && waitResult == 0)
   {
      waitResult = pw_thread_loop_timed_wait_full(m_loop, &deadline);
      state = pw_stream_get_state(m_stream, &streamError);
   }
   pw_thread_loop_unlock(m_loop);
   if (state == PW_STREAM_STATE_ERROR)
   {
      error = std::string("PipeWire rejected the capture stream: ") +
              (streamError != nullptr ? streamError : "unknown error");
      Shutdown();
      return false;
   }
   if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
   {
      error = "Timed out while connecting the PipeWire capture stream";
      Shutdown();
      return false;
   }
   return true;
}
bool CLoopbackCapture::StartSystemCapture(Napi::ThreadSafeFunction tsfn, std::string &error)
{
   m_tsfn = tsfn;
   m_tsfnActive = true;
   if (!InitializeCore(error))
   {
      return false;
   }
   pw_properties *properties = pw_properties_new(
       PW_KEY_MEDIA_TYPE, "Audio",
       PW_KEY_MEDIA_CATEGORY, "Capture",
       PW_KEY_MEDIA_ROLE, "Screen",
       PW_KEY_NODE_NAME, "loopback-capture",
       PW_KEY_NODE_DESCRIPTION, "Loopback Capture",
       PW_KEY_STREAM_CAPTURE_SINK, "true",
       nullptr);
   std::array<std::uint8_t, 1024> podBuffer{};
   spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer.data(), podBuffer.size());
   spa_audio_info_raw format = MakeStandardFormat();
   const spa_pod *params[] = {
       spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format),
   };
   return ConnectAndWait(properties, params, 1, error);
}
bool CLoopbackCapture::StartProcessCapture(pid_t pid, bool includeProcessTree, Napi::ThreadSafeFunction tsfn,
                                            std::string &error)
{
   m_tsfn = tsfn;
   m_tsfnActive = true;
   if (!InitializeCore(error))
   {
      return false;
   }
   std::string targetObject;
   if (!FindProcessNodeTarget(pid, includeProcessTree, targetObject, error))
   {
      Shutdown();
      return false;
   }
   pw_properties *properties = pw_properties_new(
       PW_KEY_MEDIA_TYPE, "Audio",
       PW_KEY_MEDIA_CATEGORY, "Capture",
       PW_KEY_MEDIA_ROLE, "Screen",
       PW_KEY_NODE_NAME, "process-loopback-capture",
       PW_KEY_NODE_DESCRIPTION, "Process Loopback Capture",
       PW_KEY_TARGET_OBJECT, targetObject.c_str(),
       nullptr);
   std::array<std::uint8_t, 1024> podBuffer{};
   spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer.data(), podBuffer.size());
   spa_audio_info_raw format = MakeStandardFormat();
   const spa_pod *params[] = {
       spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format),
   };
   return ConnectAndWait(properties, params, 1, error);
}
void CLoopbackCapture::StopCapture()
{
   Shutdown();
}
void CLoopbackCapture::OnStreamStateChanged(void *data, pw_stream_state, pw_stream_state, const char *)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   if (capture->m_loop != nullptr)
   {
      pw_thread_loop_signal(capture->m_loop, false);
   }
}
void CLoopbackCapture::OnStreamProcess(void *data)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   pw_buffer *pipeWireBuffer = pw_stream_dequeue_buffer(capture->m_stream);
   if (pipeWireBuffer == nullptr)
   {
      return;
   }
   spa_buffer *buffer = pipeWireBuffer->buffer;
   for (std::uint32_t i = 0; i < buffer->n_datas; ++i)
   {
      const spa_data &plane = buffer->datas[i];
      if (plane.data == nullptr || plane.chunk == nullptr)
      {
         continue;
      }
      const std::uint32_t offset = std::min(plane.chunk->offset, plane.maxsize);
      const std::uint32_t size = std::min(plane.chunk->size, plane.maxsize - offset);
      const auto *bytes = static_cast<const std::byte *>(plane.data) + offset;
      capture->EmitAudioData(bytes, size);
   }
   pw_stream_queue_buffer(capture->m_stream, pipeWireBuffer);
}
void CLoopbackCapture::OnRegistryGlobal(void *data, uint32_t id, uint32_t /*permissions*/, const char *type,
                                         uint32_t /*version*/, const spa_dict *props)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   if (capture->m_lookupFound || props == nullptr || type == nullptr)
   {
      return;
   }

   if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0)
   {
      const auto pid = ReadProcessId(props);
      if (!pid.has_value())
      {
         return;
      }

      capture->m_clientPids[id] = *pid;
      const auto targets = capture->m_targetsByClientId.equal_range(id);
      for (auto it = targets.first; it != targets.second && !capture->m_lookupFound; ++it)
      {
         capture->ConsiderProcessTarget(*pid, it->second);
      }
      return;
   }

   if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
   {
      return;
   }

   const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
   if (mediaClass == nullptr || std::strcmp(mediaClass, "Stream/Output/Audio") != 0)
   {
      return;
   }

   // Prefer object.serial: it's guaranteed unique for the lifetime of the
   // registry connection, unlike node.name which apps can duplicate.
   const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
   const char *nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
   if (serial == nullptr && nodeName == nullptr)
   {
      return;
   }

   const std::string targetObject = serial != nullptr ? serial : nodeName;

   // Some session managers copy the originating PID onto the stream node.
   // Keep that fast path, but do not require it.
   if (const auto nodePid = ReadProcessId(props); nodePid.has_value())
   {
      capture->ConsiderProcessTarget(*nodePid, targetObject);
      if (capture->m_lookupFound)
      {
         return;
      }
   }

   // Native PipeWire clients commonly expose application.process.id on the
   // Client global and only client.id on their playback Node. Correlate the
   // two, retaining the node if its Client event has not arrived yet.
   const auto clientId = ReadClientId(props);
   if (!clientId.has_value())
   {
      return;
   }

   const auto client = capture->m_clientPids.find(*clientId);
   if (client != capture->m_clientPids.end())
   {
      capture->ConsiderProcessTarget(client->second, targetObject);
   }
   else
   {
      capture->m_targetsByClientId.emplace(*clientId, targetObject);
   }
}
void CLoopbackCapture::EmitAudioData(const std::byte *data, std::size_t byteCount)
{
   if (!m_tsfnActive.load(std::memory_order_acquire) || data == nullptr || byteCount == 0)
   {
      return;
   }
   auto packet = std::make_unique<AudioDataPacket>();
   packet->data.assign(data, data + byteCount);
   const napi_status status = m_tsfn.NonBlockingCall(
       packet.get(),
       [](Napi::Env env, Napi::Function callback, AudioDataPacket *audio)
       {
          std::unique_ptr<AudioDataPacket> owned(audio);
          auto *bytes = reinterpret_cast<const std::uint8_t *>(owned->data.data());
          callback.Call({Napi::Buffer<std::uint8_t>::Copy(env, bytes, owned->data.size())});
       });
   if (status == napi_ok)
   {
      packet.release();
   }
}
void CLoopbackCapture::ReleaseThreadSafeFunction()
{
   if (m_tsfnActive.exchange(false, std::memory_order_acq_rel))
   {
      m_tsfn.Release();
   }
}
void CLoopbackCapture::Shutdown()
{
   if (m_loop != nullptr)
   {
      pw_thread_loop_stop(m_loop);
   }
   if (m_registry != nullptr)
   {
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
      m_registry = nullptr;
   }
   if (m_stream != nullptr)
   {
      spa_hook_remove(&m_streamListener);
      pw_stream_destroy(m_stream);
      m_stream = nullptr;
   }
   if (m_core != nullptr)
   {
      pw_core_disconnect(m_core);
      m_core = nullptr;
   }
   if (m_context != nullptr)
   {
      pw_context_destroy(m_context);
      m_context = nullptr;
   }
   if (m_loop != nullptr)
   {
      pw_thread_loop_destroy(m_loop);
      m_loop = nullptr;
   }
   m_lookupPids.clear();
   m_clientPids.clear();
   m_targetsByClientId.clear();
   m_lookupRootPid = 0;
   m_lookupIncludesProcessTree = false;
   m_lookupFound = false;
   m_lookupTargetObject.clear();
   ReleaseThreadSafeFunction();
}
