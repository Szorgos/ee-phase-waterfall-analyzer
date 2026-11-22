// A rádióteszt AAC-fájlja (tests/data/player_stereo44.aac): 1 s saját szinuszjel (bal 440 Hz, jobb 880 Hz, -6 dBFS),
// 44,1 kHz sztereó, AAC-LC 128 kbit/s ADTS-keretekben. A Windows beépített AAC-kódolójával (Media Foundation) készül;
// az ADTS-fejlécet a program írja a nyers AAC-keretek elé.
// Fordítás (llvm-mingw): x86_64-w64-mingw32-clang++ -O2 -std=c++17 tools/make_aac_test_data.cpp -o make_aac.exe
//                        -lmfplat -lmfuuid -lmf -lole32 -static
// Futtatás Windowson: make_aac.exe tests\data\player_stereo44.aac
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK_HR(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s: 0x%08lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "használat: make_aac.exe <kimenet.aac>\n"); return 2; }
    const UINT32 rate = 44100, ch = 2, frames = rate;
    CHECK_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHECK_HR(MFStartup(MF_VERSION));

    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    CHECK_HR(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                       nullptr, &out, &acts, &n));
    if (n == 0) { std::fprintf(stderr, "nincs AAC-kódoló\n"); return 1; }
    IMFTransform* enc = nullptr;
    CHECK_HR(acts[0]->ActivateObject(IID_PPV_ARGS(&enc)));

    IMFMediaType* ot = nullptr;
    CHECK_HR(MFCreateMediaType(&ot));
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
    ot->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
    ot->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);   // nyers keretek; az ADTS-fejléc lent
    CHECK_HR(enc->SetOutputType(0, ot, 0));

    IMFMediaType* it = nullptr;
    CHECK_HR(MFCreateMediaType(&it));
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    it->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    it->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    it->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    it->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
    it->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
    it->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * 2);
    CHECK_HR(enc->SetInputType(0, it, 0));
    CHECK_HR(enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));

    std::vector<int16_t> pcm(frames * ch);
    for (UINT32 i = 0; i < frames; ++i) {
        pcm[i * 2] = static_cast<int16_t>(std::lround(16384.0 * std::sin(2 * 3.14159265358979 * 440.0 * i / rate)));
        pcm[i * 2 + 1] = static_cast<int16_t>(std::lround(16384.0 * std::sin(2 * 3.14159265358979 * 880.0 * i / rate)));
    }

    std::vector<unsigned char> file;
    MFT_OUTPUT_STREAM_INFO si{};
    CHECK_HR(enc->GetOutputStreamInfo(0, &si));
    auto drain = [&]() -> HRESULT {
        for (;;) {
            IMFSample* s = nullptr;
            IMFMediaBuffer* b = nullptr;
            MFCreateSample(&s);
            MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 8192, &b);
            s->AddBuffer(b);
            MFT_OUTPUT_DATA_BUFFER od{ 0, s, 0, nullptr };
            DWORD status = 0;
            const HRESULT hr = enc->ProcessOutput(0, 1, &od, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { b->Release(); s->Release(); return S_OK; }
            if (FAILED(hr)) { b->Release(); s->Release(); return hr; }
            BYTE* p = nullptr;
            DWORD len = 0;
            b->Lock(&p, nullptr, &len);
            const unsigned flen = len + 7;   // ADTS: MPEG-4, LC, 44,1 kHz (index 4), 2 csatorna, CRC nélkül
            const unsigned char h[7] = { 0xFF, 0xF1, static_cast<unsigned char>((1 << 6) | (4 << 2) | (2 >> 2)),
                                         static_cast<unsigned char>(((2 & 3) << 6) | (flen >> 11)),
                                         static_cast<unsigned char>((flen >> 3) & 0xFF),
                                         static_cast<unsigned char>(((flen & 7) << 5) | 0x1F), 0xFC };
            file.insert(file.end(), h, h + 7);
            file.insert(file.end(), p, p + len);
            b->Unlock();
            b->Release();
            s->Release();
        }
    };
    const UINT32 chunk = 1024;
    for (UINT32 off = 0; off < frames; off += chunk) {
        const UINT32 nfr = (frames - off < chunk) ? frames - off : chunk;
        IMFMediaBuffer* b = nullptr;
        CHECK_HR(MFCreateMemoryBuffer(nfr * ch * 2, &b));
        BYTE* p = nullptr;
        b->Lock(&p, nullptr, nullptr);
        std::memcpy(p, &pcm[off * ch], nfr * ch * 2);
        b->Unlock();
        b->SetCurrentLength(nfr * ch * 2);
        IMFSample* s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(b);
        s->SetSampleTime(static_cast<LONGLONG>(off) * 10000000 / rate);
        s->SetSampleDuration(static_cast<LONGLONG>(nfr) * 10000000 / rate);
        HRESULT hr = enc->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING) { CHECK_HR(drain()); hr = enc->ProcessInput(0, s, 0); }
        CHECK_HR(hr);
        s->Release();
        b->Release();
        CHECK_HR(drain());
    }
    CHECK_HR(enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
    CHECK_HR(drain());

    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    std::printf("%zu bájt: %s\n", file.size(), argv[1]);
    enc->Release();
    MFShutdown();
    return 0;
}
