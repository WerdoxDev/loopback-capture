#include "ProcessTree.h"

#include <charconv>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace loopback
{
   std::optional<ProcessIdentity> ReadProcessIdentity(pid_t pid)
   {
      std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
      std::string line;
      if (!std::getline(file, line))
      {
         return std::nullopt;
      }
      // comm may contain spaces and closing parentheses.
      const auto end = line.rfind(')');
      if (end == std::string::npos)
      {
         return std::nullopt;
      }
      std::istringstream fields(line.substr(end + 1));
      char state;
      ProcessIdentity identity{};
      if (!(fields >> state >> identity.parent))
      {
         return std::nullopt;
      }
      std::string ignored;
      for (int field = 5; field < 22; ++field)
      {
         if (!(fields >> ignored))
         {
            return std::nullopt;
         }
      }
      if (!(fields >> identity.startTime))
      {
         return std::nullopt;
      }
      return identity;
   }

   bool ProcessTree::Initialize(pid_t root, std::string &error)
   {
      const auto identity = ReadProcessIdentity(root);
      if (!identity)
      {
         error = "Cannot read /proc/" + std::to_string(root) + "/stat for the target process";
         return false;
      }
      m_members.clear();
      m_members.emplace(root, identity->startTime);
      Refresh();
      return true;
   }

   bool ProcessTree::Contains(pid_t pid)
   {
      std::vector<std::pair<pid_t, std::uint64_t>> ancestry;
      std::unordered_set<pid_t> visited;
      while (pid > 0 && visited.insert(pid).second)
      {
         const auto identity = ReadProcessIdentity(pid);
         if (!identity)
         {
            return false;
         }
         const auto member = m_members.find(pid);
         if (member != m_members.end() && member->second == identity->startTime)
         {
            for (const auto &[child, startTime] : ancestry)
            {
               m_members[child] = startTime;
            }
            return true;
         }
         ancestry.emplace_back(pid, identity->startTime);
         pid = identity->parent;
      }
      return false;
   }

   void ProcessTree::Refresh()
   {
      DIR *directory = opendir("/proc");
      if (directory == nullptr)
      {
         return;
      }
      while (const dirent *entry = readdir(directory))
      {
         const std::string name(entry->d_name);
         pid_t pid = 0;
         const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), pid);
         if (error == std::errc{} && end == name.data() + name.size() && pid > 0)
         {
            Contains(pid);
         }
      }
      closedir(directory);
   }
} // namespace loopback
