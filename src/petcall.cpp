#include "petcall.h"
#include "config.h"
#include "voice.h"
#include "util.h"

// The heavier engine include lives in this translation unit, not in petcall.h, so
// main.cpp and the bubble do not pay for it.
#include "WickedEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <thread>

namespace vrmpet
{
    // -----------------------------------------------------------------------
    // Mouth maths - ported from web/js/vrmcall.js
    // -----------------------------------------------------------------------
    float ShapeMouth(float level, float gain)
    {
        const float lin = std::clamp(level * gain, 0.0f, 1.0f);
        const float shaped = std::pow(lin, 0.6f);   // lift quiet speech
        return shaped < 0.03f ? 0.0f : shaped;      // hard floor: silence is a shut mouth
    }

    float StepMouth(float current, float target, float dt, float attack, float release)
    {
        const float tau = (target > current) ? attack : release;
        const float k = 1.0f - std::exp(-std::max(0.0f, dt) / tau);
        return current + (target - current) * k;
    }

    namespace
    {
        // .env is read a line at a time, so a prompt that wants a line break has
        // to ask for one. This is the whole escape vocabulary - three sequences,
        // which is enough to lay a prompt out and small enough to hold in the
        // head, and an unknown escape is left exactly as written.
        std::string Unescape(const std::string& text)
        {
            std::string out;
            out.reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '\\' && i + 1 < text.size())
                {
                    const char next = text[i + 1];
                    if (next == 'n')  { out += '\n'; ++i; continue; }
                    if (next == 't')  { out += '\t'; ++i; continue; }
                    if (next == '\\') { out += '\\'; ++i; continue; }
                }
                out += text[i];
            }
            return out;
        }
    }

    // -----------------------------------------------------------------------
    // Speaker
    // -----------------------------------------------------------------------
    struct Speaker::Impl
    {
        struct Segment
        {
            std::vector<int16_t> samples;
            std::vector<uint8_t> wav;
            wi::audio::Sound sound;
            wi::audio::SoundInstance instance;
            std::string text;
            double startedAt = 0.0;
            int      framesObserved = 0;
            uint64_t lastCursor = 0;     // to detect a voice that never starts
            double   lastMoveAt = 0.0;
            bool     warnedStalled = false;
        };

        std::mutex mutex;
        std::deque<std::pair<std::vector<int16_t>, std::string>> queue;
        std::unique_ptr<Segment> current;
        float mouth = 0.0f;
        float lastPeak = 0.0f;
        float sentencePeak = 0.0f;
        bool muted = false;
        long long spoken = 0;
    };

    Speaker::Speaker() : impl_(std::make_unique<Impl>()) {}
    Speaker::~Speaker() { Stop(); }

    void Speaker::Enqueue(std::vector<int16_t> samples, std::string text)
    {
        if (samples.empty())
            return;
        std::scoped_lock lock(impl_->mutex);
        impl_->queue.emplace_back(std::move(samples), std::move(text));
    }

    bool Speaker::IsPlaying() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->current != nullptr;
    }

    size_t Speaker::Pending() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->queue.size();
    }

    void Speaker::Stop()
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->current != nullptr)
        {
            if (impl_->current->instance.IsValid())
                wi::audio::Stop(&impl_->current->instance);
            impl_->current.reset();
        }
        impl_->queue.clear();
        impl_->mouth = 0.0f;
    }

    bool Speaker::Muted() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->muted;
    }

    void Speaker::SetMuted(bool muted)
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->muted = muted;
    }

    long long Speaker::SpokenLines() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->spoken;
    }

    float Speaker::LastPeak() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->lastPeak;
    }

    float Speaker::Update(float dt, float gain)
    {
        std::scoped_lock lock(impl_->mutex);
        Impl& self = *impl_;

        auto startNext = [&]() -> bool
        {
            while (!self.queue.empty())
            {
                auto item = std::move(self.queue.front());
                self.queue.pop_front();

                if (self.muted || item.first.empty())
                    continue;

                auto segment = std::make_unique<Impl::Segment>();
                segment->samples = std::move(item.first);
                segment->text = std::move(item.second);
                segment->wav = WrapPcm16AsWav(segment->samples, 16000);

                // Our own header over our own samples, so this cannot fail - but
                // it is the same check a network WAV would have to pass, so it
                // runs here too rather than only being proven by inspection.
                std::string why;
                if (!SafeToPlay(segment->wav.data(), segment->wav.size(), why))
                {
                    Log("VRMPet: refusing to play a WAV we built ourselves: " + why);
                    continue;
                }

                if (!wi::audio::CreateSound(segment->wav.data(), segment->wav.size(), &segment->sound))
                {
                    Log("VRMPet: the engine could not decode a " + std::to_string(segment->wav.size())
                        + "-byte wav we built - this sentence is silent");
                    continue;
                }
                if (!wi::audio::CreateSoundInstance(&segment->sound, &segment->instance))
                {
                    Log("VRMPet: could not create a sound instance");
                    continue;
                }

                // No SoundComponent is created anywhere, on purpose. Attaching
                // one to the expression entity would switch the engine's own
                // talking system on - it looks for a SoundComponent on the same
                // entity as the ExpressionComponent - and that system writes the
                // mouth weights itself with a fixed 0.4 lerp, which would fight
                // the continuous drive below. Driving the instance directly keeps
                // it inert. SoundInstance::_flags defaults to EMPTY, so unlike
                // SoundComponent there is no LOOPED default to switch off.
                // What the engine thinks it decoded, next to what we handed it.
                // A mismatch here is the difference between "the voice is quiet"
                // and "the container was misread", and wiAudio re-tags every fmt
                // chunk as PCM, so a silent failure is the expected one.
                const wi::audio::SampleInfo info = wi::audio::GetSampleInfo(&segment->sound);
                Log("VRMPet: engine decoded " + std::to_string(info.sample_count) + " samples at "
                    + std::to_string(info.sample_rate) + " Hz, " + std::to_string(info.channel_count)
                    + " ch; we handed it " + std::to_string(segment->samples.size())
                    + " at 16000 Hz, 1 ch, " + std::to_string(segment->wav.size()) + " bytes");

                wi::audio::Play(&segment->instance);
                segment->startedAt = NowSeconds();

                Log("VRMPet: speaking " + Fmt(float(segment->samples.size()) / 16000.0f, 2) + "s"
                    + (segment->text.empty() ? "" : " - \"" + segment->text.substr(0, 120) + "\""));
                self.current = std::move(segment);
                return true;
            }
            return false;
        };

        float target = 0.0f;
        for (int guard = 0; guard < 64; ++guard)
        {
            if (self.current == nullptr && !startNext())
                break;

            Impl::Segment& segment = *self.current;

            // The engine's cursor is the clock; the samples are ours, so the
            // mouth reads the buffer we built rather than re-deriving it from
            // the engine's decode of the file we just wrote.
            const uint64_t cursor = wi::audio::GetTotalSamplesPlayed(&segment.instance);
            const double now = NowSeconds();
            if (cursor > segment.lastCursor)
            {
                segment.lastCursor = cursor;
                segment.lastMoveAt = now;
            }

            // Lead the audio by ~25 ms: the output device has its own latency and
            // the eye forgives a lead far more than a lag.
            const uint64_t readAt = cursor + 400;
            target = ShapeMouth(PeakLevel(segment.samples, size_t(readAt), 64) * gain);
            if (target < 0.004f)
                target = 0.0f;   // silence is a closed mouth, not a nearly closed one
            self.lastPeak = target;
            if (target > self.sentencePeak)
                self.sentencePeak = target;

            // IsEnded() alone is not usable here, and this is not a nitpick:
            // SoundInstanceInternal::ended is initialised to TRUE (wiAudio.cpp:225)
            // and only becomes false when the voice's OnBufferStart fires, which
            // happens on XAudio2's own thread once the device starts pulling
            // audio. Read on the first frame after Play() it therefore says
            // "ended" while nothing has played at all, and the reply is dropped
            // before a syllable of it is heard. The cursor is the honest signal;
            // IsEnded() is only meaningful once the cursor has moved.
            const bool playingNow = cursor > 0;
            // The stall rule applies only while the cursor has never moved at all:
            // once it has, the device is pulling audio and the end of the buffer is
            // the only thing that ends the sentence. (Applying it unconditionally
            // truncated the first sentence tried here to 1.5 s.)
            const bool neverStarted = segment.lastMoveAt == 0.0 && now - segment.startedAt > 1.5;
            const bool ended = (playingNow && (wi::audio::IsEnded(&segment.instance)
                                               || readAt >= segment.samples.size()))
                || neverStarted;

            // So the first frame or two report the real state rather than a
            // guess, and a reply really became audible or really did not.
            if (segment.framesObserved < 4)
            {
                ++segment.framesObserved;
                Log("VRMPet: cursor " + std::to_string(cursor) + "/"
                    + std::to_string(segment.samples.size()) + " after "
                    + Fmt(float(now - segment.startedAt), 2) + "s, engine ended="
                    + std::to_string(wi::audio::IsEnded(&segment.instance) ? 1 : 0)
                    + ", mouth " + Fmt(target, 3));
            }

            if (!ended)
                break;

            if (!playingNow && !segment.warnedStalled)
            {
                segment.warnedStalled = true;
                Log("VRMPet: WARNING - the engine never played this sentence. The sound "
                    "decoded and the instance was created, so the output device is the "
                    "suspect: check that Windows has an active playback endpoint.");
            }

            // Ended: the next sentence starts in this same frame, so a reply
            // does not gain a frame of silence between every sentence.
            wi::audio::Stop(&segment.instance);
            ++self.spoken;
            // The mouth is the one part of this pipeline that cannot be seen in
            // any log by default, so the peak it reached is reported per
            // sentence: a peak of 0.00 while audio was audible means the face
            // never moved, which is a bug this line would otherwise hide.
            Log("VRMPet: sentence " + std::to_string(self.spoken) + " spoken, mouth peak "
                + Fmt(self.sentencePeak, 3) + " -> weight " + Fmt(self.sentencePeak * 0.5f, 3));
            self.sentencePeak = 0.0f;
            self.current.reset();
            target = 0.0f;
        }

        self.mouth = StepMouth(self.mouth, target, std::max(dt, 0.0f));
        return self.mouth;
    }

    // -----------------------------------------------------------------------
    // The mouth, in the scene
    // -----------------------------------------------------------------------
    bool MouthAvailable(std::string& why)
    {
        wi::scene::Scene& scene = wi::scene::GetScene();
        if (scene.expressions.GetCount() == 0)
        {
            why = "the avatar has no ExpressionComponent";
            return false;
        }
        wi::scene::ExpressionComponent& ex = scene.expressions[0];
        const int index = ex.presets[size_t(wi::scene::ExpressionComponent::Preset::Aa)];
        if (index < 0 || index >= int(ex.expressions.size()))
        {
            // VRM 1.0 makes every preset optional. The engine's own talking path
            // indexes presets[phoneme] with no check at all (wiScene.cpp:3021),
            // which is an out-of-bounds read for a model missing a vowel - so
            // this check is the difference between a mouth and a crash.
            why = "the model has no `aa` preset";
            return false;
        }
        why.clear();
        return true;
    }

    bool ApplyMouth(float mouth)
    {
        static bool warned = false;
        std::string why;
        if (!MouthAvailable(why))
        {
            if (!warned)
            {
                warned = true;
                Log("VRMPet: no mouth available - " + why);
            }
            return false;
        }

        wi::scene::Scene& scene = wi::scene::GetScene();
        wi::scene::ExpressionComponent& ex = scene.expressions[0];
        const int index = ex.presets[size_t(wi::scene::ExpressionComponent::Preset::Aa)];

        // Half the mouth value while the face is neutral, a quarter under an
        // emotion: an expression already moves the face and the two would
        // otherwise add up. This build applies no emotions, so it is always half.
        ex.expressions[size_t(index)].SetWeight(mouth * 0.5f);

        // The face is the one part of this pipeline that nothing else can
        // confirm, so it is reported from the *mesh*: the engine's expression
        // system copies the weight onto every morph target the expression binds
        // to, and that copy is what actually deforms the face. A mouth weight
        // that never appears there means the expression is set and the mesh is
        // not moving, which is exactly the failure this line would otherwise
        // hide. Once a second, only while the mouth is open.
        static double lastReport = -10.0;
        const double now = NowSeconds();
        if (mouth > 0.05f && now - lastReport > 1.0)
        {
            lastReport = now;
            float maxMorph = 0.0f;
            size_t morphed = 0;
            for (size_t m = 0; m < scene.meshes.GetCount(); ++m)
            {
                for (const wi::scene::MeshComponent::MorphTarget& target : scene.meshes[m].morph_targets)
                {
                    if (target.weight > maxMorph)
                        maxMorph = target.weight;
                    if (target.weight > 0.001f)
                        ++morphed;
                }
            }
            Log("VRMPet: mouth " + Fmt(mouth, 3) + " -> expression weight "
                + Fmt(mouth * 0.5f, 3) + ", mesh morph targets driven: "
                + std::to_string(morphed) + " (max weight " + Fmt(maxMorph, 3) + ")");
        }
        return true;
    }

    void DumpExpressionPresets()
    {
        wi::scene::Scene& scene = wi::scene::GetScene();
        if (scene.expressions.GetCount() == 0)
        {
            Log("VRMPet: presets - the avatar has no ExpressionComponent");
            return;
        }

        wi::scene::ExpressionComponent& ex = scene.expressions[0];
        Log("VRMPet: presets - " + std::to_string(ex.expressions.size()) + " expressions, "
            + std::to_string(sizeof(ex.presets) / sizeof(ex.presets[0])) + " preset slots");

        static const char* names[] = {
            "Happy", "Angry", "Sad", "Relaxed", "Surprised",
            "Aa", "Ih", "Ou", "Ee", "Oh",
            "Blink", "BlinkLeft", "BlinkRight",
            "LookUp", "LookDown", "LookLeft", "LookRight", "Neutral",
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        {
            const int index = ex.presets[i];
            std::string line = std::string("VRMPet:   preset ") + names[i] + " -> "
                + (index < 0 ? std::string("(absent)") : std::to_string(index));
            if (index >= 0 && index < int(ex.expressions.size()))
                line += "  '" + ex.expressions[size_t(index)].name + "'";
            Log(line);
        }
    }

    // -----------------------------------------------------------------------
    // Sentence splitting
    // -----------------------------------------------------------------------
    std::vector<std::string> SplitSentences(const std::string& text)
    {
        std::vector<std::string> chunks;
        std::string current;

        auto flush = [&]()
        {
            const std::string trimmed = Trim(current);
            if (!trimmed.empty())
                chunks.push_back(trimmed);
            current.clear();
        };

        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            current += c;

            bool boundary = (c == '.' || c == '!' || c == '?' || c == '\n');
            if (!boundary && (unsigned char)c == 0xE2 && i + 2 < text.size())
            {
                // UTF-8 ellipsis: E2 80 A6
                boundary = (unsigned char)text[i + 1] == 0x80 && (unsigned char)text[i + 2] == 0xA6;
                if (boundary)
                {
                    current += text[i + 1];
                    current += text[i + 2];
                    i += 2;
                }
            }
            if (boundary && Trim(current).size() >= 12)
                flush();

            // A paragraph of one sentence still has to be spoken in pieces, or
            // nothing is heard until the whole thing is synthesized.
            if (current.size() >= 240)
            {
                size_t cut = current.rfind(", ", current.size());
                if (cut == std::string::npos || cut < 60)
                    cut = current.rfind(' ', current.size());
                if (cut == std::string::npos || cut < 40)
                    cut = current.size();
                chunks.push_back(Trim(current.substr(0, cut)));
                current = current.substr(cut);
            }
        }
        flush();

        // A one-word fragment at the end ("Right.") reads as a hiccup when it is
        // its own request; fold short tails into the previous chunk.
        if (chunks.size() >= 2 && chunks.back().size() < 12)
        {
            chunks[chunks.size() - 2] += " " + chunks.back();
            chunks.pop_back();
        }
        return chunks;
    }

    // -----------------------------------------------------------------------
    // CallController
    // -----------------------------------------------------------------------
    struct CallController::Impl
    {
        enum class EventKind { Transcript, Mode, Error };

        struct Event
        {
            EventKind   kind = EventKind::Mode;
            uint32_t    segmentId = 0;
            std::string a;
            std::string b;
            bool        final = false;
        };

        FishClient  fish;
        MicCapture  mic;
        AgentEvents events;
        std::function<void(std::vector<int16_t>)> audioSink;

        std::thread       worker;
        std::atomic<bool> run{ false };
        std::atomic<bool> busy{ false };
        std::atomic<bool> micEnabled{ true };
        std::atomic<uint32_t> nextSegment{ 1 };

        // A turn waiting for the worker. `show` is false for a turn the user did
        // not say - the startup greeting - so it reaches the agent as the user's
        // message but never reaches the transcript.
        struct QueuedTurn
        {
            std::string text;
            bool        show = true;
        };

        std::mutex mutex;
        std::deque<std::vector<int16_t>> utterances;
        std::deque<QueuedTurn> typed;
        std::vector<TurnMessage> history;
        std::vector<Event> pending;

        std::string greetPrompt;   // .env GREET, empty when the pet stays quiet
        std::string mode = "idle";
        std::string error;
        double      errorAt = -1000.0;
        bool        fishReady = false;
        std::string fishError;

        void Push(Event event)
        {
            std::scoped_lock lock(mutex);
            pending.push_back(std::move(event));
        }

        void SetMode(const std::string& next)
        {
            {
                std::scoped_lock lock(mutex);
                if (mode == next)
                    return;
                mode = next;
            }
            Event event;
            event.kind = EventKind::Mode;
            event.a = next;
            Push(std::move(event));
        }

        void Fail(const std::string& message)
        {
            {
                std::scoped_lock lock(mutex);
                error = message;
                errorAt = NowSeconds();
            }
            Log("VRMPet: " + message);
            Event event;
            event.kind = EventKind::Error;
            event.a = message;
            Push(std::move(event));
        }

        void HandleTurn(std::vector<int16_t> pcm, std::string text, bool show);
    };

    void CallController::Impl::HandleTurn(std::vector<int16_t> pcm, std::string text, bool show)
    {
        const double turnStart = NowSeconds();
        long long asrMs = 0, agentMs = 0, ttsMs = 0;
        size_t spokenSamples = 0;

        // Half duplex: nothing the microphone hears while the pet is answering
        // belongs to the next turn. Lift it on every exit path.
        mic.SetPaused(true);
        busy = true;
        auto endTurn = [&](const std::string& mode)
        {
            mic.SetPaused(false);
            busy = false;
            SetMode(mode);
        };

        if (!pcm.empty())
        {
            SetMode("transcribing");

            std::string heard;
            FishResult result;
            if (!fish.Asr(pcm, 16000, heard, result))
            {
                endTurn("listening");
                Fail("Could not transcribe the recording: " + result.error);
                return;
            }
            asrMs = result.ms;
            text = Trim(heard);
            Log("VRMPet: heard -> \"" + text + "\"  (asr " + FmtMs(asrMs) + ")");

            if (text.empty())
            {
                // The Windows 11 microphone privacy setting can leave a device
                // enumerable, openable and running while delivering digital
                // silence. There is no error to catch, so the RMS logged by the
                // capture side plus this line are the whole diagnosis.
                endTurn("listening");
                Fail("Speech was transcribed as silence. If the mic is on, check "
                     "Settings > Privacy > Microphone.");
                return;
            }
        }

        if (text.empty())
        {
            endTurn("listening");
            return;
        }

        // A hidden turn still becomes the user's message as far as the agent is
        // concerned - that is what makes it answer - it just is not drawn.
        if (show)
        {
            const uint32_t userSegment = nextSegment.fetch_add(1);
            Event event;
            event.kind = EventKind::Transcript;
            event.segmentId = userSegment;
            event.a = "user";
            event.b = text;
            event.final = true;
            Push(std::move(event));
        }

        history.push_back({ "user", text });
        if (history.size() > 16)
            history.erase(history.begin(), history.begin() + (history.size() - 16));

        // The agent leg is seconds, not milliseconds, so the mode matters: a
        // still avatar for four seconds reads as a hang.
        SetMode("thinking");

        std::string reply;
        FishResult result;
        if (!fish.AgentTurn(history, reply, result))
        {
            endTurn("listening");
            Fail("The agent did not answer: " + result.error);
            return;
        }
        agentMs = result.ms;
        reply = Trim(reply);

        history.push_back({ "agent", reply });
        if (history.size() > 16)
            history.erase(history.begin(), history.begin() + (history.size() - 16));

        const uint32_t replySegment = nextSegment.fetch_add(1);
        {
            Event event;
            event.kind = EventKind::Transcript;
            event.segmentId = replySegment;
            event.a = "agent";
            event.b = reply;
            event.final = true;
            Push(std::move(event));
        }

        SetMode("speaking");

        // Sentence by sentence, so the first audio starts while the rest is
        // still being synthesized.
        const std::vector<std::string> chunks = SplitSentences(reply);
        for (size_t i = 0; i < chunks.size(); ++i)
        {
            std::vector<int16_t> samples;
            FishResult tts;
            if (!fish.Tts(chunks[i], samples, tts))
            {
                Fail("Could not speak sentence " + std::to_string(i + 1) + " of "
                     + std::to_string(chunks.size()) + ": " + tts.error);
                break;
            }
            ttsMs += tts.ms;
            spokenSamples += samples.size();
            if (audioSink)
                audioSink(std::move(samples));
        }

        const double total = NowSeconds() - turnStart;
        Log("VRMPet: turn done - asr " + FmtMs(asrMs)
            + " | agent " + FmtMs(agentMs)
            + " (" + std::to_string(chunks.size()) + " sentence"
            + (chunks.size() == 1 ? "" : "s") + ")"
            + " | tts " + FmtMs(ttsMs)
            + " | " + Fmt(float(spokenSamples) / 16000.0f, 2) + "s of speech"
            + " | " + Fmt(float(total), 2) + "s of network");

        endTurn("listening");
    }

    CallController::CallController() : impl_(std::make_unique<Impl>()) {}
    CallController::~CallController() { Stop(); }

    void CallController::Configure(const std::vector<std::string>& envDirs)
    {
        Impl& self = *impl_;

        self.fishReady = self.fish.Configure(envDirs);
        if (!self.fishReady)
        {
            self.fishError = self.fish.Error();
            Log("VRMPet: call transport unavailable - " + self.fishError
                + "  (the avatar still runs; typed turns need the key)");
            return;
        }

        Log("VRMPet: fish base " + self.fish.Base());
        if (!self.fish.ResolveAgent() || !self.fish.FetchAgentConfig())
        {
            self.fishError = self.fish.Error();
            Log("VRMPet: call transport unavailable - " + self.fishError);
            self.fishReady = false;
            return;
        }

        // What the pet says when it appears is the user's line to write, not
        // this tool's: GREET in config.txt holds it. No GREET means it waits to
        // be spoken to - a pet that talks first is a preference, and one that
        // recites a prompt nobody chose would be worse than silence.
        self.greetPrompt = Unescape(Trim(Settings().Text("GREET")));
        if (self.greetPrompt.empty())
            Log("VRMPet: no GREET in config.txt - the pet waits to be spoken to");
        else
            Log("VRMPet: startup greeting from config.txt GREET ("
                + std::to_string(self.greetPrompt.size()) + " chars)");

        // The mouth's tuning is per-model: a quiet voice needs more gain than a
        // loud one. 1.0 is the studio's default and the design document's
        // starting point.
        self.mode = "listening";
    }

    void CallController::Attach(const AgentEvents& events)
    {
        impl_->events = events;
    }

    void CallController::SetAudioSink(std::function<void(std::vector<int16_t>)> sink)
    {
        impl_->audioSink = std::move(sink);
    }

    bool CallController::Ready() const
    {
        return impl_->fishReady;
    }

    void CallController::Start()
    {
        Impl& self = *impl_;
        if (self.run.load())
            return;

        self.run = true;
        self.worker = std::thread([this]() { WorkerMain(); });

        if (self.micEnabled.load())
        {
            self.mic.Start(
                [this](Utterance utterance)
                {
                    std::scoped_lock lock(impl_->mutex);
                    impl_->utterances.push_back(std::move(utterance.pcm));
                },
                nullptr);
        }
        else
        {
            Log("VRMPet: microphone disabled at startup - typed turns only");
        }
    }

    void CallController::Stop()
    {
        Impl& self = *impl_;
        if (!self.run.load() && !self.worker.joinable())
            return;
        self.run = false;
        self.mic.Stop();
        if (self.worker.joinable())
            self.worker.join();
        self.busy = false;
    }

    void CallController::WorkerMain()
    {
        Impl& self = *impl_;
        Log("VRMPet: call worker started"
            + std::string(self.fishReady ? "" : " (no transport - turns will fail)"));

        while (self.run.load())
        {
            std::vector<int16_t> pcm;
            std::string text;
            bool show = true;
            bool have = false;

            {
                std::scoped_lock lock(self.mutex);
                if (!self.typed.empty())
                {
                    text = std::move(self.typed.front().text);
                    show = self.typed.front().show;
                    self.typed.pop_front();
                    have = true;
                }
                else if (!self.utterances.empty())
                {
                    pcm = std::move(self.utterances.front());
                    self.utterances.pop_front();
                    have = true;
                }
            }

            if (!have)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            if (!self.fishReady)
            {
                self.Fail("No transport: " + (self.fishError.empty() ? "not configured" : self.fishError));
                continue;
            }

            self.HandleTurn(std::move(pcm), std::move(text), show);
        }
    }

    void CallController::SendText(const std::string& text)
    {
        const std::string trimmed = Trim(text);
        if (trimmed.empty())
            return;
        Impl& self = *impl_;
        {
            std::scoped_lock lock(self.mutex);
            self.typed.push_back({ trimmed, true });
        }
        Log("VRMPet: queued a typed turn (" + std::to_string(trimmed.size()) + " chars)");
    }

    void CallController::Greet()
    {
        Impl& self = *impl_;
        if (!self.fishReady)
        {
            Log("VRMPet: no greeting - there is no transport to greet with");
            return;
        }

        if (self.greetPrompt.empty())
        {
            Log("VRMPet: no greeting - GREET is not set in config.txt");
            return;
        }

        // Sent as the user speaking, because that is the only role the agent
        // harness gives us: there is no system message to add and no way to ask
        // the agent for an opening line directly. Whatever GREET holds goes over
        // verbatim - the wording is the user's - and it is the reply, not this,
        // that is spoken and drawn.
        {
            std::scoped_lock lock(self.mutex);
            self.typed.push_back({ self.greetPrompt, false });
        }
        Log("VRMPet: greeting the user from config.txt GREET; only the reply reaches the transcript");
    }

    void CallController::SetMicEnabled(bool enabled)
    {
        Impl& self = *impl_;
        self.micEnabled = enabled;
        self.mic.SetPaused(!enabled);
        Log(std::string("VRMPet: microphone ") + (enabled ? "listening" : "muted"));
    }

    bool CallController::MicEnabled() const { return impl_->micEnabled.load(); }
    bool CallController::MicOpen() const { return impl_->mic.IsOpen(); }
    float CallController::Level() const { return impl_->mic.Level(); }
    bool CallController::Busy() const { return impl_->busy.load(); }

    std::string CallController::Mode() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->mode;
    }

    std::string CallController::StatusText() const
    {
        Impl& self = *impl_;
        std::scoped_lock lock(self.mutex);

        if (!self.error.empty() && NowSeconds() - self.errorAt < 12.0)
            return "!! " + self.error;

        if (!self.fishReady)
            return self.fishError.empty() ? "no transport configured" : self.fishError;

        std::string status = self.mode;
        if (self.busy.load() && status != "thinking" && status != "transcribing")
            status = "thinking";
        if (!self.micEnabled.load())
            status += "  (mic muted)";
        else if (!self.mic.IsOpen())
            status += "  (no mic - paste a message with Ctrl+Shift+V)";
        else if (!self.busy.load() && self.mode == "listening")
            status = "listening";
        return status;
    }

    void CallController::Tick()
    {
        Impl& self = *impl_;

        std::vector<Impl::Event> drained;
        {
            std::scoped_lock lock(self.mutex);
            drained.swap(self.pending);
        }

        for (Impl::Event& event : drained)
        {
            switch (event.kind)
            {
            case Impl::EventKind::Transcript:
                if (self.events.onTranscript)
                    self.events.onTranscript(event.segmentId, event.a, event.b, event.final);
                break;
            case Impl::EventKind::Mode:
                if (self.events.onMode)
                    self.events.onMode(event.a);
                break;
            case Impl::EventKind::Error:
                if (self.events.onError)
                    self.events.onError(event.a);
                break;
            }
        }
    }
}
