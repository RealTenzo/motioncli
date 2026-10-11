#pragma once
#include <string>

namespace motion {

struct OptimizeStats {
    int inW = 0;
    int inH = 0;
    int outW = 0;
    int outH = 0;
    long long outBytes = 0;
    double seconds = 0.0;
};

bool needsOptimize(const std::wstring& path);
int targetHeight();
bool optimizeVideo(const std::wstring& src, const std::wstring& dest,
                   void(*onProgress)(int, void*), void* ctx,
                   OptimizeStats& stats, std::string& err, std::string& note);
void pointConfigAtMedia(const std::wstring& oldPath, const std::wstring& newPath);
int runOptimize();

}
