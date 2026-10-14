// ---------------------------------------------------------------------------
//  vrmpet::voice - the microphone, the WAV container, and the safety check
//
//  Three separate concerns that all exist because of one fact: on Windows
//  wi::audio is XAudio2 and its CreateSound() decodes only RIFF/WAVE-with-PCM
//  and Ogg Vorbis, takes the `data` chunk's size from the file's own header, and
//  its chunk walker (wiAudio.cpp FindChunk) has no bounds check at all and never
//  increments its own byte counter. A WAV from the network with a placeholder
//  size - which is exactly what a streaming TTS endpoint emits - is therefore a
//  ~4 GB allocation and a wild memcpy in a Release build, not a parse error.
//
//  So: audio we receive is headerless PCM, wrapped in a header we write from a
//  sample count we hold; and anything that does arrive as a file goes through
//  SafeToPlay() first, which is our own bounds-checked walk with a hard reject
//  on every deviation.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrmpet
{
    // 44-byte canonical header over samples we already hold. Mono, 16-bit.
    std::vector<uint8_t> WrapPcm16AsWav(const std::vector<int16_t>& samples, int rate);

    // Bounds-checked RIFF walk. Requires PCM, mono-or-any, 16-bit, and
    // `dataSize == size - (dataPos + 8)` - the last one is the check that
    // catches a placeholder size, and a file that passes it cannot make
    // CreateSound allocate more than it received.
    bool SafeToPlay(const uint8_t* data, size_t size, std::string& why);

    // Peak amplitude of a window, 0..1. The mouth's input.
    float PeakLevel(const std::vector<int16_t>& samples, size_t begin, size_t count);

    struct Utterance
    {
        std::vector<int16_t> pcm;
        int    rate = 16000;
        float  rms = 0.0f;      // of the trimmed utterance
        double seconds = 0.0;
    };

    struct MicStatus
    {
        int         devices = 0;      // active capture endpoints
        std::string deviceName;
        bool        opened = false;
        std::string error;            // why it is not open
        int         rate = 16000;     // the rate the stream was converted to
        int         sourceRate = 0;   // what the device actually delivered
        bool        resampled = false;
    };

    // Always-listening capture: the device opens once and stays open, and
    // utterances are cut out of the stream by an RMS threshold with a hold, so
    // no push-to-talk key and no VAD library are involved.
    class MicCapture
    {
    public:
        using UtteranceCallback = std::function<void(Utterance)>;
        using LevelCallback = std::function<void(float)>;

        ~MicCapture();

        void Start(UtteranceCallback onUtterance, LevelCallback onLevel);
        void Stop();

        MicStatus Status() const;
        bool      IsOpen() const;

        // Half duplex. While a reply is playing the capture thread keeps
        // reading - the level meter stays alive - but does not cut utterances:
        // option A has no barge-in, and without this the pet transcribes its own
        // voice out of the speakers and answers itself.
        void SetPaused(bool paused) { paused_ = paused; }
        bool IsPaused() const { return paused_.load(); }

        float Level() const { return level_.load(); }   // 0..1, most recent frame

        // Speech gate, for the log and for tuning. Defaults suit a headset mic;
        // a noisy room wants a higher floor.
        static void Configure(float startRms, float holdSeconds, float minUtteranceSeconds);

    private:
        void ThreadMain();
        struct Resampler;

        std::thread       thread_;
        std::atomic<bool> run_{ false };
        std::atomic<bool> paused_{ false };
        std::atomic<float> level_{ 0.0f };
        mutable std::mutex statusMutex_;
        MicStatus          status_;
        UtteranceCallback  onUtterance_;
        LevelCallback      onLevel_;
    };
}
