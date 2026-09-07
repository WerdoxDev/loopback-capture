#pragma once

#include <windows.h>
#include <iomanip>
#include <sstream>
#include <string>

inline std::string HResultToString(HRESULT hr)
{
   std::ostringstream oss;
   oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
       << static_cast<unsigned long>(hr);
   return oss.str();
}
