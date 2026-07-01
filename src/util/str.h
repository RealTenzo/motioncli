#pragma once
#include <string>

namespace motion {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);
std::string urlEncode(const std::string& s);
std::string htmlDecode(const std::string& s);
std::string trim(const std::string& s);
bool fileExists(const std::wstring& path);
bool endsWith(const std::string& s, const std::string& suf);

}