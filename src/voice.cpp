#include "voice.h"
#include "util.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

namespace vrmpet
{
    namespace
    {
        // --- speech gate ----------------------------------------------------
        float g_startRms = 0.012f;      // ~ -38 dBFS; well below speech, above room tone
        float g_holdSeconds = 0.7f;     // silence that ends an utterance
        float g_minSeconds = 0.25f;     // shorter than this and it is a cough
        const int kFrameMs = 20;
        const int kPreRollMs = 300;
        const int kMaxUtteranceSeconds = 20;
        const int kAttackFrames = 3;    // 60 ms above threshold to open

        float Rms(const int16_t* samples, size_t count)
        {
            if (count == 0)
                return 0.0f;
            double sum = 0.0;
            for (size_t i = 0; i < count; ++i)
            {
                const double v = double(samples[i]) / 32768.0;
                sum += v * v;
            }
            return float(std::sqrt(sum / double(count)));
        }

        int16_t ClampSample(double v)
        {
            const double scaled = v * 32767.0;
            if (scaled > 32767.0) return int16_t(32767);
            if (scaled < -32768.0) return int16_t(-32768);
            return int16_t(scaled);
        }
    }

    std::vector<uint8_t> WrapPcm16AsWav(const std::vector<int16_t>& samples, int rate)
    {
        const uint32_t dataBytes = uint32_t(samples.size() * sizeof(int16_t));
        std::vector<uint8_t> wav(size_t(44) + dataBytes);
        uint8_t* p = wav.data();

        auto put32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
        auto put16 = [&](uint16_t v) { memcpy(p, &v, 2); p += 2; };

        memcpy(p, "RIFF", 4); p += 4;
        put32(36 + dataBytes);
        memcpy(p, "WAVE", 4); p += 4;
        memcpy(p, "fmt ", 4); p += 4;
        put32(16);                                   // no extension chunk
        put16(1);                                    // WAVE_FORMAT_PCM
        put16(1);                                    // mono
        put32(uint32_t(rate));
        put32(uint32_t(rate) * 2);                   // byte rate
        put16(2);                                    // block align
        put16(16);                                   // bits
        memcpy(p, "data", 4); p += 4;
        put32(dataBytes);

        if (dataBytes > 0)
            memcpy(p, samples.data(), dataBytes);

        return wav;
    }

    bool SafeToPlay(const uint8_t* d, size_t n, std::string& why)
    {
        auto fail = [&](const std::string& reason) { why = reason; return false; };

        if (d == nullptr)
            return fail("no data");
        if (n < 44)
            return fail("too short to hold a header (" + std::to_string(n) + " bytes)");
        if (memcmp(d, "RIFF", 4) != 0)
            return fail("not RIFF");
        if (memcmp(d + 8, "WAVE", 4) != 0)
            return fail("RIFF type is not WAVE");

        // Our own walk. wiAudio.cpp's FindChunk never increments its byte
        // counter, so its size check is always false and an unrecognised chunk
        // is skipped by an unvalidated size - a malformed file is an
        // out-of-bounds read there. Every step below is checked against n.
        size_t pos = 12;
        size_t dataPos = 0, dataSize = 0;
        bool haveFmt = false;
        uint16_t format = 0, channels = 0, bits = 0;
        uint32_t rate = 0;

        while (pos + 8 <= n)
        {
            char id[4];
            memcpy(id, d + pos, 4);
            uint32_t size = 0;
            memcpy(&size, d + pos + 4, 4);
            const size_t body = pos + 8;
            if (body + size > n)
                return fail(std::string("chunk '") + std::string(id, 4)
                    + "' claims " + std::to_string(size) + " bytes but only "
                    + std::to_string(n - body) + " remain");

            if (memcmp(id, "fmt ", 4) == 0)
            {
                if (size < 16)
                    return fail("fmt chunk is too small");
                memcpy(&format, d + body, 2);
                memcpy(&channels, d + body + 2, 2);
                memcpy(&rate, d + body + 4, 4);
                memcpy(&bits, d + body + 14, 2);
                if (format == 0xFFFE && size >= 40)
                {
                    // WAVE_FORMAT_EXTENSIBLE: the real tag is the first two
                    // bytes of the SubFormat GUID. This is the chunk size that
                    // overflows wiAudio's WAVEFORMATEX memcpy (40 into 18).
                    uint16_t sub = 0;
                    memcpy(&sub, d + body + 24, 2);
                    format = sub;
                }
                haveFmt = true;
            }
            else if (memcmp(id, "data", 4) == 0)
            {
                dataPos = body;
                dataSize = size;
                break;   // the data chunk is the end of what we need
            }

            pos = body + size + (size & 1u);
        }

        if (!haveFmt)
            return fail("no fmt chunk");
        if (dataPos == 0)
            return fail("no data chunk");
        if (format != 1)
            return fail("not PCM (format tag " + std::to_string(format) + ")");
        if (bits != 16)
            return fail("not 16-bit (" + std::to_string(bits) + ")");
        if (channels == 0)
            return fail("zero channels");
        if (rate == 0)
            return fail("zero sample rate");

        // The check that matters. `dataPos` is the first sample's byte, so the
        // data chunk has to account for every byte that follows it - which is
        // exactly the repair the studio's fixWavHeader performs, and the one
        // check a streaming WAV with the placeholder size cannot pass. Without
        // it the placeholder number reaches CreateSound, where it becomes a ~4 GB
        // resize and a memcpy off the end of the buffer.
        if (dataSize != n - dataPos)
            return fail("data size " + std::to_string(dataSize) + " does not match the "
                + std::to_string(n - dataPos) + " bytes actually present");

        why.clear();
        return true;
    }

    float PeakLevel(const std::vector<int16_t>& samples, size_t begin, size_t count)
    {
        if (samples.empty() || begin >= samples.size())
            return 0.0f;
        const size_t end = (std::min)(samples.size(), begin + count);
        float peak = 0.0f;
        for (size_t i = begin; i < end; ++i)
        {
            const float v = std::fabs(float(samples[i]) / 32768.0f);
            if (v > peak)
                peak = v;
        }
        return peak;
    }

    // -----------------------------------------------------------------------
    // Resampling
    // -----------------------------------------------------------------------
    struct MicCapture::Resampler
    {
        double ratio = 1.0;      // source samples per output sample
        bool   up = false;

        // decimation
        double phase = 0.0;
        double acc = 0.0;
        int    count = 0;

        // interpolation
        double inPos = 0.0;
        double srcIndex = 0.0;
        float  prev = 0.0f;

        void Reset(int sourceRate, int targetRate)
        {
            ratio = double(sourceRate) / double(targetRate);
            up = ratio < 1.0;
            phase = 0.0;
            acc = 0.0;
            count = 0;
            inPos = 0.0;
            srcIndex = 0.0;
            prev = 0.0f;
        }

        // Appends to `out`. A non-integer decimation ratio averages the window
        // straddling the boundary, which is a crude low-pass but is exactly what
        // 48 kHz -> 16 kHz needs and costs one add per sample.
        void Push(const float* mono, size_t n, std::vector<int16_t>& out)
        {
            if (n == 0)
                return;

            if (up)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    const double index = srcIndex + double(i);
                    while (inPos <= index)
                    {
                        const double a = (inPos < index) ? double(prev) : double(mono[i]);
                        const double b = double(mono[i]);
                        const double frac = std::clamp(inPos - (index - 1.0), 0.0, 1.0);
                        out.push_back(ClampSample(a + (b - a) * frac));
                        inPos += ratio;
                    }
                }
                prev = mono[n - 1];
                srcIndex += double(n);
                return;
            }

            for (size_t i = 0; i < n; ++i)
            {
                acc += double(mono[i]);
                ++count;
                phase += 1.0;
                if (phase >= ratio)
                {
                    out.push_back(ClampSample(count > 0 ? acc / double(count) : 0.0));
                    acc = 0.0;
                    count = 0;
                    phase -= ratio;
                }
            }
        }
    };

    // -----------------------------------------------------------------------
    // Capture
    // -----------------------------------------------------------------------
    namespace
    {
        const char* DeviceName(IPropertyStore* store);

        template <typename T>
        struct ComHandle
        {
            T* p = nullptr;
            ~ComHandle() { if (p != nullptr) p->Release(); }
            T* operator->() const { return p; }
            explicit operator bool() const { return p != nullptr; }
        };

        // "No capture endpoint at all" is only actionable next to the names and
        // states of the endpoints that do exist: a disabled or unplugged device
        // is a different problem from a machine with no microphone, and on this
        // machine the answer was neither - zero active endpoints, because the
        // only microphones here are Bluetooth and virtual devices that were not
        // running.
        void LogEndpoints(IMMDeviceEnumerator* enumerator)
        {
            ComHandle<IMMDeviceCollection> all;
            if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATEMASK_ALL, &all.p)))
                return;

            UINT count = 0;
            all->GetCount(&count);
            Log("VRMPet: capture endpoints in any state: " + std::to_string(count));
            for (UINT i = 0; i < count; ++i)
            {
                ComHandle<IMMDevice> device;
                if (FAILED(all->Item(i, &device.p)))
                    continue;
                DWORD state = 0;
                device->GetState(&state);
                const char* label = "unknown";
                if (state & DEVICE_STATE_ACTIVE)          label = "active";
                else if (state & DEVICE_STATE_DISABLED)   label = "disabled";
                else if (state & DEVICE_STATE_NOTPRESENT) label = "not present";
                else if (state & DEVICE_STATE_UNPLUGGED)  label = "unplugged";

                std::string name = "?";
                ComHandle<IPropertyStore> store;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store.p)))
                    name = DeviceName(store.p);
                Log("VRMPet:   endpoint " + std::to_string(i) + " '" + name + "' - " + label);
            }
        }

        const char* DeviceName(IPropertyStore* store)
        {
            static thread_local std::string name;
            name.clear();
            if (store == nullptr)
                return "";
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            {
                name = Narrow(value.pwszVal);
            }
            PropVariantClear(&value);
            return name.c_str();
        }
    }

    MicCapture::~MicCapture()
    {
        Stop();
    }

    void MicCapture::Configure(float startRms, float holdSeconds, float minUtteranceSeconds)
    {
        g_startRms = startRms;
        g_holdSeconds = holdSeconds;
        g_minSeconds = minUtteranceSeconds;
    }

    MicStatus MicCapture::Status() const
    {
        std::scoped_lock lock(statusMutex_);
        return status_;
    }

    bool MicCapture::IsOpen() const
    {
        std::scoped_lock lock(statusMutex_);
        return status_.opened;
    }

    void MicCapture::Start(UtteranceCallback onUtterance, LevelCallback onLevel)
    {
        if (run_.load())
            return;
        onUtterance_ = std::move(onUtterance);
        onLevel_ = std::move(onLevel);
        run_ = true;
        thread_ = std::thread([this]() { ThreadMain(); });
    }

    void MicCapture::Stop()
    {
        if (!run_.load() && !thread_.joinable())
            return;
        run_ = false;
        if (thread_.joinable())
            thread_.join();
        // The device handle and its COM objects were created inside the thread
        // and are released at the end of ThreadMain, so nothing is left to close
        // here. Reading a capture endpoint from two apartments is a crash, not a
        // race.
    }

    void MicCapture::ThreadMain()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        std::wstring deviceName;
        HRESULT openResult = E_FAIL;
        HRESULT enumerateResult = E_FAIL;
        UINT deviceCount = 0;

        {
            ComHandle<IMMDeviceEnumerator> enumerator;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&enumerator.p))))
            {
                std::scoped_lock lock(statusMutex_);
                status_.error = "no audio device enumerator - COM is not usable in this thread";
            }
            else
            {
                ComHandle<IMMDeviceCollection> collection;
                // The cheap answer, and the analogue of enumerateDevices():
                // "is there an input at all", with no permission prompt and no
                // device opened.
                enumerateResult = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection.p);
                if (SUCCEEDED(enumerateResult))
                    collection->GetCount(&deviceCount);

                {
                    std::scoped_lock lock(statusMutex_);
                    status_.devices = int(deviceCount);
                }

                if (deviceCount == 0)
                    LogEndpoints(enumerator.p);

                ComHandle<IMMDevice> device;
                openResult = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device.p);
                if (SUCCEEDED(openResult))
                {
                    ComHandle<IPropertyStore> store;
                    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store.p)))
                        deviceName = Widen(DeviceName(store.p));

                    ComHandle<IAudioClient> client;
                    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                (void**)&client.p)))
                    {
                        openResult = E_FAIL;
                    }
                    else
                    {
                        WAVEFORMATEX wanted = {};
                        wanted.wFormatTag      = WAVE_FORMAT_PCM;
                        wanted.nChannels       = 1;
                        wanted.nSamplesPerSec  = 16000;
                        wanted.wBitsPerSample  = 16;
                        wanted.nBlockAlign     = 2;
                        wanted.nAvgBytesPerSec = 16000 * 2;

                        // AUTOCONVERTPCM | SRC_DEFAULT_QUALITY is the whole
                        // trick: in shared mode the audio engine will convert
                        // for us, so we can ask for 16 kHz mono PCM and Windows
                        // resamples the device's 48 kHz float mix down. Without
                        // those flags shared-mode Initialize rejects anything
                        // that is not the mix format, which is why people end up
                        // writing their own resampler.
                        HRESULT init = client->Initialize(
                            AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            100 * 10000, 0, &wanted, nullptr);

                        bool converted = SUCCEEDED(init);
                        WAVEFORMATEX* actual = nullptr;

                        if (!converted)
                        {
                            // This driver will not convert. Take the mix format
                            // and do it ourselves.
                            if (FAILED(client->GetMixFormat(&actual)))
                            {
                                openResult = init;
                            }
                            else
                            {
                                openResult = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                                                100 * 10000, 0, actual, nullptr);
                            }
                        }

                        if (SUCCEEDED(openResult))
                        {
                            ComHandle<IAudioCaptureClient> capture;
                            if (FAILED(client->GetService(IID_PPV_ARGS(&capture.p))))
                            {
                                openResult = E_FAIL;
                            }
                            else
                            {
                                WAVEFORMATEX stream = {};
                                if (converted)
                                    stream = wanted;
                                else if (actual != nullptr)
                                    stream = *actual;

                                const int sourceRate = int(stream.nSamplesPerSec);
                                const int sourceChannels = int(stream.nChannels > 0 ? stream.nChannels : 1);
                                const uint16_t sourceBits = stream.wBitsPerSample;
                                uint16_t sourceFormat = stream.wFormatTag;
                                if (sourceFormat == WAVE_FORMAT_EXTENSIBLE)
                                {
                                    const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)&stream;
                                    sourceFormat = ext->SubFormat.Data1 == 3
                                        ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
                                }

                                {
                                    std::scoped_lock lock(statusMutex_);
                                    status_.deviceName = Narrow(deviceName);
                                    status_.opened = true;
                                    status_.rate = 16000;
                                    status_.sourceRate = sourceRate;
                                    status_.resampled = sourceRate != 16000 || sourceChannels != 1;
                                }

                                Log("VRMPet: mic '" + Narrow(deviceName) + "' open  "
                                    + std::to_string(sourceRate) + " Hz "
                                    + std::to_string(sourceChannels) + " ch "
                                    + std::to_string(sourceBits) + "-bit"
                                    + (converted ? "  (engine converted to 16 kHz mono)"
                                                 : "  (mix format, converting in-process)")
                                    + "  endpoints=" + std::to_string(deviceCount));

                                if (actual != nullptr)
                                    CoTaskMemFree(actual);

                                if (FAILED(client->Start()))
                                {
                                    std::scoped_lock lock(statusMutex_);
                                    status_.opened = false;
                                    status_.error = "the capture client would not start";
                                }
                                else
                                {
                                    Resampler resampler;
                                    resampler.Reset(sourceRate, 16000);

                                    const int frameSamples = 16000 / (1000 / kFrameMs);   // 320
                                    const size_t preRollSamples = size_t(16000 * kPreRollMs / 1000);
                                    const size_t maxSamples = size_t(16000) * kMaxUtteranceSeconds;

                                    std::vector<int16_t> pending;
                                    size_t consumed = 0;
                                    std::vector<int16_t> preRoll;
                                    std::vector<int16_t> utterance;
                                    std::vector<float> mono;
                                    std::vector<int16_t> resampled;
                                    bool speaking = false;
                                    int speechFrames = 0;
                                    int silenceFrames = 0;
                                    int64_t totalFrames = 0;
                                    float noiseFloor = 0.001f;

                                    while (run_.load())
                                    {
                                        UINT32 packet = 0;
                                        capture->GetNextPacketSize(&packet);
                                        bool readSomething = false;

                                        while (packet != 0 && run_.load())
                                        {
                                            BYTE* data = nullptr;
                                            UINT32 frames = 0;
                                            DWORD flags = 0;
                                            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                                                break;

                                            if (frames > 0)
                                            {
                                                mono.clear();
                                                resampled.clear();

                                                if (flags & AUDCLNT_BUFFERFLAGS_SILENT || data == nullptr)
                                                {
                                                    mono.assign(size_t(frames) * sourceChannels, 0.0f);
                                                }
                                                else
                                                {
                                                    mono.resize(size_t(frames));
                                                    if (sourceFormat == WAVE_FORMAT_IEEE_FLOAT && sourceBits == 32)
                                                    {
                                                        const float* src = (const float*)data;
                                                        for (UINT32 f = 0; f < frames; ++f)
                                                        {
                                                            float sum = 0.0f;
                                                            for (int c = 0; c < sourceChannels; ++c)
                                                                sum += src[size_t(f) * sourceChannels + c];
                                                            mono[f] = sum / float(sourceChannels);
                                                        }
                                                    }
                                                    else if (sourceBits == 16)
                                                    {
                                                        const int16_t* src = (const int16_t*)data;
                                                        for (UINT32 f = 0; f < frames; ++f)
                                                        {
                                                            int sum = 0;
                                                            for (int c = 0; c < sourceChannels; ++c)
                                                                sum += src[size_t(f) * sourceChannels + c];
                                                            mono[f] = float(sum) / float(sourceChannels) / 32768.0f;
                                                        }
                                                    }
                                                    else if (sourceBits == 32)
                                                    {
                                                        const int32_t* src = (const int32_t*)data;
                                                        for (UINT32 f = 0; f < frames; ++f)
                                                        {
                                                            int64_t sum = 0;
                                                            for (int c = 0; c < sourceChannels; ++c)
                                                                sum += src[size_t(f) * sourceChannels + c];
                                                            mono[f] = float(double(sum) / double(sourceChannels) / 2147483648.0);
                                                        }
                                                    }
                                                    else
                                                    {
                                                        // 24-bit packed or anything else: treat as silence
                                                        // rather than pretend to decode it.
                                                        mono.assign(size_t(frames), 0.0f);
                                                    }
                                                }

                                                resampler.Push(mono.data(), mono.size(), resampled);
                                                pending.insert(pending.end(), resampled.begin(), resampled.end());
                                                readSomething = true;
                                            }

                                            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                                            {
                                                // A glitch is a glitch. Log it and carry on:
                                                // pretending it did not happen is how a
                                                // one-sided conversation gets debugged for a week.
                                                Log("VRMPet: mic glitch - data discontinuity at "
                                                    + std::to_string(totalFrames) + " frames");
                                            }

                                            capture->ReleaseBuffer(frames);
                                            capture->GetNextPacketSize(&packet);
                                        }

                                        if (readSomething)
                                        {
                                            // Consume whole 20 ms frames.
                                            while (pending.size() - consumed >= size_t(frameSamples))
                                            {
                                                const int16_t* frame = pending.data() + consumed;
                                                const float rms = Rms(frame, size_t(frameSamples));
                                                level_.store(rms);
                                                if (onLevel_)
                                                    onLevel_(rms);

                                                const float threshold = (std::max)(g_startRms, noiseFloor * 3.0f);

                                                if (!speaking)
                                                {
                                                    preRoll.insert(preRoll.end(), frame, frame + frameSamples);
                                                    if (preRoll.size() > preRollSamples)
                                                        preRoll.erase(preRoll.begin(),
                                                            preRoll.begin() + (preRoll.size() - preRollSamples));

                                                    if (rms > threshold)
                                                    {
                                                        if (++speechFrames >= kAttackFrames)
                                                        {
                                                            speaking = true;
                                                            utterance = preRoll;   // includes the attack
                                                            preRoll.clear();
                                                            silenceFrames = 0;
                                                        }
                                                    }
                                                    else
                                                    {
                                                        speechFrames = 0;
                                                        noiseFloor = (std::min)(rms, noiseFloor * 1.05f);
                                                    }
                                                }
                                                else
                                                {
                                                    utterance.insert(utterance.end(), frame, frame + frameSamples);
                                                    if (rms > threshold)
                                                        silenceFrames = 0;
                                                    else
                                                        ++silenceFrames;

                                                    const bool held = double(silenceFrames * kFrameMs) / 1000.0 >= g_holdSeconds;
                                                    const bool full = utterance.size() >= maxSamples;
                                                    if (held || full)
                                                    {
                                                        // Trim to the speech: keep 150 ms of margin on
                                                        // each side, which is free accuracy on ASR and
                                                        // costs nothing.
                                                        size_t first = 0, last = utterance.size();
                                                        const size_t margin = size_t(16000 * 0.15);
                                                        for (size_t i = 0; i + frameSamples <= utterance.size(); i += frameSamples)
                                                        {
                                                            if (Rms(utterance.data() + i, frameSamples) > threshold)
                                                            {
                                                                first = i > margin ? i - margin : 0;
                                                                break;
                                                            }
                                                        }
                                                        for (size_t i = utterance.size(); i >= frameSamples; i -= frameSamples)
                                                        {
                                                            if (Rms(utterance.data() + i - frameSamples, frameSamples) > threshold)
                                                            {
                                                                last = (std::min)(utterance.size(), i + margin);
                                                                break;
                                                            }
                                                        }
                                                        if (last <= first)
                                                            last = utterance.size();

                                                        Utterance result;
                                                        result.pcm.assign(utterance.begin() + first, utterance.begin() + last);
                                                        result.rate = 16000;
                                                        result.seconds = double(result.pcm.size()) / 16000.0;
                                                        result.rms = Rms(result.pcm.data(), result.pcm.size());

                                                        const bool longEnough = result.seconds >= g_minSeconds;
                                                        if (paused_.load())
                                                        {
                                                            Log("VRMPet: heard " + Fmt(float(result.seconds), 2)
                                                                + "s while the reply was playing - ignored "
                                                                "(option A has no barge-in)");
                                                        }
                                                        else if (longEnough && onUtterance_)
                                                        {
                                                            Log("VRMPet: heard " + Fmt(float(result.seconds), 2)
                                                                + "s  rms " + Fmt(result.rms, 4)
                                                                + (full ? "  (cut at the 20 s ceiling)" : ""));
                                                            onUtterance_(result);
                                                        }
                                                        else if (longEnough)
                                                        {
                                                            // no consumer: nothing to do
                                                        }
                                                        else
                                                        {
                                                            Log("VRMPet: discarded " + Fmt(float(result.seconds), 2)
                                                                + "s of sound - shorter than the "
                                                                + Fmt(g_minSeconds, 2) + "s floor");
                                                        }

                                                        speaking = false;
                                                        speechFrames = 0;
                                                        silenceFrames = 0;
                                                        utterance.clear();
                                                    }
                                                }

                                                consumed += size_t(frameSamples);
                                                totalFrames += frameSamples;
                                            }

                                            if (consumed > 8192)
                                            {
                                                pending.erase(pending.begin(), pending.begin() + consumed);
                                                consumed = 0;
                                            }
                                        }

                                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                    }

                                    client->Stop();
                                }
                            }
                        }
                    }
                }
            }
        }

        if (FAILED(openResult))
        {
            std::scoped_lock lock(statusMutex_);
            status_.opened = false;
            if (deviceCount == 0)
                status_.error = "no active capture endpoint";
            else if (openResult == AUDCLNT_E_DEVICE_IN_USE)
                status_.error = "the microphone is held exclusively by another application";
            else if (openResult == AUDCLNT_E_DEVICE_INVALIDATED)
                status_.error = "the capture device was removed";
            else if (openResult == E_ACCESSDENIED)
                status_.error = "microphone access is denied (Settings > Privacy > Microphone)";
            else if (openResult == AUDCLNT_E_UNSUPPORTED_FORMAT)
                status_.error = "the driver rejected both 16 kHz mono and the mix format";
            else
                status_.error = "could not open the default capture device (0x"
                    + std::to_string((unsigned)openResult) + ")";

            Log("VRMPet: mic unavailable - " + status_.error + "  (typed turns still work)");
        }

        if (SUCCEEDED(com))
            CoUninitialize();
        run_ = false;
    }
}
