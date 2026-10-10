#include "runtime/ps2_audio.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int checks = 0, failures = 0;
    void check(bool passed, const char *expression, int line)
    {
        ++checks;
        if (!passed)
        {
            ++failures;
            std::cerr << "FAIL line " << line << ": " << expression << '\n';
        }
    }
#define CHECK(expression) check((expression), #expression, __LINE__)

    void environment(const char *key, const std::string &value)
    {
#ifdef _WIN32
        _putenv_s(key, value.c_str());
#else
        if (value.empty()) unsetenv(key);
        else setenv(key, value.c_str(), 1);
#endif
    }

    namespace fake
    {
        // Raylib serializes callbacks and stream mutation under one device lock.
        std::mutex audioLock;
        std::atomic<bool> device{true};
        bool validStream = true;
        bool playing = false;
        AudioCallback callback = nullptr;
        unsigned rate = 0, bits = 0, channels = 0;
        int token = 0;
        std::thread::id uiThread = std::this_thread::get_id();
        std::atomic<int> nonUiApiCalls{0};
        std::vector<std::string> lifecycle;

        void api() { if (std::this_thread::get_id() != uiThread) ++nonUiApiCalls; }
        void reset()
        {
            std::lock_guard lock(audioLock);
            device = true;
            validStream = true;
            playing = false;
            callback = nullptr;
            rate = bits = channels = 0;
            nonUiApiCalls = 0;
            lifecycle.clear();
        }
        std::vector<int16_t> render(unsigned frames)
        {
            std::vector<int16_t> output(static_cast<size_t>(frames) * 2, 12345);
            std::lock_guard lock(audioLock);
            if (playing && callback) callback(output.data(), frames);
            else std::fill(output.begin(), output.end(), int16_t{0});
            return output;
        }
    }

    void ready(PS2AudioBackend &audio)
    {
        audio.setAudioReady(true);
        CHECK(audio.startPcmStream());
        CHECK(!fake::playing);
    }

    void prefill(PS2AudioBackend &audio)
    {
        std::vector<int16_t> silence(4800 * 2, 0);
        audio.submitPcm(silence);
        audio.updatePcmStream();
        CHECK(fake::playing);
        CHECK(fake::render(4800) == silence);
        audio.updatePcmStream();
    }

    std::vector<uint8_t> readFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    uint32_t littleWord(const std::vector<uint8_t> &data, size_t offset)
    {
        uint32_t value = 0;
        if (offset + 4 > data.size()) return UINT32_MAX;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t{data[offset + i]} << (8 * i);
        return value;
    }
    void verifyWav(const std::vector<uint8_t> &data, size_t frames)
    {
        CHECK(data.size() == 44 + frames * 4);
        if (data.size() < 44) return;
        CHECK(std::memcmp(data.data(), "RIFF", 4) == 0);
        CHECK(std::memcmp(data.data() + 8, "WAVEfmt ", 8) == 0);
        CHECK(std::memcmp(data.data() + 36, "data", 4) == 0);
        CHECK(littleWord(data, 4) == 36 + frames * 4);
        CHECK(littleWord(data, 24) == 48000);
        CHECK(littleWord(data, 28) == 192000);
        CHECK(data[20] == 1 && data[22] == 2 && data[32] == 4 && data[34] == 16);
        CHECK(littleWord(data, 40) == frames * 4);
    }
    std::vector<int16_t> wavSamples(const std::vector<uint8_t> &data)
    {
        std::vector<int16_t> output;
        if (data.size() < 44) return output;
        for (size_t i = 44; i + 1 < data.size(); i += 2)
            output.push_back(static_cast<int16_t>(uint16_t{data[i]} | (uint16_t{data[i + 1]} << 8)));
        return output;
    }

    void initialization()
    {
        fake::reset();
        PS2AudioBackend first, second;
        CHECK(!first.startPcmStream());
        first.setAudioReady(true);
        fake::device = false;
        CHECK(!first.startPcmStream());
        fake::device = true;
        fake::validStream = false;
        CHECK(!first.startPcmStream());
        fake::validStream = true;
        ready(first);
        CHECK(fake::rate == 48000 && fake::bits == 16 && fake::channels == 2);
        CHECK(first.startPcmStream());
        second.setAudioReady(true);
        CHECK(!second.startPcmStream());
        first.stopPcmStream();
        CHECK(!first.pcmStats().running);
        ready(second);
        second.stopPcmStream();
        CHECK(fake::callback == nullptr && !fake::playing);
        CHECK(fake::nonUiApiCalls == 0);
    }

    void stereoAndUnderrun()
    {
        fake::reset();
        PS2AudioBackend audio;
        ready(audio);
        prefill(audio);
        const std::array<int16_t, 8> pcm{1, -2, 32767, -32768, 501, -700, 0, 0};
        audio.submitPcm(pcm);
        CHECK(audio.pcmStats().queuedFrames == 4);
        CHECK(fake::render(2) == std::vector<int16_t>(pcm.begin(), pcm.begin() + 4));
        CHECK(fake::render(4) == std::vector<int16_t>({501, -700, 0, 0, 0, 0, 0, 0}));
        CHECK(fake::render(3) == std::vector<int16_t>(6, 0));
        audio.submitPcm({});
        const std::array<int16_t, 1> incomplete{900};
        audio.submitPcm(incomplete);
        const std::array<int16_t, 3> odd{111, -222, 900};
        audio.submitPcm(odd);
        CHECK(fake::render(1) == std::vector<int16_t>({111, -222}));
        CHECK(fake::render(0).empty());
        const auto stats = audio.pcmStats();
        CHECK(stats.submittedFrames == 4805 && stats.consumedFrames == 4805);
        CHECK(stats.outputFrames == 4810 && stats.underrunFrames == 5);
        CHECK(stats.nonzeroSamples == 8 && stats.peak == 32768);
        CHECK(stats.queuedFrames == 0 && stats.droppedFrames == 0);
        audio.stopPcmStream();
        ready(audio);
        CHECK(audio.pcmStats().submittedFrames == 0 && audio.pcmStats().outputFrames == 0);
        CHECK(fake::render(1) == std::vector<int16_t>(2, 0));
    }

    void boundedBufferAndWrap()
    {
        fake::reset();
        PS2AudioBackend audio;
        ready(audio);
        std::vector<int16_t> pcm(9000 * 2);
        for (size_t i = 0; i < 9000; ++i) { pcm[i * 2] = static_cast<int16_t>(i); pcm[i * 2 + 1] = static_cast<int16_t>(-static_cast<int>(i)); }
        audio.submitPcm(pcm);
        CHECK(audio.pcmStats().queuedFrames == 8192);
        CHECK(audio.pcmStats().droppedFrames == 808);
        audio.updatePcmStream();
        const auto first = fake::render(8000);
        CHECK(std::equal(first.begin(), first.end(), pcm.begin()));
        const std::array<int16_t, 4> tail{20001, -20001, 20002, -20002};
        audio.submitPcm(tail);
        const auto wrapped = fake::render(194);
        CHECK(std::equal(wrapped.begin(), wrapped.begin() + 384, pcm.begin() + 16000));
        CHECK(std::equal(wrapped.end() - 4, wrapped.end(), tail.begin()));
        CHECK(audio.pcmStats().queuedFrames == 0);
        CHECK(audio.pcmStats().consumedFrames == 8194);
        CHECK(fake::render(1) == std::vector<int16_t>(2, 0));
    }

    void producerConcurrency()
    {
        fake::reset();
        PS2AudioBackend audio;
        ready(audio);
        std::atomic<int> producers{2};
        std::atomic<bool> tornFrame{false};
        auto produce = [&](int base) {
            std::array<int16_t, 32> block{};
            for (int iteration = 0; iteration < 2000; ++iteration)
            {
                for (int i = 0; i < 16; ++i)
                {
                    const auto value = static_cast<int16_t>(base + iteration);
                    block[i * 2] = value;
                    block[i * 2 + 1] = static_cast<int16_t>(-value);
                }
                audio.submitPcm(block);
                if (iteration % 7 == 0) std::this_thread::yield();
            }
            --producers;
        };
        std::thread left(produce, 1000), right(produce, 10000);
        std::thread output([&] {
            while (producers.load() != 0 || audio.pcmStats().queuedFrames != 0)
            {
                const auto frames = fake::render(128);
                for (size_t i = 0; i < frames.size(); i += 2)
                    if (frames[i] != -static_cast<int>(frames[i + 1])) tornFrame = true;
                std::this_thread::yield();
            }
        });
        while (audio.pcmStats().queuedFrames < 4800) std::this_thread::yield();
        audio.updatePcmStream();
        left.join(); right.join(); output.join();
        CHECK(!tornFrame);
        CHECK(fake::nonUiApiCalls == 0);
        const auto stats = audio.pcmStats();
        CHECK(stats.submittedFrames == 64000);
        CHECK(stats.consumedFrames + stats.droppedFrames == stats.submittedFrames);
        CHECK(stats.queuedFrames == 0);
        CHECK(stats.outputFrames == stats.consumedFrames + stats.underrunFrames);

        // Teardown with both producer and callback threads alive must detach safely.
        std::atomic<bool> finish{false};
        std::atomic<unsigned> producerVisits{0}, callbackVisits{0};
        std::thread producer([&] { const std::array<int16_t, 2> frame{17, -17}; while (!finish) { audio.submitPcm(frame); ++producerVisits; std::this_thread::yield(); } });
        std::thread consumer([&] { while (!finish) { fake::render(32); ++callbackVisits; std::this_thread::yield(); } });
        while (producerVisits < 100 || callbackVisits < 100) std::this_thread::yield();
        audio.stopPcmStream();
        finish = true;
        producer.join(); consumer.join();
        CHECK(!audio.pcmStats().running && fake::callback == nullptr);
        CHECK(fake::render(2) == std::vector<int16_t>(4, 0));
        CHECK(fake::nonUiApiCalls == 0);
    }

    void captures()
    {
        fake::reset();
        const auto devicePath = std::filesystem::absolute("host-audio-device-test.wav");
        const auto sourcePath = std::filesystem::absolute("host-audio-source-test.wav");
        environment("PS2X_AUDIO_DUMP", devicePath.string());
        environment("PS2X_AUDIO_SOURCE_DUMP", sourcePath.string());
        std::vector<int16_t> pcm(10000 * 2);
        for (size_t i = 0; i < 10000; ++i) { pcm[i * 2] = static_cast<int16_t>(i + 1); pcm[i * 2 + 1] = static_cast<int16_t>(-static_cast<int>(i + 1)); }
        std::vector<int16_t> played;
        {
            PS2AudioBackend audio;
            ready(audio);
            audio.submitPcm(pcm);
            audio.updatePcmStream();
            played = fake::render(10004);
            audio.updatePcmStream();
            audio.stopPcmStream();
            CHECK(audio.pcmStats().droppedFrames == 1808);
            CHECK(audio.pcmStats().underrunFrames == 1812);
            CHECK(audio.pcmStats().captureDroppedFrames == 0);
            CHECK(audio.pcmStats().sourceCaptureDroppedFrames == 0);
            const auto eventCount = fake::lifecycle.size();
            audio.stopPcmStream();
            CHECK(fake::lifecycle.size() == eventCount);
        }
        const auto device = readFile(devicePath), source = readFile(sourcePath);
        verifyWav(device, 10004);
        verifyWav(source, 10000);
        CHECK(wavSamples(device) == played);
        CHECK(wavSamples(source) == pcm);
        CHECK(std::equal(played.begin(), played.begin() + 8192 * 2, pcm.begin()));
        CHECK(std::all_of(played.begin() + 8192 * 2, played.end(), [](int16_t value) { return value == 0; }));

        // A stalled UI cannot make diagnostic capture buffers grow without bound.
        {
            PS2AudioBackend audio;
            ready(audio);
            prefill(audio);
            std::vector<int16_t> large(270000 * 2, 4321);
            audio.submitPcm(large);
            fake::render(270000);
            CHECK(audio.pcmStats().sourceCaptureDroppedFrames == 270000 - 262144);
            CHECK(audio.pcmStats().captureDroppedFrames == 270000 - 262144);
            audio.stopPcmStream();
        }
        verifyWav(readFile(devicePath), 4800 + 262144);
        verifyWav(readFile(sourcePath), 4800 + 262144);
        environment("PS2X_AUDIO_DUMP", "");
        environment("PS2X_AUDIO_SOURCE_DUMP", "");
        CHECK(std::filesystem::remove(devicePath));
        CHECK(std::filesystem::remove(sourcePath));
    }

    void deviceTeardown()
    {
        fake::reset();
        {
            PS2AudioBackend audio;
            ready(audio);
            prefill(audio);
            audio.stopPcmStream();
            CHECK(fake::lifecycle == std::vector<std::string>({"load", "attach", "play", "stop", "detach", "unload"}));
            fake::device = false;
            audio.setAudioReady(false);
        }
        CHECK(fake::lifecycle.back() == "unload");
        CHECK(fake::callback == nullptr);
    }

    void prebufferContract()
    {
        fake::reset();
        PS2AudioBackend audio;
        ready(audio);
        std::vector<int16_t> samples(4799 * 2);
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<int16_t>(i);
        audio.submitPcm(samples);
        audio.updatePcmStream();
        CHECK(!fake::playing);
        CHECK(fake::render(16) == std::vector<int16_t>(32, 0));
        CHECK(audio.pcmStats().consumedFrames == 0 && audio.pcmStats().outputFrames == 0);
        const std::array<int16_t, 2> finalFrame{111, -222};
        audio.submitPcm(finalFrame);
        audio.updatePcmStream();
        CHECK(fake::playing);
        samples.insert(samples.end(), finalFrame.begin(), finalFrame.end());
        CHECK(fake::render(4800) == samples);
        CHECK(audio.pcmStats().underrunFrames == 0);
        audio.stopPcmStream();
        ready(audio);
        audio.updatePcmStream();
        CHECK(!fake::playing && audio.pcmStats().queuedFrames == 0);
    }
}

extern "C"
{
    bool IsAudioDeviceReady(void) { fake::api(); return fake::device.load(); }
    AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels)
    {
        fake::api(); std::lock_guard lock(fake::audioLock);
        fake::rate = sampleRate; fake::bits = sampleSize; fake::channels = channels;
        fake::lifecycle.emplace_back("load");
        AudioStream stream{};
        if (fake::validStream) stream.buffer = reinterpret_cast<decltype(stream.buffer)>(&fake::token);
        stream.sampleRate = sampleRate; stream.sampleSize = sampleSize; stream.channels = channels;
        return stream;
    }
    bool IsAudioStreamValid(AudioStream stream) { fake::api(); return stream.buffer != nullptr; }
    void SetAudioStreamCallback(AudioStream, AudioCallback callback)
    {
        fake::api(); std::lock_guard lock(fake::audioLock);
        fake::callback = callback;
        fake::lifecycle.emplace_back(callback ? "attach" : "detach");
    }
    void PlayAudioStream(AudioStream) { fake::api(); std::lock_guard lock(fake::audioLock); fake::playing = true; fake::lifecycle.emplace_back("play"); }
    void StopAudioStream(AudioStream) { fake::api(); std::lock_guard lock(fake::audioLock); fake::playing = false; fake::lifecycle.emplace_back("stop"); }
    void UnloadAudioStream(AudioStream) { fake::api(); std::lock_guard lock(fake::audioLock); fake::lifecycle.emplace_back("unload"); }
    bool IsSoundPlaying(Sound) { return false; }
    void UnloadSound(Sound) {}
    void StopSound(Sound) {}
    Wave LoadWaveFromMemory(const char *, const unsigned char *, int) { return {}; }
    Sound LoadSoundFromWave(Wave) { return {}; }
    void UnloadWave(Wave) {}
    void SetSoundPitch(Sound, float) {}
    void SetSoundVolume(Sound, float) {}
    void PlaySound(Sound) {}
}

int main()
{
    environment("PS2X_AUDIO_TRACE", "");
    environment("PS2X_AUDIO_DUMP", "");
    environment("PS2X_AUDIO_SOURCE_DUMP", "");
    initialization();
    prebufferContract();
    stereoAndUnderrun();
    boundedBufferAndWrap();
    producerConcurrency();
    captures();
    deviceTeardown();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
