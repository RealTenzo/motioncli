#pragma once

#include "tui/terminal.h"
#include <string>
#include <vector>

namespace motion::tui {

bool renderImage(const std::wstring& path, int maxCols, int maxRows, std::string& out);
bool renderImageFromMemory(const std::vector<unsigned char>& data, int maxCols, int maxRows, std::string& out);
bool playVideoInConsole(const std::wstring& url, int maxCols, int maxRows, const Terminal& term);

}
