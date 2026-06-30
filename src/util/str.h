#pragma once
#include <string>

namespace motion {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);

}
