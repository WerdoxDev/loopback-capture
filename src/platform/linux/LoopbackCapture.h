#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <napi.h>
#include <pipewire/pipewire.h>
class CLoopbackCapture
{
public:
   CLoopbackCapture() = default;
   ~CLoopbackCapture();
   CLoopbackCapture(const CLoopbackCapture &) = delete;
   CLoopbackCapture &operator=(const CLoopbackCapture &) = delete;
   bool StartSystemCapture(Napi::ThreadSafeFunction tsfn, std::string &error);
   bool StartProcessCapture(pid_t pid, bool includeProcessTree, Napi::ThreadSafeFunction tsfn, std::string &error);
   void StopCapture();
   static void OnStreamStateChanged(void *data, pw_stream_state oldState, pw_stream_state state, const char *error);
   static void OnStreamProcess(void *data);
   static void OnRegistryGlobal(void *data, uint32_t id, uint32_t permissions, const char *type,
                                 uint32_t version, const spa_dict *props);
private:
   bool InitializeCore(std::string &error);
   bool FindProcessNodeTarget(pid_t rootPid, bool includeProcessTree, std::string &targetObject,
                              std::string &error);
   bool ConnectAndWait(pw_properties *properties, const spa_pod **params, uint32_t nParams, std::string &error);
   bool PidMatchesLookup(pid_t pid) const;
   void ConsiderProcessTarget(pid_t pid, const std::string &targetObject);
   void EmitAudioData(const std::byte *data, std::size_t byteCount);
   void ReleaseThreadSafeFunction();
   void Shutdown();
   pw_thread_loop *m_loop = nullptr;
   pw_context *m_context = nullptr;
   pw_core *m_core = nullptr;
   pw_stream *m_stream = nullptr;
   pw_registry *m_registry = nullptr;
   spa_hook m_streamListener{};
   spa_hook m_registryListener{};
   Napi::ThreadSafeFunction m_tsfn;
   std::atomic<bool> m_tsfnActive{false};
   // Used only while FindProcessNodeTarget's registry listener is active.
   std::unordered_set<pid_t> m_lookupPids;
   std::unordered_map<std::uint32_t, pid_t> m_clientPids;
   std::unordered_multimap<std::uint32_t, std::string> m_targetsByClientId;
   pid_t m_lookupRootPid = 0;
   bool m_lookupIncludesProcessTree = false;
   bool m_lookupFound = false;
   std::string m_lookupTargetObject;
};
