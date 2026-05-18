/**
 * @file main.cpp
 * @brief ext_pcm — host-PCM-injection StackFlow unit for K144 (TT #131).
 *
 * Bridges externally-supplied PCM (e.g. Tab5's ES7210 mic over UART) into
 * K144's internal `sys.pcm` audio bus so the existing `llm_asr`
 * (sherpa-ncnn streaming zipformer) consumes it as if it came from K144's
 * own hardware mic.
 *
 * Architecture (per research 2026-05-18 — see PLAN-tab5-mic-to-k144-ext-pcm.md
 * in TinkerTab/docs/):
 *
 *   Tab5 mic → UART → llm_sys (existing JSON bridge) → routes by work_id
 *                  → THIS UNIT'S inference_url ZMQ socket
 *                  → we publish bytes onto sys.pcm ZMQ PUB
 *                  → llm_asr subscribes to sys.pcm, transcribes our audio
 *
 * Critical: the audio unit (`llm_audio`) is normally the binder of the
 * `sys.pcm` ZMQ PUB at `ipc:///tmp/llm/pcm.cap.socket`.  Two binders
 * is illegal — only one process can own a given ipc:// or tcp:// endpoint.
 *
 * Coordination strategy:
 *   - On setup, this unit binds the PUB endpoint FIRST (before audio.setup
 *     would).  If audio.setup later tries to bind the same URL its bind
 *     fails — audio's _cap() loop still runs (reads from HW mic) but the
 *     PUB it would push to is now ours.
 *   - Caller (Tab5) is responsible for NOT calling audio.setup with the
 *     default URL while we're alive.  If user picks wake_src=ext_pcm,
 *     Tab5-side voice_onboard skips both audio.setup and the regular
 *     K144 wakeword chain — only ext_pcm + asr.setup.
 *   - To avoid the binder conflict if audio.setup ran first (e.g. from a
 *     stale prior session), our setup probes whether the URL is already
 *     bound and returns an error code the caller can handle (call
 *     audio.exit, then retry).
 *
 * Wire shape — incoming inference frames from llm_sys:
 *   {"action":"inference",
 *    "work_id":"ext_pcm.NNNN",
 *    "object":"audio.pcm.base64",
 *    "data":"<base64 of int16 LE PCM at 16 kHz mono>"}
 *
 * Each frame is base64-decoded and forwarded to the PUB socket as a
 * single ZMQ message.  llm_asr consumes the same byte stream it would
 * have gotten from the HW mic — same sample rate, same int16 LE
 * encoding, no extra framing.
 *
 * NB: This source file is intended to drop into M5Stack's StackFlow
 * monorepo at `projects/llm_framework/main_ext_pcm/src/main.cpp`.  It
 * uses M5Stack's pzmq + StackFlow base class headers from
 * `ext_components/StackFlow/`, which only exist in their build env.
 * Source is checked into TinkerTab/k144-stackflow/ for tracking; the
 * build flow is documented in PLAN-tab5-mic-to-k144-ext-pcm.md.
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "StackFlow.h"        // M5Stack base class — provides RPC plumbing
#include "channel.h"          // pzmq_channel for ZMQ wrappers
#include "pzmq.hpp"           // M5Stack's ZMQ helper

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace StackFlows {

// ───────────────────────────────────────────────────────────────────
// Helpers — base64 decode is the only "new" code needed; everything
// else mirrors main_audio.
// ───────────────────────────────────────────────────────────────────

static const std::string DEFAULT_SYS_PCM_URL = "ipc:///tmp/llm/pcm.cap.socket";

// Minimal base64 decoder — no third-party dep.  Input is the body of
// the JSON `data` field.  Caller owns out_bytes.
static bool base64_decode(const std::string &in, std::string &out) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,
        -1,-1,-1,-1,-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,-1,26,27,28,
        29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
        49,50,51,-1,-1,-1,-1,-1,
        // rest default-init to -1
    };
    out.clear();
    out.reserve((in.size() * 3) / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int8_t v = (c < 128) ? T[c] : -1;
        if (v < 0) {
            // skip whitespace silently; reject other invalid chars
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            return false;
        }
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return true;
}

// ───────────────────────────────────────────────────────────────────
// llm_task — the per-work-id state held by the StackFlow base class
// ───────────────────────────────────────────────────────────────────

class ext_pcm_task {
public:
    std::string sys_pcm_cap_channel_ = DEFAULT_SYS_PCM_URL;
    std::unique_ptr<pzmq> pub_ctx_;
    std::atomic<uint64_t> frames_published_{0};

    bool start(const json &setup_body) {
        if (setup_body.contains("sys_pcm_cap_channel")) {
            sys_pcm_cap_channel_ = setup_body["sys_pcm_cap_channel"];
        }
        try {
            pub_ctx_ = std::make_unique<pzmq>(sys_pcm_cap_channel_, ZMQ_PUB);
        } catch (const std::exception &e) {
            std::cerr << "ext_pcm: PUB bind failed on " << sys_pcm_cap_channel_
                      << " — likely audio.setup is still running.  Stop it via "
                      << "audio.exit and retry.  Detail: " << e.what() << "\n";
            return false;
        }
        std::cerr << "ext_pcm: PUB bound to " << sys_pcm_cap_channel_ << "\n";
        return true;
    }

    // Per-inference frame entrypoint.  data is the body the caller sent
    // in the JSON `data` field — base64 of int16 LE 16 kHz mono PCM.
    void inference(const std::string &b64) {
        if (!pub_ctx_) return;
        std::string raw;
        if (!base64_decode(b64, raw)) {
            std::cerr << "ext_pcm: base64 decode failed (size=" << b64.size() << ")\n";
            return;
        }
        // Publish as a single ZMQ message — same shape llm_audio uses.
        pub_ctx_->send(raw.data(), raw.size());
        if (++frames_published_ % 100 == 0) {
            std::cerr << "ext_pcm: pumped " << frames_published_ << " frames\n";
        }
    }

    void stop() {
        pub_ctx_.reset();
        std::cerr << "ext_pcm: PUB closed after " << frames_published_ << " frames\n";
    }
};

// ───────────────────────────────────────────────────────────────────
// StackFlow integration — 7 RPCs.  Pattern lifted from main_audio.
// ───────────────────────────────────────────────────────────────────

class llm_ext_pcm : public StackFlow {
public:
    llm_ext_pcm() : StackFlow("ext_pcm") {}

protected:
    void task_setup(int work_id_num, const std::shared_ptr<llm_channel_obj> &llm_channel, const std::string &data) override {
        json data_body;
        try { data_body = json::parse(data); } catch (...) { data_body = json::object(); }
        auto task = std::make_shared<ext_pcm_task>();
        if (!task->start(data_body)) {
            send("setup", "", LLM_NO_ERROR, work_id_num);
            return;
        }
        task_map_[work_id_num] = task;
        // Subscribe llm_channel to incoming inference frames so they
        // route into our inference handler.
        llm_channel->subscriber_work_id(
            "", [task](const std::shared_ptr<pzmq> &, const std::shared_ptr<pzmq_data> &raw) {
                // raw->string() is the JSON object as a string per
                // StackFlow's existing convention.
                json msg;
                try {
                    msg = json::parse(raw->string());
                } catch (...) {
                    return;
                }
                if (msg.contains("data") && msg["data"].is_string()) {
                    task->inference(msg["data"]);
                } else if (msg.contains("data") && msg["data"].is_object()
                           && msg["data"].contains("data")) {
                    task->inference(msg["data"]["data"]);
                }
            });
        send("setup", "", LLM_NO_ERROR, work_id_num);
    }

    void task_pause(int, const std::shared_ptr<llm_channel_obj> &, const std::string &) override {}

    void task_work(int work_id_num, const std::shared_ptr<llm_channel_obj> &, const std::string &data) override {
        // "work" is the explicit RPC verb for inference-style calls when
        // the caller hits us directly (rather than via subscribe).
        auto it = task_map_.find(work_id_num);
        if (it == task_map_.end()) return;
        try {
            json msg = json::parse(data);
            if (msg.contains("data") && msg["data"].is_string()) {
                it->second->inference(msg["data"]);
            }
        } catch (...) {
            // ignore malformed
        }
    }

    void task_exit(int work_id_num, const std::shared_ptr<llm_channel_obj> &, const std::string &) override {
        auto it = task_map_.find(work_id_num);
        if (it != task_map_.end()) {
            it->second->stop();
            task_map_.erase(it);
        }
        send("exit", "", LLM_NO_ERROR, work_id_num);
    }

    void task_link(int, const std::shared_ptr<llm_channel_obj> &, const std::string &) override {}
    void task_unlink(int, const std::shared_ptr<llm_channel_obj> &, const std::string &) override {}
    void task_taskinfo(int, const std::shared_ptr<llm_channel_obj> &, const std::string &) override {}

private:
    std::unordered_map<int, std::shared_ptr<ext_pcm_task>> task_map_;
};

}  // namespace StackFlows

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    StackFlows::llm_ext_pcm srv;
    srv.run();
    return 0;
}
