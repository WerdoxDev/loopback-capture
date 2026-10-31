#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_map>

#include <napi.h>
#include <pipewire/pipewire.h>

#include "ProcessTree.h"

class CLoopbackCapture
{
 public:
   CLoopbackCapture();
   ~CLoopbackCapture();
   CLoopbackCapture(const CLoopbackCapture &) = delete;
   CLoopbackCapture &operator=(const CLoopbackCapture &) = delete;

   bool StartSystemCapture(Napi::ThreadSafeFunction tsfn, std::string &error,
                           std::optional<pid_t> excludeProcessId = std::nullopt);
   bool StartProcessCapture(pid_t pid, Napi::ThreadSafeFunction tsfn, std::string &error);
   void StopCapture();

 private:
   struct Client;
   struct Node;
   struct Port
   {
      std::uint32_t node;
      std::string channel;
      bool input;
   };
   struct Link;
   using LinkKey = std::pair<std::uint32_t, std::uint32_t>;

   enum class CaptureMode
   {
      System,
      Process,
      ExcludingProcess
   };
   bool Start(Napi::ThreadSafeFunction tsfn, std::string &error);
   bool Connect(std::string &error);
   void Disconnect();
   void RefreshLinks();
   std::optional<pid_t> GetOwnerPid(const Node &node) const;
   void EmitAudioData(const std::byte *data, std::size_t byteCount);
   void ReleaseThreadSafeFunction();
   void Shutdown();

   static void OnStreamStateChanged(void *data, pw_stream_state oldState, pw_stream_state state, const char *error);
   static void OnStreamProcess(void *data);
   static void OnRegistryGlobal(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
                                const spa_dict *props);
   static void OnRegistryRemoved(void *data, uint32_t id);
   static void OnNodeInfo(void *data, const pw_node_info *info);
   static void OnClientInfo(void *data, const pw_client_info *info);
   static void OnLinkError(void *data, int seq, int res, const char *message);
   static void OnLinkRemoved(void *data);
   static void OnCoreError(void *data, uint32_t id, int seq, int res, const char *message);
   static void OnTimer(void *data, uint64_t expirations);

   CaptureMode m_mode = CaptureMode::System;
   loopback::ProcessTree m_processTree;
   pw_thread_loop *m_loop = nullptr;
   bool m_loopStarted = false;
   pw_context *m_context = nullptr;
   pw_core *m_core = nullptr;
   pw_stream *m_stream = nullptr;
   pw_registry *m_registry = nullptr;
   spa_source *m_timer = nullptr;
   spa_hook m_coreListener{};
   spa_hook m_streamListener{};
   spa_hook m_registryListener{};
   std::unordered_map<std::uint32_t, std::unique_ptr<Client>> m_clients;
   std::unordered_map<std::uint32_t, std::unique_ptr<Node>> m_nodes;
   std::unordered_map<std::uint32_t, Port> m_ports;
   std::map<LinkKey, std::unique_ptr<Link>> m_links;
   std::uint32_t m_streamNodeId = PW_ID_ANY;
   std::string m_connectionError;
   bool m_reconnect = false;
   bool m_starting = false;
   std::chrono::steady_clock::time_point m_connectStarted{};
   std::chrono::steady_clock::time_point m_nextReconnect{};
   std::chrono::steady_clock::time_point m_nextTreeRefresh{};
   Napi::ThreadSafeFunction m_tsfn;
   std::atomic<bool> m_tsfnActive{false};
};
