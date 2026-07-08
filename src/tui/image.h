#pragma once
#include <string>
#include <vector>

namespace motion::tui {

bool renderImage(const std::wstring& path, int maxCols, int maxRows, std::string& out);
bool renderImageFromMemory(const std::vector<unsigned char>& data, int maxCols, int maxRows, std::string& out);

}
