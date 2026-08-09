#include "tui/image.h"

#include <windows.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdio>
#include <vector>
#include <conio.h>

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
            int r1 = t[2], g1 = t[1], b1 = t[0], a1 = t[3];
            int r2 = b[2], g2 = b[1], b2 = b[0], a2 = b[3];
            r1 = (r1 * a1) / 255; g1 = (g1 * a1) / 255; b1 = (b1 * a1) / 255;
            r2 = (r2 * a2) / 255; g2 = (g2 * a2) / 255; b2 = (b2 * a2) / 255;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm\xe2\x96\x80",
                r1, g1, b1, r2, g2, b2);
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

bool playVideoInConsole(const std::wstring& url, int maxCols, int maxRows, const Terminal& term) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return false;

    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromURL(url.c_str(), attr, &reader);
    attr->Release();
    if (FAILED(hr) || !reader) { MFShutdown(); return false; }

    IMFMediaType* outType = nullptr;
    reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &outType);
    UINT32 w = 0, h = 0;
    if (outType) {
        MFGetAttributeSize(outType, MF_MT_FRAME_SIZE, &w, &h);
        outType->Release();
    }
    if (w == 0 || h == 0) { w = 1920; h = 1080; }

    int cols = maxCols;
    int rows = (int)((double)h / w * cols / 2.0 + 0.5);
    if (rows < 1) rows = 1;
    if (rows > maxRows) {
        rows = maxRows;
        cols = (int)((double)w / h * rows * 2.0 + 0.5);
        if (cols < 1) cols = 1;
        if (cols > maxCols) cols = maxCols;
    }
    UINT pw = (UINT)cols, ph = (UINT)rows * 2;

    IMFMediaType* type = nullptr;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
    type->Release();

    term.hideCursor();
    term.moveTo(1, 1);

    bool playing = true;
    while (playing) {
        if (_kbhit()) {
            term.readKey(); // consume
            break;
        }

        DWORD stream, flags;
        LONGLONG timestamp;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;

        IMFMediaBuffer* buffer = nullptr;
        sample->ConvertToContiguousBuffer(&buffer);
        if (buffer) {
            BYTE* data = nullptr;
            DWORD len = 0;
            buffer->Lock(&data, nullptr, &len);
            
            IMFMediaType* currentType = nullptr;
            UINT32 cw = pw, ch = ph;
            if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType))) {
                MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &cw, &ch);
                currentType->Release();
            }
            
            if (data && len >= cw * ch * 4 && cw > 0 && ch > 0) {
                std::string frameOut;
                frameOut.reserve(rows * cols * 40);
                char buf[64];
                for (UINT y = 0; y + 1 < ph; y += 2) {
                    frameOut.append("  ");
                    for (UINT x = 0; x < pw; ++x) {
                        UINT srcX = (x * cw) / pw;
                        UINT srcY1 = (y * ch) / ph;
                        UINT srcY2 = ((y + 1) * ch) / ph;
                        
                        if (srcX >= cw) srcX = cw - 1;
                        if (srcY1 >= ch) srcY1 = ch - 1;
                        if (srcY2 >= ch) srcY2 = ch - 1;
                        
                        UINT memY1 = ch - 1 - srcY1;
                        UINT memY2 = ch - 1 - srcY2;
                        
                        const unsigned char* p1 = &data[memY1 * cw * 4 + srcX * 4];
                        const unsigned char* p2 = &data[memY2 * cw * 4 + srcX * 4];
                        
                        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm\xe2\x96\x80",
                            p1[2], p1[1], p1[0], p2[2], p2[1], p2[0]);
                        frameOut.append(buf);
                    }
                    if (y + 3 < ph) frameOut.append("\x1b[0m\r\n");
                    else frameOut.append("\x1b[0m");
                }
                Frame f; f.raw(frameOut);
                term.present(f);
            }
            buffer->Unlock();
            buffer->Release();
        }
        sample->Release();
        Sleep(33);
    }

    reader->Release();
    MFShutdown();
    return true;
}

}