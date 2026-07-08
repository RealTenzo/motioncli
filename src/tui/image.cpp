#include "tui/image.h"

#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <vector>

namespace motion::tui {

namespace {

struct FrameGuard {
    IWICBitmapFrameDecode* f = nullptr;
    ~FrameGuard() { if (f) f->Release(); }
    explicit FrameGuard(IWICBitmapFrameDecode* p) : f(p) {}
};

struct ScalerGuard {
    IWICBitmapScaler* s = nullptr;
    ~ScalerGuard() { if (s) s->Release(); }
    explicit ScalerGuard(IWICBitmapScaler* p) : s(p) {}
};

struct ConvGuard {
    IWICFormatConverter* c = nullptr;
    ~ConvGuard() { if (c) c->Release(); }
    explicit ConvGuard(IWICFormatConverter* p) : c(p) {}
};

IWICImagingFactory* getFactory() {
    static IWICImagingFactory* factory = nullptr;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&factory));
    }
    return factory;
}

bool renderFromDecoder(IWICImagingFactory* factory, IWICBitmapDecoder* dec,
                       int maxCols, int maxRows, std::string& out) {
    IWICBitmapFrameDecode* rawFrame = nullptr;
    if (!dec || FAILED(dec->GetFrame(0, &rawFrame)) || !rawFrame) return false;
    FrameGuard frame(rawFrame);

    UINT sw = 0, sh = 0;
    frame.f->GetSize(&sw, &sh);
    if (!sw || !sh) return false;

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

    IWICBitmapScaler* rawScaler = nullptr;
    if (FAILED(factory->CreateBitmapScaler(&rawScaler)) || !rawScaler) return false;
    ScalerGuard scaler(rawScaler);

    if (FAILED(scaler.s->Initialize(frame.f, pw, ph,
                    WICBitmapInterpolationModeHighQualityCubic)))
        return false;

    IWICFormatConverter* rawConv = nullptr;
    if (FAILED(factory->CreateFormatConverter(&rawConv)) || !rawConv) return false;
    ConvGuard conv(rawConv);

    if (FAILED(conv.c->Initialize(scaler.s, GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeCustom)))
        return false;

    static std::vector<unsigned char> px;
    px.resize((size_t)pw * ph * 4);
    if (FAILED(conv.c->CopyPixels(nullptr, pw * 4, (UINT)px.size(), px.data())))
        return false;

    char buf[64];
    for (UINT y = 0; y + 1 < ph; y += 2) {
        out.append("  ");
        for (UINT x = 0; x < pw; ++x) {
            const unsigned char* t = &px[((size_t)y * pw + x) * 4];
            const unsigned char* b = &px[((size_t)(y + 1) * pw + x) * 4];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm\xe2\x96\x80",
                t[2], t[1], t[0], b[2], b[1], b[0]);
            out.append(buf);
        }
        out.append("\x1b[0m\r\n");
    }
    return true;
}

}

bool renderImage(const std::wstring& path, int maxCols, int maxRows, std::string& out) {
    out.clear();
    if (maxCols < 1) maxCols = 1;
    if (maxRows < 1) maxRows = 1;

    IWICImagingFactory* factory = getFactory();
    if (!factory) return false;

    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &dec)) || !dec) return false;

    bool ok = renderFromDecoder(factory, dec, maxCols, maxRows, out);
    dec->Release();
    return ok;
}

bool renderImageFromMemory(const std::vector<unsigned char>& data, int maxCols, int maxRows, std::string& out) {
    out.clear();
    if (maxCols < 1) maxCols = 1;
    if (maxRows < 1) maxRows = 1;
    if (data.empty()) return false;

    IWICImagingFactory* factory = getFactory();
    if (!factory) return false;

    IWICStream* stream = nullptr;
    if (FAILED(factory->CreateStream(&stream)) || !stream) return false;

    HRESULT hr = stream->InitializeFromMemory(
        const_cast<BYTE*>(data.data()), (DWORD)data.size());
    if (FAILED(hr)) { stream->Release(); return false; }

    IWICBitmapDecoder* dec = nullptr;
    hr = factory->CreateDecoderFromStream(stream, nullptr,
                                          WICDecodeMetadataCacheOnDemand, &dec);
    stream->Release();
    if (FAILED(hr) || !dec) return false;

    bool ok = renderFromDecoder(factory, dec, maxCols, maxRows, out);
    dec->Release();
    return ok;
}

}