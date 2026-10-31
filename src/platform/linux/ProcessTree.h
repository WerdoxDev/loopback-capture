#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_map>

namespace loopback
{
   struct ProcessIdentity
   {
      pid_t parent;
      std::uint64_t startTime;
   };

   std::optional<ProcessIdentity> ReadProcessIdentity(pid_t pid);

   // Start times prevent PID reuse from admitting unrelated processes.
   // Remember discovered descendants even after they are reparented.
   class ProcessTree
   {
    public:
      bool Initialize(pid_t root, std::string &error);
      void Refresh();
      bool Contains(pid_t pid);

    private:
      std::unordered_map<pid_t, std::uint64_t> m_members;
   };
} // namespace loopback
