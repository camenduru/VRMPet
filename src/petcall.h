// ---------------------------------------------------------------------------
//  vrmpet::petcall - one turn, from the microphone to the mouth
//
//  This is the whole of option A in one place:
//
//    mic (always listening, RMS VAD)
//      -> POST /v1/asr                       text
//      -> the agent's own LLM (via the test harness, see fish.h)  reply text
//      -> POST /v1/tts, sentence by sentence   headerless s16le
//      -> wi::audio                           speakers
//      -> the played samples                  mouth weight
//
//  Three threads, and only three things cross between them:
//
//    * the level  - an atomic float, read every frame by the mouth. A mutex
//                   there would be a lock at 60 Hz for one number.
//    * events     - transcript turns, mode changes, errors. A few per turn, so
//                   a mutex is free. Delivered on the main thread by Tick(),
//                   because the bubble is Direct2D on the main thread's DC.
//    * audio      - handed over once per spoken sentence, then owned by the
//                   main thread, because every wi::audio call is made there.
//
//  Option A has no barge-in, so the microphone is paused while a reply plays.
//  Without that the pet transcribes its own voice out of the speakers and
//  answers itself, which reads as a haunting rather than a conversation.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fish.h"

namespace vrmpet
{
    // The seam from the design document: what a transport has to be able to tell
    // an avatar, and nothing about how it does it. Nothing here knows about
    // WebRTC, and nothing knows about VRM. A realtime transport can replace
    // option A behind this interface without touching the bubble or the mouth.
    struct AgentEvents
    {
        std::function<void(uint32_t segmentId, const std::string& role,
                           const std::string& text, bool final)> onTranscript;
        std::function<void(const std::string& mode)>  onMode;   // listening|thinking|speaking
        std::function<void(const std::string& message)> onError;
    };

    // level -> mouth, ported from the browser build's vrmcall.js. The gamma
    // below 1 lifts quiet speech so a soft consonant still opens the lips, and
    // the 0.03 floor is what stops the mouth resting slightly parted forever.
    float ShapeMouth(float level, float gain = 1.0f);

    // One smoothing step: 45 ms to open, 140 ms to close, so the mouth snaps
    // onto a word onset and closes over a syllable instead of chattering on
    // every analyser frame.
    float StepMouth(float current, float target, float dt,
                    float attack = 0.045f, float release = 0.14f);

    // --- playback + mouth, main thread only --------------------------------
    class Speaker
    {
    public:
        Speaker();
        ~Speaker();

        void Enqueue(std::vector<int16_t> samples, std::string text);   // any thread
        bool IsPlaying() const;
        size_t Pending() const;
        void Stop();

        // Starts the next queued sentence, advances the mouth one frame, and
        // returns the mouth weight for this frame (0..1, already shaped and
        // smoothed). Called once per frame from the main thread.
        float Update(float dt, float gain);

        bool  Muted() const;
        void  SetMuted(bool muted);
        long long SpokenLines() const;
        float LastPeak() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class CallController
    {
    public:
        CallController();
        ~CallController();

        // Reads .env from `envDirs`, resolves the agent, and (if `withMic`)
        // opens the microphone. Safe to call when the network or the key is
        // missing: it reports why and leaves the tool in text-only mode.
        void Configure(const std::vector<std::string>& envDirs);
        void Attach(const AgentEvents& events);
        void SetAudioSink(std::function<void(std::vector<int16_t>)> sink);

        void Start();
        void Stop();
        bool Ready() const;

        void SendText(const std::string& text);       // clipboard or typed turn
        void SetMicEnabled(bool enabled);
        bool MicEnabled() const;
        bool MicOpen() const;
        float Level() const;

        bool Busy() const;                            // a turn is in flight
        std::string Mode() const;
        std::string StatusText() const;               // what the band shows

        // Main thread, once per frame: forwards queued events to the AgentEvents
        // callbacks and drops a stale error.
        void Tick();

    private:
        void WorkerMain();

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // --- the mouth, on the scene -------------------------------------------
    // Writes the `aa` preset weight. Returns false (once, with a reason) when
    // the model has no `aa` - a VRM 1.0 model may omit any preset, and the
    // engine's own talking code indexes presets[] without checking, so this
    // check is load-bearing rather than defensive.
    bool ApplyMouth(float mouth);
    bool MouthAvailable(std::string& why);

    // REPORT.md section 10.1: print every preset index the avatar actually has.
    void DumpExpressionPresets();

    // Split a reply into speakable sentences: the first audio starts while the
    // rest is still being synthesized, which is most of what makes a turn-based
    // exchange feel alive.
    std::vector<std::string> SplitSentences(const std::string& text);
}
