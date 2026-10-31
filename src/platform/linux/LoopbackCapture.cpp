#include "LoopbackCapture.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <vector>

#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

namespace
{
   using Clock = std::chrono::steady_clock;
   constexpr std::int64_t kWaitTimeoutNanos = 5'000'000'000LL;
   constexpr std::array<spa_audio_channel, 8> kChannels = {
       SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
       SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR};

   struct AudioDataPacket
   {
      std::vector<std::byte> data;
   };

   std::optional<std::uint32_t> ReadId(const spa_dict *props, const char *key)
   {
      const char *value = props ? spa_dict_lookup(props, key) : nullptr;
      if (value == nullptr)
      {
         return std::nullopt;
      }
      std::uint32_t id;
      const auto *end = value + std::strlen(value);
      const auto result = std::from_chars(value, end, id);
      return result.ec == std::errc{} && result.ptr == end ? std::optional(id) : std::nullopt;
   }

   std::optional<pid_t> ReadPid(const spa_dict *props, const char *key)
   {
      const auto id = ReadId(props, key);
      if (!id || *id == 0 || *id > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max()))
      {
         return std::nullopt;
      }
      return static_cast<pid_t>(*id);
   }

   bool IsPulseServer(const spa_dict *props)
   {
      const char *binary = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
      if (binary && std::strcmp(binary, "pipewire-pulse") == 0)
      {
         return true;
      }
      if (const auto pid = ReadPid(props, PW_KEY_SEC_PID))
      {
         std::ifstream file("/proc/" + std::to_string(*pid) + "/comm");
         std::string name;
         std::getline(file, name);
         return name == "pipewire-pulse";
      }
      return false;
   }

   // Match graph channels before downmixing. Mono is copied to both front
   // channels; surround channels remain separate until the final conversion.
   bool ChannelsMatch(const std::string &output, const std::string &input)
   {
      if (output == input)
      {
         return true;
      }
      if (output == "MONO")
      {
         return input == "FL" || input == "FR";
      }
      if (output == "FLC" || output == "AUX0")
         return input == "FL";
      if (output == "FRC" || output == "AUX1")
         return input == "FR";
      if (output == "RC")
         return input == "FC";
      if (output == "LFE2")
         return input == "LFE";
      return false;
   }

   std::int16_t ToPcm16(float sample)
   {
      if (!std::isfinite(sample))
         return 0;
      return static_cast<std::int16_t>(
          std::clamp(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32768.0f), -32768L, 32767L));
   }
} // namespace

struct CLoopbackCapture::Client
{
   CLoopbackCapture *owner;
   pw_client *proxy = nullptr;
   pw_properties *properties = nullptr;
   spa_hook listener{};
   bool pulseServer = false;
   bool infoReceived = false;
   ~Client()
   {
      if (proxy)
      {
         spa_hook_remove(&listener);
         pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy));
      }
      if (properties)
         pw_properties_free(properties);
   }
};

struct CLoopbackCapture::Node
{
   CLoopbackCapture *owner;
   std::uint32_t id;
   pw_node *proxy = nullptr;
   pw_properties *properties = nullptr;
   spa_hook listener{};
   Clock::time_point discovered = Clock::now();
   bool unknownReported = false;
   ~Node()
   {
      if (proxy)
      {
         spa_hook_remove(&listener);
         pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy));
      }
      if (properties)
         pw_properties_free(properties);
   }
};

struct CLoopbackCapture::Link
{
   pw_proxy *proxy = nullptr;
   spa_hook listener{};
   bool failed = false;
   Clock::time_point retryAfter{};
   ~Link()
   {
      if (proxy)
      {
         spa_hook_remove(&listener);
         pw_proxy_destroy(proxy);
      }
   }
};

CLoopbackCapture::CLoopbackCapture() = default;
CLoopbackCapture::~CLoopbackCapture()
{
   Shutdown();
}

bool CLoopbackCapture::StartSystemCapture(Napi::ThreadSafeFunction tsfn, std::string &error,
                                          std::optional<pid_t> excludeProcessId)
{
   m_mode = excludeProcessId ? CaptureMode::ExcludingProcess : CaptureMode::System;
   if (excludeProcessId && !m_processTree.Initialize(*excludeProcessId, error))
   {
      tsfn.Release();
      return false;
   }
   return Start(tsfn, error);
}

bool CLoopbackCapture::StartProcessCapture(pid_t pid, Napi::ThreadSafeFunction tsfn, std::string &error)
{
   m_mode = CaptureMode::Process;
   if (!m_processTree.Initialize(pid, error))
   {
      tsfn.Release();
      return false;
   }
   return Start(tsfn, error);
}

bool CLoopbackCapture::Start(Napi::ThreadSafeFunction tsfn, std::string &error)
{
   m_starting = true;
   m_tsfn = tsfn;
   m_tsfnActive = true;
   static std::once_flag initialized;
   std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
   m_loop = pw_thread_loop_new("loopback-capture", nullptr);
   if (!m_loop)
   {
      error = "Failed to create the PipeWire thread loop";
      Shutdown();
      return false;
   }
   auto *loop = pw_thread_loop_get_loop(m_loop);
   m_context = pw_context_new(loop, nullptr, 0);
   if (!m_context)
   {
      error = "Failed to create the PipeWire context";
      Shutdown();
      return false;
   }
   if (!Connect(error))
   {
      Shutdown();
      return false;
   }
   m_timer = pw_loop_add_timer(loop, OnTimer, this);
   if (!m_timer)
   {
      error = "Failed to create the PipeWire discovery timer";
      Shutdown();
      return false;
   }
   timespec interval{0, 250'000'000};
   pw_loop_update_timer(loop, m_timer, &interval, &interval, false);

   // Hold the loop lock before starting it so startup state changes cannot
   // race the predicate used by the wait below.
   pw_thread_loop_lock(m_loop);
   if (pw_thread_loop_start(m_loop) < 0)
   {
      pw_thread_loop_unlock(m_loop);
      error = "Failed to start the PipeWire thread loop";
      Shutdown();
      return false;
   }
   m_loopStarted = true;
   timespec deadline{};
   pw_thread_loop_get_time(m_loop, &deadline, kWaitTimeoutNanos);
   int waitResult = 0;
   auto state = pw_stream_get_state(m_stream, nullptr);
   while (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING && m_connectionError.empty() &&
          waitResult == 0)
   {
      waitResult = pw_thread_loop_timed_wait_full(m_loop, &deadline);
      state = pw_stream_get_state(m_stream, nullptr);
   }
   const bool ready =
       m_connectionError.empty() && (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING);
   error = m_connectionError.empty() ? "Timed out while connecting to PipeWire" : m_connectionError;
   m_starting = false;
   pw_thread_loop_unlock(m_loop);
   if (!ready)
   {
      Shutdown();
      return false;
   }
   error.clear();
   return true;
}

bool CLoopbackCapture::Connect(std::string &error)
{
   m_connectStarted = Clock::now();
   m_connectionError.clear();
   m_reconnect = false;
   m_core = pw_context_connect(m_context, nullptr, 0);
   if (!m_core)
   {
      error = "Failed to connect to PipeWire: " + std::string(std::strerror(errno));
      return false;
   }
   static const pw_core_events coreEvents = [] {
      pw_core_events events{};
      events.version = PW_VERSION_CORE_EVENTS;
      events.error = OnCoreError;
      return events;
   }();
   pw_core_add_listener(m_core, &m_coreListener, &coreEvents, this);

   const bool filtered = m_mode != CaptureMode::System;
   if (filtered)
   {
      m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
      if (!m_registry)
      {
         error = "Failed to get the PipeWire registry";
         return false;
      }
      static const pw_registry_events registryEvents = [] {
         pw_registry_events events{};
         events.version = PW_VERSION_REGISTRY_EVENTS;
         events.global = OnRegistryGlobal;
         events.global_remove = OnRegistryRemoved;
         return events;
      }();
      pw_registry_add_listener(m_registry, &m_registryListener, &registryEvents, this);
   }

   auto *properties =
       pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
                         PW_KEY_NODE_NAME, "loopback-capture", PW_KEY_NODE_DESCRIPTION, "Loopback Capture", nullptr);
   if (filtered)
   {
      // Explicitly link selected playback ports. Never let the session manager
      // connect this stream to a default microphone or an unfiltered sink.
      pw_properties_set(properties, PW_KEY_NODE_AUTOCONNECT, "false");
      pw_properties_set(properties, PW_KEY_NODE_ALWAYS_PROCESS, "true");
      pw_properties_set(properties, "adapter.auto-port-config", "{ mode = dsp monitor = false position = preserve }");
   }
   else
   {
      pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true");
   }
   m_stream = pw_stream_new(m_core, "Loopback capture", properties);
   if (!m_stream)
   {
      error = "Failed to create the PipeWire capture stream";
      return false;
   }
   static const pw_stream_events streamEvents = [] {
      pw_stream_events events{};
      events.version = PW_VERSION_STREAM_EVENTS;
      events.state_changed = OnStreamStateChanged;
      events.process = OnStreamProcess;
      return events;
   }();
   pw_stream_add_listener(m_stream, &m_streamListener, &streamEvents, this);
   spa_audio_info_raw format{};
   format.format = filtered ? SPA_AUDIO_FORMAT_F32 : SPA_AUDIO_FORMAT_S16_LE;
   format.rate = 48000;
   format.channels = filtered ? kChannels.size() : 2;
   std::copy_n(kChannels.begin(), format.channels, format.position);
   std::array<std::uint8_t, 1024> storage{};
   spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
   const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
   const auto flags =
       static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | (filtered ? 0 : PW_STREAM_FLAG_AUTOCONNECT));
   const int result = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1);
   if (result < 0)
   {
      error = "Failed to connect the PipeWire capture stream: " + std::string(spa_strerror(result));
      return false;
   }
   return true;
}

void CLoopbackCapture::OnStreamStateChanged(void *data, pw_stream_state, pw_stream_state state, const char *error)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
   {
      capture->m_streamNodeId = pw_stream_get_node_id(capture->m_stream);
      capture->RefreshLinks();
   }
   else if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
   {
      capture->m_connectionError = error ? error : "PipeWire capture disconnected";
      capture->m_reconnect = true;
   }
   pw_thread_loop_signal(capture->m_loop, false);
}

void CLoopbackCapture::OnCoreError(void *data, uint32_t id, int, int res, const char *message)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   if (id == PW_ID_CORE && res < 0)
   {
      capture->m_connectionError = message ? message : "PipeWire server disconnected";
      capture->m_reconnect = true;
      pw_thread_loop_signal(capture->m_loop, false);
   }
}

void CLoopbackCapture::OnTimer(void *data, uint64_t)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   if (capture->m_starting)
      return;
   const auto now = Clock::now();
   if (capture->m_stream && now - capture->m_connectStarted > std::chrono::seconds(5))
   {
      const auto state = pw_stream_get_state(capture->m_stream, nullptr);
      if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
         capture->m_reconnect = true;
   }
   if (capture->m_reconnect && now >= capture->m_nextReconnect)
   {
      capture->Disconnect();
      std::string error;
      if (!capture->Connect(error))
      {
         capture->Disconnect();
         capture->m_reconnect = true;
      }
      capture->m_nextReconnect = now + std::chrono::seconds(1);
   }
   if (capture->m_mode != CaptureMode::System && now >= capture->m_nextTreeRefresh)
   {
      capture->m_processTree.Refresh();
      capture->m_nextTreeRefresh = now + std::chrono::seconds(1);
   }
   capture->RefreshLinks();
}

void CLoopbackCapture::OnRegistryGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t version,
                                        const spa_dict *props)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   if (!props || !type)
      return;
   if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0)
   {
      auto client = std::make_unique<Client>();
      client->owner = capture;
      client->properties = pw_properties_new_dict(props);
      client->proxy = static_cast<pw_client *>(pw_registry_bind(
          capture->m_registry, id, type, std::min(version, static_cast<uint32_t>(PW_VERSION_CLIENT)), 0));
      if (!client->proxy)
         return;
      static const pw_client_events events = [] {
         pw_client_events value{};
         value.version = PW_VERSION_CLIENT_EVENTS;
         value.info = OnClientInfo;
         return value;
      }();
      auto *entry = client.get();
      capture->m_clients.emplace(id, std::move(client));
      pw_client_add_listener(entry->proxy, &entry->listener, &events, entry);
   }
   else if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
   {
      const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
      if (!mediaClass || std::strcmp(mediaClass, "Stream/Output/Audio") != 0)
         return;
      auto node = std::make_unique<Node>();
      node->owner = capture;
      node->id = id;
      node->properties = pw_properties_new_dict(props);
      node->proxy = static_cast<pw_node *>(pw_registry_bind(
          capture->m_registry, id, type, std::min(version, static_cast<uint32_t>(PW_VERSION_NODE)), 0));
      if (!node->proxy)
         return;
      static const pw_node_events events = [] {
         pw_node_events value{};
         value.version = PW_VERSION_NODE_EVENTS;
         value.info = OnNodeInfo;
         return value;
      }();
      auto *entry = node.get();
      capture->m_nodes.emplace(id, std::move(node));
      pw_node_add_listener(entry->proxy, &entry->listener, &events, entry);
   }
   else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
   {
      const auto node = ReadId(props, PW_KEY_NODE_ID);
      const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
      const char *format = spa_dict_lookup(props, PW_KEY_FORMAT_DSP);
      const char *channel = spa_dict_lookup(props, "audio.channel");
      const char *monitor = spa_dict_lookup(props, PW_KEY_PORT_MONITOR);
      if (monitor && std::strcmp(monitor, "true") == 0)
         return;
      if (!node || !direction || !channel || !format || std::strcmp(format, "32 bit float mono audio") != 0)
         return;
      capture->m_ports[id] = Port{*node, channel, std::strcmp(direction, "in") == 0};
   }
   capture->RefreshLinks();
}

void CLoopbackCapture::OnNodeInfo(void *data, const pw_node_info *info)
{
   auto *node = static_cast<Node *>(data);
   if (info->props)
      pw_properties_update(node->properties, info->props);
   node->owner->RefreshLinks();
}

void CLoopbackCapture::OnClientInfo(void *data, const pw_client_info *info)
{
   auto *client = static_cast<Client *>(data);
   client->infoReceived = true;
   if (info->props)
      pw_properties_update(client->properties, info->props);
   client->pulseServer = IsPulseServer(&client->properties->dict);
   client->owner->RefreshLinks();
}

void CLoopbackCapture::OnRegistryRemoved(void *data, uint32_t id)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   capture->m_ports.erase(id);
   capture->m_nodes.erase(id);
   capture->m_clients.erase(id);
   capture->RefreshLinks();
}

std::optional<pid_t> CLoopbackCapture::GetOwnerPid(const Node &node) const
{
   // Node info contains originating application metadata that the registry's
   // abbreviated global properties often omit (notably for pipewire-pulse).
   const auto id = ReadId(&node.properties->dict, PW_KEY_CLIENT_ID);
   const auto client = id ? m_clients.find(*id) : m_clients.end();
   if (client == m_clients.end() || !client->second->infoReceived)
      return std::nullopt;
   const auto *props = &client->second->properties->dict;
   if (const auto pid = ReadPid(&node.properties->dict, PW_KEY_APP_PROCESS_ID))
   {
      if (client->second->pulseServer && pid == ReadPid(props, PW_KEY_SEC_PID))
         return std::nullopt;
      return pid;
   }
   if (client->second->pulseServer)
      return std::nullopt;
   if (const auto pid = ReadPid(props, PW_KEY_SEC_PID))
      return pid;
   return ReadPid(props, PW_KEY_APP_PROCESS_ID);
}

void CLoopbackCapture::RefreshLinks()
{
   if (m_mode == CaptureMode::System || m_streamNodeId == PW_ID_ANY || !m_core || m_reconnect)
      return;
   std::set<std::uint32_t> selected;
   for (auto &[id, node] : m_nodes)
   {
      const auto pid = GetOwnerPid(*node);
      if (!pid || !loopback::ReadProcessIdentity(*pid))
      {
         if (!node->unknownReported && Clock::now() - node->discovered > std::chrono::seconds(2))
         {
            std::cerr << "loopback-capture: cannot identify the owner of playback node " << id
                      << "; skipping it during filtered capture\n";
            node->unknownReported = true;
         }
         continue;
      }
      const bool inTree = m_processTree.Contains(*pid);
      if (inTree == (m_mode == CaptureMode::Process))
         selected.insert(id);
   }
   std::set<LinkKey> desired;
   for (const auto &[outputId, output] : m_ports)
   {
      if (output.input || !selected.contains(output.node))
         continue;
      for (const auto &[inputId, input] : m_ports)
      {
         if (input.input && input.node == m_streamNodeId && ChannelsMatch(output.channel, input.channel))
            desired.emplace(outputId, inputId);
      }
   }
   for (auto it = m_links.begin(); it != m_links.end();)
   {
      if (!desired.contains(it->first))
         it = m_links.erase(it);
      else
         ++it;
   }
   static const pw_proxy_events linkEvents = [] {
      pw_proxy_events events{};
      events.version = PW_VERSION_PROXY_EVENTS;
      events.error = OnLinkError;
      events.removed = OnLinkRemoved;
      return events;
   }();
   for (const auto &key : desired)
   {
      const auto existing = m_links.find(key);
      if (existing != m_links.end())
      {
         if (!existing->second->failed || Clock::now() < existing->second->retryAfter)
            continue;
         m_links.erase(existing);
      }
      auto link = std::make_unique<Link>();
      const auto output = std::to_string(key.first);
      const auto input = std::to_string(key.second);
      auto *properties = pw_properties_new(PW_KEY_LINK_OUTPUT_PORT, output.c_str(), PW_KEY_LINK_INPUT_PORT,
                                           input.c_str(), PW_KEY_OBJECT_LINGER, "false", nullptr);
      link->proxy = static_cast<pw_proxy *>(
          pw_core_create_object(m_core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &properties->dict, 0));
      pw_properties_free(properties);
      if (link->proxy)
         pw_proxy_add_listener(link->proxy, &link->listener, &linkEvents, link.get());
      else
      {
         link->failed = true;
         link->retryAfter = Clock::now() + std::chrono::seconds(1);
      }
      m_links.emplace(key, std::move(link));
   }
}

void CLoopbackCapture::OnLinkError(void *data, int, int, const char *message)
{
   auto *link = static_cast<Link *>(data);
   link->failed = true;
   link->retryAfter = Clock::now() + std::chrono::seconds(1);
   std::cerr << "loopback-capture: playback link failed: " << (message ? message : "unknown error") << '\n';
}

void CLoopbackCapture::OnLinkRemoved(void *data)
{
   auto *link = static_cast<Link *>(data);
   link->failed = true;
   link->retryAfter = Clock::now() + std::chrono::seconds(1);
}

void CLoopbackCapture::OnStreamProcess(void *data)
{
   auto *capture = static_cast<CLoopbackCapture *>(data);
   // Without RT_PROCESS this callback runs on the same loop as registry
   // updates. PipeWire mixes and resamples linked DSP ports to 48 kHz here.
   while (auto *packet = pw_stream_dequeue_buffer(capture->m_stream))
   {
      const auto *buffer = packet->buffer;
      if (buffer->n_datas > 0)
      {
         const auto &plane = buffer->datas[0];
         if (plane.data && plane.chunk && plane.maxsize > 0)
         {
            const auto offset = std::min(plane.chunk->offset, plane.maxsize);
            const auto size = std::min(plane.chunk->size, plane.maxsize - offset);
            const auto *bytes = static_cast<const std::byte *>(plane.data) + offset;
            if (capture->m_mode == CaptureMode::System)
               capture->EmitAudioData(bytes, size - size % 4);
            else
            {
               constexpr std::size_t stride = kChannels.size() * sizeof(float);
               const auto frames = size / stride;
               std::vector<std::int16_t> pcm(frames * 2);
               for (std::size_t frame = 0; frame < frames; ++frame)
               {
                  std::array<float, kChannels.size()> samples;
                  std::memcpy(samples.data(), bytes + frame * stride, stride);
                  constexpr float surround = 0.70710678f;
                  const float center = samples[2] * surround + samples[3] * 0.5f;
                  pcm[frame * 2] = ToPcm16(samples[0] + center + surround * (samples[4] + samples[6]));
                  pcm[frame * 2 + 1] = ToPcm16(samples[1] + center + surround * (samples[5] + samples[7]));
               }
               capture->EmitAudioData(reinterpret_cast<const std::byte *>(pcm.data()), pcm.size() * sizeof(pcm[0]));
            }
         }
      }
      pw_stream_queue_buffer(capture->m_stream, packet);
   }
}

void CLoopbackCapture::EmitAudioData(const std::byte *data, std::size_t byteCount)
{
   if (!m_tsfnActive.load(std::memory_order_acquire) || !data || byteCount == 0)
      return;
   auto packet = std::make_unique<AudioDataPacket>();
   packet->data.assign(data, data + byteCount);
   const auto status =
       m_tsfn.NonBlockingCall(packet.get(), [](Napi::Env env, Napi::Function callback, AudioDataPacket *audio) {
          std::unique_ptr<AudioDataPacket> owned(audio);
          callback.Call({Napi::Buffer<std::uint8_t>::Copy(
              env, reinterpret_cast<const std::uint8_t *>(owned->data.data()), owned->data.size())});
       });
   if (status == napi_ok)
      packet.release();
}

void CLoopbackCapture::ReleaseThreadSafeFunction()
{
   if (m_tsfnActive.exchange(false, std::memory_order_acq_rel))
      m_tsfn.Release();
}

void CLoopbackCapture::Disconnect()
{
   m_streamNodeId = PW_ID_ANY;
   m_links.clear();
   m_ports.clear();
   m_nodes.clear();
   m_clients.clear();
   if (m_registry)
   {
      spa_hook_remove(&m_registryListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
      m_registry = nullptr;
   }
   if (m_stream)
   {
      spa_hook_remove(&m_streamListener);
      pw_stream_destroy(m_stream);
      m_stream = nullptr;
   }
   if (m_core)
   {
      spa_hook_remove(&m_coreListener);
      pw_core_disconnect(m_core);
      m_core = nullptr;
   }
}

void CLoopbackCapture::StopCapture()
{
   Shutdown();
}

void CLoopbackCapture::Shutdown()
{
   if (m_loopStarted)
   {
      pw_thread_loop_stop(m_loop);
      m_loopStarted = false;
   }
   m_starting = false;
   if (m_timer)
   {
      pw_loop_destroy_source(pw_thread_loop_get_loop(m_loop), m_timer);
      m_timer = nullptr;
   }
   Disconnect();
   if (m_context)
   {
      pw_context_destroy(m_context);
      m_context = nullptr;
   }
   if (m_loop)
   {
      pw_thread_loop_destroy(m_loop);
      m_loop = nullptr;
   }
   ReleaseThreadSafeFunction();
}
