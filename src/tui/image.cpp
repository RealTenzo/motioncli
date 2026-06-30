#include "tui/image.h"

#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <vector>

namespace motion::tui {

bool renderImage(const std::wstring& path, int maxCols, int maxRows, std::string& out) {
    out.clear();
    if (maxCols < 1) maxCols = 1;
    if (maxRows < 1) maxRows = 1;

    bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) && factory) {
        IWICBitmapDecoder* dec = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &dec)) && dec) {
            IWICBitmapFrameDecode* frame = nullptr;
            if (SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
                UINT sw = 0, sh = 0;
                frame->GetSize(&sw, &sh);
                if (sw && sh) {
                    int cols = maxCols;
                    int rows = (int)((double)sh / sw * cols / 2.0 + 0.5);
                    if (rows < 1) rows = 1;
                    if (rows > maxRows) {
                        rows = maxRows;
                        cols = (int)((double)sw / sh * rows * 2.0 + 0.5);
                        if (cols < 1) cols = 1;
                        if (cols > maxCols) cols = maxCols;
                    }
                    UINT pw = (UINT)cols, ph = (UINT)rows * 2;

                    IWICBitmapScaler* scaler = nullptr;
                    IWICFormatConverter* conv = nullptr;
                    if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && scaler &&
                        SUCCEEDED(scaler->Initialize(frame, pw, ph,
                                      WICBitmapInterpolationModeHighQualityCubic)) &&
                        SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv &&
                        SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom))) {
                        static std::vector<unsigned char> px;
                        px.resize((size_t)pw * ph * 4);
                        if (SUCCEEDED(conv->CopyPixels(nullptr, pw * 4,
                                          (UINT)px.size(), px.data()))) {
                            char buf[64];
                            for (UINT y = 0; y + 1 < ph; y += 2) {
                                out += "  ";
                                for (UINT x = 0; x < pw; ++x) {
                                    const unsigned char* t = &px[((size_t)y * pw + x) * 4];
                                    const unsigned char* b = &px[((size_t)(y + 1) * pw + x) * 4];
                                    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                                        "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm\xe2\x96\x80",
                                        t[2], t[1], t[0], b[2], b[1], b[0]);
                                    out += buf;
                                }
                                out += "\x1b[0m\r\n";
                            }
                        }
                    }
                    if (conv) conv->Release();
                    if (scaler) scaler->Release();
                }
                frame->Release();
            }
            dec->Release();
        }
        factory->Release();
    }
    if (comInit) CoUninitialize();
    return !out.empty();
}

}
