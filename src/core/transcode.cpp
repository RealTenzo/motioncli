#include "core/transcode.h"
#include "core/config.h"
#include "core/wallpaper.h"
#include "util/str.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <mferror.h>

#include <cstdio>

namespace motion {

namespace {

void releaseUnknown(IUnknown* obj) {
    if (obj) obj->Release();
}

bool readDims(IMFSourceReader* reader, int& w, int& h, int& fpsN, int& fpsD, LONGLONG& dur) {
    w = 0; h = 0; fpsN = 30; fpsD = 1; dur = 0;
    IMFMediaType* native = nullptr;
    if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)) || !native)
        return false;
    UINT32 uw = 0, uh = 0;
    bool ok = SUCCEEDED(MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &uw, &uh)) && uw > 0 && uh > 0;
    UINT32 n = 0, d = 0;
    if (SUCCEEDED(MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &n, &d)) && n > 0 && d > 0) {
        fpsN = (int)n; fpsD = (int)d;
    }
    native->Release();
    if (!ok) return false;
    w = (int)uw; h = (int)uh;
    PROPVARIANT var{};
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
        if (var.vt == VT_UI8) dur = (LONGLONG)var.uhVal.QuadPart;
        PropVariantClear(&var);
    }
    return true;
}

IMFSample* makeNv12Sample(int w, int h) {
    IMFMediaBuffer* buf = nullptr;
    DWORD cb = (DWORD)w * (DWORD)h * 3 / 2;
    if (FAILED(MFCreateMemoryBuffer(cb, &buf)) || !buf) return nullptr;
    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample)) || !sample) {
        buf->Release();
        return nullptr;
    }
    if (FAILED(sample->AddBuffer(buf))) {
        buf->Release();
        sample->Release();
        return nullptr;
    }
    buf->Release();
    return sample;
}

IMFMediaType* makeVideoType(const GUID& sub, int w, int h, int fpsN, int fpsD) {
    IMFMediaType* type = nullptr;
    if (FAILED(MFCreateMediaType(&type)) || !type) return nullptr;
    bool ok = SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
              SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, sub)) &&
              SUCCEEDED(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h)) &&
              SUCCEEDED(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, (UINT32)fpsN, (UINT32)fpsD)) &&
              SUCCEEDED(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) &&
              SUCCEEDED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    if (!ok) {
        type->Release();
        return nullptr;
    }
    return type;
}

}

bool needsOptimize(const std::wstring& path) {
    if (!fileExists(path)) return false;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(hrCo);
    HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    bool shutdown = SUCCEEDED(hrMf);
    int w = 0, h = 0, fpsN = 30, fpsD = 1;
    LONGLONG dur = 0;
    bool probed = false;
    IMFSourceReader* reader = nullptr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) && reader) {
        probed = readDims(reader, w, h, fpsN, fpsD, dur);
        reader->Release();
    }
    if (shutdown) MFShutdown();
    if (uninit) CoUninitialize();
    return probed && h > targetHeight() + 80;
}

int targetHeight() {
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    if (screenH <= 0) screenH = 768;
    return screenH <= 800 ? 720 : 1080;
}

bool optimizeVideo(const std::wstring& src, const std::wstring& dest,
                   void(*onProgress)(int, void*), void* ctx,
                   OptimizeStats& stats, std::string& err, std::string& note) {
    stats = OptimizeStats{};
    note.clear();
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(hrCo);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        err = "Media Foundation failed to start";
        if (uninit) CoUninitialize();
        return false;
    }

    IMFSourceReader* reader = nullptr;
    IMFTransform* scale = nullptr;
    IMFSinkWriter* writer = nullptr;
    IMFSample* outSample = nullptr;
    DWORD videoStream = 0;
    DWORD audioStream = 0;
    bool writerAudio = false;
    bool srcHadAudio = false;
    int lastPct = -2;

    auto fail = [&](const std::string& msg) {
        err = msg;
        releaseUnknown(outSample);
        releaseUnknown(writer);
        releaseUnknown(scale);
        releaseUnknown(reader);
        MFShutdown();
        if (uninit) CoUninitialize();
        DeleteFileW(dest.c_str());
    };

    if (FAILED(MFCreateSourceReaderFromURL(src.c_str(), nullptr, &reader)) || !reader) {
        fail("Could not open video file");
        return false;
    }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)))
        srcHadAudio = true;

    int inW = 0, inH = 0, fpsN = 30, fpsD = 1;
    LONGLONG dur = 0;
    if (!readDims(reader, inW, inH, fpsN, fpsD, dur) || inW <= 0 || inH <= 0) {
        fail("Could not read video dimensions");
        return false;
    }
    if (inH <= targetHeight() + 80) {
        fail("Video is already efficient for this PC");
        return false;
    }
    int outH = targetHeight();
    int outW = (inW * outH / inH) & ~1;
    if (outW < 2) outW = 2;
    long long bitrate = (long long)outW * outH * fpsN / (fpsD == 0 ? 1 : fpsD) / 8;
    if (bitrate < 1000000) bitrate = 1000000;
    if (bitrate > 8000000) bitrate = 8000000;
    stats.inW = inW; stats.inH = inH;
    stats.outW = outW; stats.outH = outH;

    IMFMediaType* readerType = makeVideoType(MFVideoFormat_NV12, inW, inH, fpsN, fpsD);
    if (!readerType || FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, readerType))) {
        releaseUnknown(readerType);
        fail("Decoder setup failed");
        return false;
    }
    readerType->Release();

    if (FAILED(CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&scale))) || !scale) {
        fail("Video scaler unavailable");
        return false;
    }
    IMFMediaType* scaleIn = makeVideoType(MFVideoFormat_NV12, inW, inH, fpsN, fpsD);
    IMFMediaType* scaleOut = makeVideoType(MFVideoFormat_NV12, outW, outH, fpsN, fpsD);
    bool scaleOk = scaleIn && scaleOut &&
                   SUCCEEDED(scale->SetInputType(0, scaleIn, 0)) &&
                   SUCCEEDED(scale->SetOutputType(0, scaleOut, 0));
    releaseUnknown(scaleIn);
    releaseUnknown(scaleOut);
    if (!scaleOk) {
        fail("Scaler setup failed");
        return false;
    }
    scale->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    scale->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    if (FAILED(MFCreateSinkWriterFromURL(dest.c_str(), nullptr, nullptr, &writer)) || !writer) {
        fail("Could not create output file");
        return false;
    }
    IMFMediaType* encType = makeVideoType(MFVideoFormat_H264, outW, outH, fpsN, fpsD);
    if (!encType || FAILED(encType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate))) {
        releaseUnknown(encType);
        fail("Encoder setup failed");
        return false;
    }
    if (FAILED(writer->AddStream(encType, &videoStream))) {
        encType->Release();
        fail("Encoder setup failed");
        return false;
    }
    encType->Release();
    IMFMediaType* writerIn = makeVideoType(MFVideoFormat_NV12, outW, outH, fpsN, fpsD);
    if (!writerIn || FAILED(writer->SetInputMediaType(videoStream, writerIn, nullptr))) {
        releaseUnknown(writerIn);
        fail("Encoder input setup failed");
        return false;
    }
    writerIn->Release();

    if (srcHadAudio) {
        IMFMediaType* audioNative = nullptr;
        GUID audioSub = GUID_NULL;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audioNative)) && audioNative) {
            audioNative->GetGUID(MF_MT_SUBTYPE, &audioSub);
            if (audioSub == MFAudioFormat_AAC &&
                SUCCEEDED(writer->AddStream(audioNative, &audioStream)) &&
                SUCCEEDED(writer->SetInputMediaType(audioStream, audioNative, nullptr))) {
                writerAudio = true;
            }
            audioNative->Release();
        }
        if (!writerAudio)
            reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
    }

    if (FAILED(writer->BeginWriting())) {
        fail("Could not start writing output");
        return false;
    }

    LONGLONG frameDur = (LONGLONG)fpsD * 10000000 / (fpsN <= 0 ? 30 : fpsN);
    if (frameDur <= 0) frameDur = 333333;
    LONGLONG nextTime = 0;
    int changes = 0;

    auto report = [&](LONGLONG stamp) {
        if (!onProgress || dur <= 0) return;
        int pct = (int)(stamp * 100 / dur);
        if (pct > 100) pct = 100;
        if (pct != lastPct) {
            lastPct = pct;
            onProgress(pct, ctx);
        }
    };

    auto pushScaled = [&](IMFSample* scaled, LONGLONG fallback) -> bool {
        if (!scaled) return true;
        LONGLONG stamp = fallback;
        LONGLONG sdur = frameDur;
        LONGLONG t = 0;
        if (SUCCEEDED(scaled->GetSampleTime(&t))) stamp = t;
        if (FAILED(scaled->GetSampleDuration(&t))) scaled->SetSampleDuration(sdur);
        else sdur = t;
        scaled->SetSampleTime(stamp);
        nextTime = stamp + sdur;
        if (FAILED(writer->WriteSample(videoStream, scaled))) {
            err = "Failed while writing output";
            return false;
        }
        report(stamp);
        return true;
    };

    auto pullScaled = [&](LONGLONG fallback, bool& ok) {
        while (true) {
            if (!outSample) outSample = makeNv12Sample(outW, outH);
            if (!outSample) { ok = false; err = "Out of memory"; return; }
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.pSample = outSample;
            DWORD status = 0;
            HRESULT hr = scale->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { ok = true; return; }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                outSample->Release(); outSample = nullptr;
                if (++changes > 8) { ok = false; err = "Scaler unstable"; return; }
                continue;
            }
            if (FAILED(hr) || !ob.pSample) { ok = false; err = "Scaler output failed"; return; }
            if (ob.pSample != outSample) {
                outSample->Release();
                outSample = ob.pSample;
            }
            if (ob.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) continue;
            if (!pushScaled(outSample, fallback)) { ok = false; return; }
        }
    };

    bool videoDone = false;    bool audioDone = !writerAudio;
    while (!videoDone) {
        DWORD readFlags = 0;
        LONGLONG stamp = 0;
        IMFSample* inSample = nullptr;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &readFlags, &stamp, &inSample);
        if (FAILED(hr)) {
            releaseUnknown(inSample);
            fail("Failed while reading input");
            return false;
        }
        if (readFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            releaseUnknown(inSample);
            videoDone = true;
        } else if (inSample) {
            hr = scale->ProcessInput(0, inSample, 0);
            inSample->Release();
            if (FAILED(hr)) {
                fail("Scaler input failed");
                return false;
            }
            bool ok = true;
            pullScaled(stamp, ok);
            if (!ok) {
                fail(err.empty() ? "Scaler output failed" : err);
                return false;
            }
        }
        if (!audioDone) {
            DWORD aFlags = 0;
            LONGLONG aStamp = 0;
            IMFSample* aSample = nullptr;
            hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &aFlags, &aStamp, &aSample);
            if (FAILED(hr)) {
                releaseUnknown(aSample);
                fail("Failed while reading audio");
                return false;
            }
            if (aFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                releaseUnknown(aSample);
                audioDone = true;
            } else if (aSample) {
                hr = writer->WriteSample(audioStream, aSample);
                aSample->Release();
                if (FAILED(hr)) {
                    fail("Failed while writing audio");
                    return false;
                }
            }
        }
    }

    while (!audioDone) {
        DWORD aFlags = 0;
        LONGLONG aStamp = 0;
        IMFSample* aSample = nullptr;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &aFlags, &aStamp, &aSample);
        if (FAILED(hr)) {
            releaseUnknown(aSample);
            fail("Failed while reading audio");
            return false;
        }
        if (aFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            releaseUnknown(aSample);
            audioDone = true;
        } else if (aSample) {
            hr = writer->WriteSample(audioStream, aSample);
            aSample->Release();
            if (FAILED(hr)) {
                fail("Failed while writing audio");
                return false;
            }
        }
    }

    scale->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    {
        bool ok = true;
        pullScaled(nextTime, ok);
        if (!ok) {
            fail(err.empty() ? "Scaler drain failed" : err);
            return false;
        }
    }

    if (FAILED(writer->Finalize())) {
        fail("Could not finish output file");
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz{};
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        stats.outBytes = (long long)sz.QuadPart;
    }
    stats.seconds = dur > 0 ? (double)dur / 10000000.0 : 0.0;
    if (srcHadAudio && !writerAudio)
        note = "video only, original audio dropped";

    releaseUnknown(outSample);
    releaseUnknown(writer);
    releaseUnknown(scale);
    releaseUnknown(reader);
    MFShutdown();
    if (uninit) CoUninitialize();
    if (onProgress) onProgress(100, ctx);
    return true;
}

void pointConfigAtMedia(const std::wstring& oldPath, const std::wstring& newPath) {
    Config cfg = Config::load();
    if (!cfg.currentMediaPath.empty() && cfg.currentMediaPath == oldPath)
        cfg.currentMediaPath = newPath;
    for (auto& kv : cfg.monitorAssignments) {
        if (kv.second == oldPath) kv.second = newPath;
    }
    cfg.save();
}

int runOptimize() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    Config cfg = Config::load();
    std::wstring src = cfg.currentMediaPath;
    if (src.empty() || !fileExists(src)) {
        printf("No active wallpaper file to optimize.\n");
        fflush(stdout);
        FreeConsole();
        return 1;
    }
    if (!needsOptimize(src)) {
        printf("Already efficient for this PC, nothing to do.\n");
        fflush(stdout);
        FreeConsole();
        return 0;
    }

    std::wstring dest = src;
    size_t dot = dest.find_last_of(L'.');
    size_t slash = dest.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        dest.resize(dot);
    dest += L".opt.mp4";

    printf("Optimizing %s\n", narrow(src).c_str());
    int lastShown = -2;
    auto progress = +[](int pct, void* vctx) {
        int* last = (int*)vctx;
        if (pct == *last) return;
        *last = pct;
        if (pct < 0) {
            printf("\rWorking...    ");
        } else {
            printf("\r[%3d%%]", pct);
        }
        fflush(stdout);
    };

    OptimizeStats stats;
    std::string err, note;
    bool ok = optimizeVideo(src, dest, progress, &lastShown, stats, err, note);
    printf("\n");
    if (!ok) {
        printf("Optimize failed: %s\n", err.c_str());
        fflush(stdout);
        FreeConsole();
        return 1;
    }
    pointConfigAtMedia(src, dest);
    printf("Done: %dx%d -> %dx%d, %.1f MB%s%s\n",
           stats.inW, stats.inH, stats.outW, stats.outH,
           stats.outBytes / 1048576.0,
           note.empty() ? "" : " (",
           note.empty() ? "" : (note + ")").c_str());

    std::string restartErr;
    EngineController controller(cfg);
    if (!controller.restart(restartErr)) {
        printf("Saved, but engine restart failed: %s\n", restartErr.c_str());
        fflush(stdout);
        FreeConsole();
        return 1;
    }
    fflush(stdout);
    FreeConsole();
    return 0;
}

}
