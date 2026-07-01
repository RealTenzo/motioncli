#pragma once
#include <string>
#include <vector>

namespace motion::http {

using ProgressFn = void(*)(unsigned long long received, unsigned long long total, void* ctx);

bool getString(const std::wstring& url, std::string& outBody, std::string& err,
               const std::wstring& extraHeaders = L"");

bool downloadFile(const std::wstring& url,
                  const std::wstring& destPath,
                  ProgressFn onProgress,
                  void* progressCtx,
                  std::string& err);

bool getBytes(const std::wstring& url, std::vector<unsigned char>& out,
              std::string& err, const std::wstring& extraHeaders = L"");

}
