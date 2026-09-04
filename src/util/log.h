#pragma once
#include <string>

namespace motion::log {

void init(bool truncate = true);
void write(const char* level, const std::string& msg);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);
std::wstring primaryLogPath();
std::wstring appDataLogPath();

}
