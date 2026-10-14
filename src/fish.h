// ---------------------------------------------------------------------------
//  vrmpet::fish - the three Fish Audio legs of a turn
//
//  SPEECH -> TEXT      POST /v1/asr            multipart, 16 kHz mono PCM16 wav
//  TEXT  -> REPLY TEXT via the AGENT's own LLM (see AgentTurn below)
//  TEXT  -> SPEECH     POST /v1/tts            format "pcm", headerless s16le
//
//  The key never leaves this translation unit's memory: it is read from .env,
//  put in an Authorization header for the FISH_API_BASE host, and never logged.
//  Log() reports key *names* only.
//
//  Why the middle leg is not a chat-completions call
//  -------------------------------------------------
//  Fish's public API has 39 routes and none of them turns a message into a
//  reply outside a realtime session - checked against the live
//  https://api.fish.audio/openapi.json, not against a local copy. The realtime
//  session is WebRTC, which is what this build exists to avoid, and the agent's
//  LLM (google/gemini-3.5-flash-lite for Malfoid) is not addressable directly.
//
//  What *is* addressable over plain HTTP is the agent's test harness, and a
//  `next_reply` test is exactly "run this conversation against the agent and
//  give me the reply". So a turn is:
//
//    POST /v1/agent/tests                                 create a scratch test
//      carrying the conversation so far        (deleted again at the end)
//    POST /v1/agent/agents/{id}/tests/run                 run it
//    GET  /v1/agent/agents/{id}/test-batches/{batch}      poll until completed
//    DELETE /v1/agent/agents/{id}/tests/{test}            clean up, always
//
//  That means the middle leg really is the agent - its prompt, its voice, its
//  tools, its knowledge - with no extra LLM, no Chromium and no WebRTC. The
//  price is latency (seconds, not milliseconds) and a residual trace: the test
//  is deleted, but the run it produced stays on the account's run list. Both
//  are reported in the tool's own log so the trade is visible rather than
//  silent.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrmpet
{
    struct TurnMessage
    {
        std::string role;   // "user" | "agent"
        std::string text;
    };

    struct FishResult
    {
        bool        ok = false;
        int         status = 0;
        long long   ms = 0;
        std::string error;
    };

    struct FishAgent
    {
        std::string id;
        std::string name;
        std::string prompt;      // system prompt, fetched for the log and for a future LLM leg
        std::string voiceId;     // TTS reference_id
        std::string llmModel;    // what the agent runs on, reported at startup
    };

    class FishClient
    {
    public:
        // Reads .env. Search order is the caller's job: pass every directory
        // that could hold one, nearest first.
        bool Configure(const std::vector<std::string>& envDirs);

        bool Ready() const { return keyOk_; }
        const std::string& Error() const { return error_; }
        const FishAgent&   Agent() const { return agent_; }
        const std::string& Base() const { return base_; }
        bool VoiceAvailable() const { return !agent_.voiceId.empty(); }

        // GET /v1/agent/agents -> the id behind the AGENT name in .env, then
        // GET .../{id}/config for the prompt and the voice.
        bool ResolveAgent();
        bool FetchAgentConfig();

        bool Asr(const std::vector<int16_t>& pcm, int rate, std::string& text, FishResult& out);

        // One turn through the agent. `history` is oldest-first and may hold any
        // number of prior turns; it is sent as the test's conversation.
        bool AgentTurn(const std::vector<TurnMessage>& history, std::string& reply, FishResult& out);

        // Headerless s16le at 16 kHz, wrapped by the caller (never handed to the
        // engine raw - see voice.h).
        bool Tts(const std::string& text, std::vector<int16_t>& samples, FishResult& out);

    private:
        std::string base_ = "https://api.fish.audio";
        std::string key_;
        std::string agentName_;
        std::string ttsModel_ = "drama-3-preview";
        bool        keyOk_ = false;
        std::string error_;
        std::string envPath_;
        FishAgent   agent_;
    };
}
