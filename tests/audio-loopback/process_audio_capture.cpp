// Captures only the selected game process tree through Windows' process
// loopback API. No microphone or other applications' render streams are read.
// API reference: https://learn.microsoft.com/en-us/windows/win32/api/
// audioclientactivationparams/ns-audioclientactivationparams-audioclient_process_loopback_params
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static void checked(HRESULT result, const char *operation)
{
    if (FAILED(result))
    {
        char message[160];
        std::snprintf(message, sizeof(message), "%s failed: HRESULT 0x%08lx", operation,
                      static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase>
{
public:
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    ~Activation() { if (ready) CloseHandle(ready); }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation *operation) override
    {
        HRESULT activated = E_FAIL;
        ComPtr<IUnknown> activatedObject;
        result = operation->GetActivateResult(&activated, &activatedObject);
        if (SUCCEEDED(result)) result = activated;
        if (SUCCEEDED(result)) result = activatedObject.As(&client);
        SetEvent(ready);
        return S_OK;
    }
};

static void header(std::FILE *file, uint64_t bytes)
{
    if (bytes > UINT32_MAX - 36u) throw std::runtime_error("Capture exceeded PCM WAV size limit");
    std::array<uint8_t, 44> data{};
    std::memcpy(data.data(), "RIFF", 4);
    std::memcpy(data.data() + 8, "WAVEfmt ", 8);
    std::memcpy(data.data() + 36, "data", 4);
    auto word = [&](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    word(4, static_cast<uint32_t>(bytes) + 36);
    word(16, 16);
    data[20] = 1;
    data[22] = 2;
    word(24, 48000);
    word(28, 48000 * 4);
    data[32] = 4;
    data[34] = 16;
    word(40, static_cast<uint32_t>(bytes));
    if (std::fseek(file, 0, SEEK_SET) != 0 || std::fwrite(data.data(), 1, data.size(), file) != data.size())
        throw std::runtime_error("Cannot write capture WAV header");
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3 || argc > 4)
    {
        std::fprintf(stderr, "Usage: kfiv_process_audio_capture PID output.wav [max-seconds=600]\n");
        return 2;
    }
    std::FILE *file = nullptr;
    HANDLE target = nullptr, sampleReady = nullptr;
    ComPtr<IAudioClient> client;
    bool comReady = false, started = false;
    uint64_t bytes = 0;
    try
    {
        const DWORD pid = std::stoul(argv[1]);
        const unsigned seconds = argc == 4 ? std::stoul(argv[3]) : 600u;
        if (pid == 0 || seconds == 0 || seconds > 3600) throw std::runtime_error("Invalid PID or duration");
        checked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        comReady = true;
        target = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!target) throw std::runtime_error("Target process is unavailable");
        auto activation = Microsoft::WRL::Make<Activation>();
        if (!activation || !activation->ready) throw std::runtime_error("Cannot create activation event");
        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT property{};
        property.vt = VT_BLOB;
        property.blob.cbSize = sizeof(params);
        property.blob.pBlobData = reinterpret_cast<BYTE *>(&params);
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        checked(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                            &property, activation.Get(), &operation), "ActivateAudioInterfaceAsync");
        if (WaitForSingleObject(activation->ready, 15000) != WAIT_OBJECT_0)
            throw std::runtime_error("Process loopback activation did not complete within 15 seconds");
        checked(activation->result, "Process loopback activation");
        client = activation->client;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = 192000;
        checked(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 0, 0, &format, nullptr), "IAudioClient::Initialize");
        sampleReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sampleReady) throw std::runtime_error("Cannot create capture event");
        checked(client->SetEventHandle(sampleReady), "SetEventHandle");
        ComPtr<IAudioCaptureClient> capture;
        checked(client->GetService(IID_PPV_ARGS(&capture)), "GetService(IAudioCaptureClient)");
        file = _wfopen(argv[2], L"wb");
        if (!file) throw std::runtime_error("Cannot open capture WAV");
        header(file, 0);
        checked(client->Start(), "Start");
        started = true;
        std::printf("Capturing game PID %lu, 48000 Hz stereo, Windows process loopback\n", pid);
        std::fflush(stdout);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        uint64_t nonzero = 0, packets = 0, discontinuities = 0;
        int peak = 0;
        const HANDLE events[] = {sampleReady, target};
        while (std::chrono::steady_clock::now() < deadline)
        {
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 100);
            if (wait == WAIT_OBJECT_0 + 1) break;
            if (wait == WAIT_FAILED) throw std::runtime_error("Capture wait failed");
            UINT32 available = 0;
            checked(capture->GetNextPacketSize(&available), "GetNextPacketSize");
            while (available)
            {
                BYTE *samples = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                checked(capture->GetBuffer(&samples, &frames, &flags, nullptr, nullptr), "GetBuffer");
                const size_t sampleCount = static_cast<size_t>(frames) * 2;
                std::vector<int16_t> silence;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                {
                    silence.resize(sampleCount);
                    samples = reinterpret_cast<BYTE *>(silence.data());
                }
                const auto *pcm = reinterpret_cast<const int16_t *>(samples);
                for (size_t i = 0; i < sampleCount; ++i)
                {
                    nonzero += pcm[i] != 0;
                    peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
                }
                const size_t count = std::fwrite(samples, 2, sampleCount, file);
                checked(capture->ReleaseBuffer(frames), "ReleaseBuffer");
                if (count != sampleCount) throw std::runtime_error("Capture WAV write failed");
                bytes += sampleCount * 2;
                ++packets;
                discontinuities += (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
                checked(capture->GetNextPacketSize(&available), "GetNextPacketSize");
            }
        }
        checked(client->Stop(), "Stop");
        started = false;
        header(file, bytes);
        std::fclose(file); file = nullptr;
        std::printf("frames=%llu nonzero=%llu peak=%d packets=%llu discontinuities=%llu\n",
                    bytes / 4, nonzero, peak, packets, discontinuities);
        CloseHandle(sampleReady); sampleReady = nullptr;
        CloseHandle(target); target = nullptr;
        capture.Reset();
        operation.Reset();
        activation.Reset();
        client.Reset();
        CoUninitialize();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        if (started && client) client->Stop();
        if (file) { try { header(file, bytes); } catch (...) {} std::fclose(file); }
        if (sampleReady) CloseHandle(sampleReady);
        if (target) CloseHandle(target);
        client.Reset();
        if (comReady) CoUninitialize();
        return 1;
    }
}
