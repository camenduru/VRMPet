#include "fish.h"
#include "config.h"
#include "http.h"
#include "util.h"
#include "voice.h"

// nlohmann/json is reachable because CMakeLists.txt puts the engine's Editor/
// directory on the include path - the comment there names json.hpp, and the
// vendored glTF importer uses the same header. No parser is vendored here.
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

using nlohmann::json;

namespace vrmpet
{
    namespace
    {
        std::vector<HttpHeader> Auth(const std::string& key)
        {
            std::vector<HttpHeader> headers;
            headers.push_back({ L"Authorization", Widen("Bearer " + key) });
            headers.push_back({ L"Content-Type", L"application/json" });
            return headers;
        }

        std::vector<HttpHeader> AuthOnly(const std::string& key)
        {
            std::vector<HttpHeader> headers;
            headers.push_back({ L"Authorization", Widen("Bearer " + key) });
            return headers;
        }

        // Every error body this API returns carries its own sentence. Surface
        // that rather than the status code: "402 Insufficient API credit" is
        // actionable, "HTTP 402" is not.
        std::string Describe(const HttpResponse& r)
        {
            if (!r.error.empty())
                return r.error;
            std::string message;
            try
            {
                json body = json::parse(r.body);
                if (body.is_object())
                {
                    if (body.contains("message"))
                        message = body["message"].get<std::string>();
                    else if (body.contains("detail"))
                        message = body["detail"].is_string()
                            ? body["detail"].get<std::string>()
                            : body["detail"].dump();
                }
                else if (body.is_array() && !body.empty() && body[0].contains("msg"))
                {
                    // FastAPI's 422 shape: [{loc, msg, type}, ...]
                    message = body[0]["msg"].get<std::string>();
                }
            }
            catch (...)
            {
                message = r.body.substr(0, 160);
            }
            return "HTTP " + std::to_string(r.status) + (message.empty() ? "" : " - " + message);
        }

        bool PostJson(const std::string& url, const std::string& key, const json& payload,
                      json& out, FishResult& result)
        {
            HttpResponse response;
            HttpRequest("POST", url, Auth(key), payload.dump(), response);
            result.status = response.status;
            result.ms = response.ms;
            if (!response.Ok())
            {
                result.error = Describe(response);
                return false;
            }
            try
            {
                out = json::parse(response.body);
            }
            catch (const std::exception& e)
            {
                result.error = std::string("could not parse the response: ") + e.what();
                return false;
            }
            result.ok = true;
            return true;
        }

        bool GetJson(const std::string& url, const std::string& key, json& out, FishResult& result)
        {
            HttpResponse response;
            HttpRequest("GET", url, AuthOnly(key), {}, response);
            result.status = response.status;
            result.ms = response.ms;
            if (!response.Ok())
            {
                result.error = Describe(response);
                return false;
            }
            try
            {
                out = json::parse(response.body);
            }
            catch (const std::exception& e)
            {
                result.error = std::string("could not parse the response: ") + e.what();
                return false;
            }
            result.ok = true;
            return true;
        }

        // A stage direction is not speech. "*rolls her eyes* Honestly, Potter."
        // read aloud as "asterisk rolls her eyes asterisk" is the single most
        // obvious way for a talking avatar to sound broken, so the asterisks
        // come out before the text reaches TTS - the bubble still shows them.
        std::string Speakable(const std::string& text)
        {
            std::string out;
            out.reserve(text.size());
            bool inAction = false;
            for (size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                if (c == '*')
                {
                    inAction = !inAction;
                    continue;
                }
                if (!inAction)
                    out += c;
            }
            out = Trim(out);
            // Collapse the doubled spaces an inline removal leaves behind.
            std::string collapsed;
            collapsed.reserve(out.size());
            bool lastSpace = false;
            for (char c : out)
            {
                const bool space = (c == ' ');
                if (space && lastSpace)
                    continue;
                collapsed += c;
                lastSpace = space;
            }
            return collapsed.empty() ? text : collapsed;
        }
    }

    namespace
    {
        bool InList(const std::vector<std::string>& list, const std::string& value)
        {
            return std::find(list.begin(), list.end(), value) != list.end();
        }

        std::string JoinList(const std::vector<std::string>& list)
        {
            std::string out;
            for (const std::string& item : list)
            {
                if (!out.empty())
                    out += ", ";
                out += item;
            }
            return out;
        }
    }

    bool FishClient::FetchTtsModels()
    {
        ttsModels_.clear();
        modelListError_.clear();

        FishResult result;
        json doc;
        if (!GetJson(base_ + "/openapi.json", key_, doc, result))
        {
            modelListError_ = result.error.empty() ? ("HTTP " + std::to_string(result.status))
                                                   : result.error;
            return false;
        }

        // paths -> /v1/tts -> post -> parameters -> the `model` header -> its
        // schema -> enum. Walked defensively: this is somebody else's document
        // and a single missing key must not take the process down with it.
        const json* node = &doc;
        for (const char* field : { "paths", "/v1/tts", "post", "parameters" })
        {
            if (!node->is_object() || !node->contains(field))
            {
                modelListError_ = std::string("the document has no '") + field + "' at that point";
                return false;
            }
            node = &(*node)[field];
        }
        if (!node->is_array())
        {
            modelListError_ = "the parameter list is not a list";
            return false;
        }

        for (const json& parameter : *node)
        {
            if (parameter.value("in", "") != "header" || parameter.value("name", "") != "model")
                continue;
            if (!parameter.contains("schema"))
                continue;
            const json& schema = parameter["schema"];
            if (schema.contains("enum") && schema["enum"].is_array())
            {
                for (const json& item : schema["enum"])
                {
                    if (item.is_string())
                        ttsModels_.push_back(item.get<std::string>());
                }
            }
            break;
        }

        if (ttsModels_.empty())
        {
            modelListError_ = "the `model` header carries no enum";
            return false;
        }
        return true;
    }

    bool FishClient::Configure(const std::vector<std::string>& configDirs)
    {
        Config& config = Settings();
        config.Load(configDirs);

        base_      = config.Text("FISH_API_BASE");
        key_       = config.Text("FISH_API_KEY");
        agentName_ = config.Text("AGENT");

        // MODEL is the TTS voice. An empty value is a deliberate "let Fish
        // choose", which is a different state from "the file says nothing" -
        // hence Default() being consulted rather than a hard fallback here.
        ttsModel_    = config.Text("MODEL");
        modelSource_ = config.Path().empty() ? "built-in default"
                                             : config.Path() + " MODEL";

        // The names Fish accepts are read from the API, not listed here, so a
        // model added later is usable with no rebuild. The check is a warning,
        // never a gate: a name this document has not caught up with is still
        // worth trying, and the send below happens either way.
        if (ttsModel_.empty())
        {
            Log("VRMPet: tts model none - MODEL is set but empty, so no `model` header is sent "
                "and Fish picks its own default");
        }
        else if (FetchTtsModels())
        {
            Log("VRMPet: tts model '" + ttsModel_ + "' from " + modelSource_ + " - fish offers "
                + std::to_string(ttsModels_.size()) + ": " + JoinList(ttsModels_));
            if (!InList(ttsModels_, ttsModel_))
            {
                Log("VRMPet: WARNING - '" + ttsModel_ + "' is not one of those names. Fish answers "
                    "an unknown model with s2.1-pro instead of refusing, so the voice about to be "
                    "used is not the one asked for; note Fish may also have added it after this "
                    "document. Check " + base_ + "/openapi.json");
            }
        }
        else
        {
            Log("VRMPet: tts model '" + ttsModel_ + "' from " + modelSource_
                + " - the list of models could not be read from " + base_ + "/openapi.json ("
                + modelListError_ + "), so the name is sent unchecked");
        }

        // Strip a trailing slash so every path join is one shape.
        while (!base_.empty() && base_.back() == '/')
            base_.pop_back();

        Log(std::string("VRMPet: fish key ") + (key_.empty() ? "MISSING" : "present")
            + ", agent '" + (agentName_.empty() ? std::string("(unset)") : agentName_)
            + "', from " + (config.Path().empty() ? std::string("(no config.txt)") : config.Path()));

        if (key_.empty())
        {
            error_ = "FISH_API_KEY is not set";
            return false;
        }
        if (agentName_.empty())
        {
            error_ = "AGENT is not set";
            return false;
        }
        keyOk_ = true;
        return true;
    }

    bool FishClient::ResolveAgent()
    {
        if (!keyOk_)
            return false;

        FishResult result;
        json body;
        if (!GetJson(base_ + "/v1/agent/agents", key_, body, result))
        {
            error_ = "could not list agents: " + result.error;
            return false;
        }

        const std::string wanted = Lower(agentName_);
        if (body.contains("agents"))
        {
            for (const json& entry : body["agents"])
            {
                const std::string name = entry.value("name", "");
                if (Lower(name) == wanted)
                {
                    agent_.id   = entry.value("agent_id", "");
                    agent_.name = name;
                    Log("VRMPet: agent '" + name + "' -> " + agent_.id);
                    return true;
                }
            }
        }

        std::string available;
        if (body.contains("agents"))
        {
            for (const json& entry : body["agents"])
            {
                if (!available.empty())
                    available += ", ";
                available += entry.value("name", "?");
            }
        }
        error_ = "no agent named '" + agentName_ + "' in this workspace"
            + (available.empty() ? "" : " (found: " + available + ")");
        return false;
    }

    bool FishClient::FetchAgentConfig()
    {
        if (!keyOk_ || agent_.id.empty())
            return false;

        FishResult result;
        json body;
        if (!GetJson(base_ + "/v1/agent/agents/" + agent_.id + "/config", key_, body, result))
        {
            error_ = "could not read the agent config: " + result.error;
            return false;
        }

        if (body.contains("prompt") && body["prompt"].is_object())
            agent_.prompt = body["prompt"].value("system_prompt", "");
        if (body.contains("voice") && body["voice"].is_object())
            agent_.voiceId = body["voice"].value("voice_id", "");
        if (body.contains("llm") && body["llm"].is_object())
            agent_.llmModel = body["llm"].value("model", "");

        // The prompt itself is the agent's, not this tool's, and it can be long.
        // Report its size and its first line rather than dumping it into the log.
        std::string firstLine = agent_.prompt.substr(0, agent_.prompt.find('\n'));
        if (firstLine.size() > 90)
            firstLine = firstLine.substr(0, 90) + "...";
        Log("VRMPet: agent config  voice=" + (agent_.voiceId.empty() ? "(none)" : agent_.voiceId)
            + "  llm=" + (agent_.llmModel.empty() ? "(unknown)" : agent_.llmModel)
            + "  prompt=" + std::to_string(agent_.prompt.size()) + " chars");
        Log("VRMPet: agent prompt begins: " + firstLine);
        return true;
    }

    bool FishClient::Asr(const std::vector<int16_t>& pcm, int rate, std::string& text, FishResult& out)
    {
        out = {};
        if (!keyOk_ || pcm.empty())
        {
            out.error = pcm.empty() ? "no audio captured" : "not configured";
            return false;
        }

        const std::vector<uint8_t> wav = WrapPcm16AsWav(pcm, rate);
        const std::string boundary = NewBoundary();

        std::vector<HttpHeader> headers = AuthOnly(key_);
        headers.push_back({ L"Content-Type", Widen("multipart/form-data; boundary=" + boundary) });

        const std::string body = BoundaryBody(boundary, "audio", "turn.wav", "audio/wav",
                                              std::string(reinterpret_cast<const char*>(wav.data()),
                                                          wav.size()));

        HttpResponse response;
        HttpRequest("POST", base_ + "/v1/asr", headers, body, response);
        out.status = response.status;
        out.ms = response.ms;
        if (!response.Ok())
        {
            out.error = Describe(response);
            return false;
        }

        try
        {
            json parsed = json::parse(response.body);
            text = parsed.value("text", "");
        }
        catch (const std::exception& e)
        {
            out.error = std::string("could not parse the transcript: ") + e.what();
            return false;
        }

        out.ok = true;
        return true;
    }

    bool FishClient::AgentTurn(const std::vector<TurnMessage>& history, std::string& reply, FishResult& out)
    {
        out = {};
        if (!keyOk_ || agent_.id.empty())
        {
            out.error = "no agent resolved";
            return false;
        }
        if (history.empty() || history.back().role != "user")
        {
            out.error = "the conversation must end with a user turn";
            return false;
        }

        const double start = NowSeconds();

        // 1. A scratch test carrying the conversation. `expectation` is required
        //    for next_reply by the API's own validation, and it is the judge's
        //    brief - this tool ignores the verdict and reads the reply, so a
        //    one-line brief is enough.
        json conversation = json::array();
        for (const TurnMessage& message : history)
        {
            std::string text = message.text;
            if (text.size() > 2000)
                text = text.substr(0, 2000);
            conversation.push_back({ {"role", message.role}, {"text", text} });
        }

        json createBody = {
            { "name", "VRMPet turn (auto-delete)" },
            { "test_type", "next_reply" },
            { "conversation", conversation },
            { "expectation", "The agent replies in character." },
            { "agent_ids", json::array({ agent_.id }) },
        };

        FishResult step;
        json created;
        if (!PostJson(base_ + "/v1/agent/tests", key_, createBody, created, step))
        {
            out = step;
            out.error = "could not create the turn: " + step.error;
            return false;
        }
        const std::string testId = created.value("test_id", "");
        if (testId.empty())
        {
            out.error = "the agent test came back without an id";
            return false;
        }

        // Anything between here and the reply has to delete the test, or the
        // agent's test list fills with orphans. A conversation that leaves a
        // trail is exactly the failure the design document warned about.
        bool deleted = false;
        auto cleanup = [&]()
        {
            if (deleted)
                return;
            deleted = true;
            HttpResponse drop;
            HttpRequest("DELETE", base_ + "/v1/agent/agents/" + agent_.id + "/tests/" + testId,
                        AuthOnly(key_), {}, drop);
            if (drop.status != 204 && drop.status != 200)
                Log("VRMPet: WARNING - could not delete the scratch test " + testId
                    + " (HTTP " + std::to_string(drop.status) + ")");
        };

        // 2. Run it.
        json runBody = { { "test_ids", json::array({ testId }) } };
        json batch;
        if (!PostJson(base_ + "/v1/agent/agents/" + agent_.id + "/tests/run", key_, runBody, batch, step))
        {
            cleanup();
            out = step;
            out.error = "could not run the turn: " + step.error;
            return false;
        }
        const std::string batchId = batch.value("batch_id", "");

        // 3. Poll. The batch is queued and finishes in the background; measured
        //    end to end at 6.7 s for a one-line reply, so the ceiling is
        //    generous rather than optimistic.
        const double deadline = NowSeconds() + 90.0;
        json polled;
        bool completed = false;
        while (NowSeconds() < deadline)
        {
            FishResult tick;
            if (!GetJson(base_ + "/v1/agent/agents/" + agent_.id + "/test-batches/" + batchId,
                         key_, polled, tick))
            {
                cleanup();
                out = tick;
                out.error = "could not read the turn result: " + tick.error;
                return false;
            }
            if (polled.value("completed", false))
            {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }

        cleanup();
        out.ms = (long long)((NowSeconds() - start) * 1000.0);

        if (!completed)
        {
            out.error = "the agent never finished the turn (90 s)";
            return false;
        }

        if (!polled.contains("runs") || polled["runs"].empty())
        {
            out.error = "the turn produced no run";
            return false;
        }

        const json& run = polled["runs"][0];
        const std::string status = run.value("status", "");
        reply = run.value("agent_reply", "");
        if (reply.empty())
        {
            out.error = "the turn produced no reply (status '" + status + "'"
                + (run.value("error", "").empty() ? "" : ": " + run.value("error", "")) + ")";
            return false;
        }

        out.ok = true;
        return true;
    }

    bool FishClient::Tts(const std::string& text, std::vector<int16_t>& samples, FishResult& out)
    {
        out = {};
        samples.clear();          // never leave the previous reply's samples in place
        if (!keyOk_ || text.empty())
        {
            out.error = "nothing to say";
            return false;
        }

        json body = {
            { "text", Speakable(text) },
            { "format", "pcm" },          // the default is mp3, which wi::audio cannot decode
            { "sample_rate", 16000 },
            { "latency", "low" },         // this is a conversation turn
            { "chunk_length", 200 },
        };
        if (!agent_.voiceId.empty())
            body["reference_id"] = agent_.voiceId;

        std::vector<HttpHeader> headers = Auth(key_);
        // No header at all when no model is configured, so "let Fish pick" is
        // literally the same request the API documents for that case.
        if (!ttsModel_.empty())
            headers.push_back({ L"model", Widen(ttsModel_) });

        // The whole voice path is logged so a silent reply can be traced: this
        // line says what was asked for, the HTTP line (in HttpRequest) says what
        // came back, and the "engine decoded" line in the speaker says whether it
        // was playable.
        Log("VRMPet: tts request - " + std::to_string(body["text"].get<std::string>().size())
            + " chars, model '" + (ttsModel_.empty() ? std::string("(fish default)") : ttsModel_)
            + "', voice " + (agent_.voiceId.empty() ? std::string("(agent default)") : agent_.voiceId));

        HttpResponse response;
        HttpRequest("POST", base_ + "/v1/tts", headers, body.dump(), response);
        out.status = response.status;
        out.ms = response.ms;
        if (!response.Ok())
        {
            out.error = Describe(response);
            Log("VRMPet: tts failed - " + out.error);
            return false;
        }

        // The endpoint honours format=pcm: the response is headerless s16le at
        // the requested rate, with no RIFF wrapper (verified by byte count
        // against duration x rate x 2). So it is wrapped here, from a sample
        // count we know, instead of trusting a header we did not write - which
        // is the only safe way to reach wi::audio. See voice.h on why.
        if (response.body.size() < 2 || (response.body.size() % 2) != 0)
        {
            // A RIFF header instead of raw samples is the failure to expect if
            // the API ever changes its mind. Reject rather than guess.
            std::string why;
            if (!response.body.empty() && !SafeToPlay(
                    reinterpret_cast<const uint8_t*>(response.body.data()),
                    response.body.size(), why))
            {
                out.error = "the reply is not headerless pcm (" + std::to_string(response.body.size())
                    + " bytes) and not a playable wav: " + why;
                Log("VRMPet: tts failed - " + out.error);
                return false;
            }
            out.error = "the reply is not headerless pcm (" + std::to_string(response.body.size())
                + " bytes)";
            Log("VRMPet: tts failed - " + out.error);
            return false;
        }

        const size_t count = response.body.size() / 2;
        samples.resize(count);
        memcpy(samples.data(), response.body.data(), response.body.size());

        // What came back, as samples and seconds, so a clipped clip is visible
        // here even before it is played.
        Log("VRMPet: tts ok - " + std::to_string(count) + " samples ("
            + Fmt(float(count) / 16000.0f, 2) + "s), "
            + std::to_string(response.body.size()) + " bytes, "
            + std::to_string(response.ms) + " ms");

        out.ok = true;
        return true;
    }
}
