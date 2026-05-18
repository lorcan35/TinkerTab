/*
 * SPDX-FileCopyrightText: 2026 TinkerTab (TT #131)
 * SPDX-License-Identifier: MIT
 *
 * ext_pcm — host-PCM-injection StackFlow unit for K144.
 * Bridges externally-supplied PCM into K144's internal sys.pcm ZMQ bus.
 */
#include "StackFlow.h"
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

using StackFlows::StackFlow;
using StackFlows::pzmq;
using StackFlows::pzmq_data;

static int main_exit_flag = 0;
static void on_sigint(int) { main_exit_flag = 1; }

static std::string base64_decode(const std::string &in)
{
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,
        -1,-1,-1,-1,-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,-1,26,27,28,
        29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
        49,50,51,-1,-1,-1,-1,-1
    };
    std::string out;
    out.reserve((in.size() * 3) / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int8_t v = (c < 128) ? T[c] : -1;
        if (v < 0) {
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            return std::string();
        }
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

class llm_ext_pcm : public StackFlow {
private:
    std::string sys_pcm_cap_channel_ = "ipc:///tmp/llm/pcm.cap.socket";
    std::unique_ptr<pzmq> pub_ctx_;
    std::atomic<uint64_t> frames_pumped_{0};

public:
    llm_ext_pcm() : StackFlow("ext_pcm")
    {
        rpc_ctx_->register_rpc_action(
            "push_pcm",
            std::bind(&llm_ext_pcm::push_pcm, this, std::placeholders::_1, std::placeholders::_2));
        rpc_ctx_->register_rpc_action(
            "cap",
            std::bind(&llm_ext_pcm::cap_action, this, std::placeholders::_1, std::placeholders::_2));
        rpc_ctx_->register_rpc_action(
            "cap_stop",
            std::bind(&llm_ext_pcm::cap_stop_action, this, std::placeholders::_1, std::placeholders::_2));
    }

    int setup(const std::string &work_id, const std::string &object, const std::string &data) override
    {
        (void)work_id; (void)object;
        nlohmann::json body;
        try { body = nlohmann::json::parse(data); } catch (...) { body = nlohmann::json::object(); }
        if (body.contains("sys_pcm_cap_channel") && body["sys_pcm_cap_channel"].is_string()) {
            sys_pcm_cap_channel_ = body["sys_pcm_cap_channel"];
        }
        try {
            pub_ctx_ = std::make_unique<pzmq>(sys_pcm_cap_channel_, ZMQ_PUB);
            std::cerr << "ext_pcm: PUB bound to " << sys_pcm_cap_channel_ << std::endl;
        } catch (const std::exception &e) {
            std::cerr << "ext_pcm: PUB bind failed: " << e.what() << std::endl;
            return -1;
        }
        return 0;
    }

    int exit(const std::string &work_id, const std::string &object, const std::string &data) override
    {
        (void)work_id; (void)object; (void)data;
        pub_ctx_.reset();
        std::cerr << "ext_pcm: exit after " << frames_pumped_ << " frames" << std::endl;
        return 0;
    }

    void link(const std::string &, const std::string &, const std::string &) override {}
    void unlink(const std::string &, const std::string &, const std::string &) override {}
    void taskinfo(const std::string &, const std::string &, const std::string &) override {}
    void pause(const std::string &, const std::string &, const std::string &) override {}
    void work(const std::string &work_id, const std::string &object, const std::string &data) override
    {
        (void)work_id; (void)object;
        if (!pub_ctx_) return;
        std::string raw = base64_decode(data);
        if (raw.empty()) return;
        pub_ctx_->send_data(raw.data(), raw.size());
        ++frames_pumped_;
    }

    // push_pcm RPC — Tab5 sends:
    //   {action: "push_pcm", work_id: "ext_pcm.NNNN", data: "<base64>"}
    std::string push_pcm(pzmq *, const std::shared_ptr<pzmq_data> &rawdata)
    {
        if (!pub_ctx_) return "ext_pcm not setup";
        std::string b64 = rawdata->string();
        std::string raw = base64_decode(b64);
        if (raw.empty()) return "base64 decode failed";
        pub_ctx_->send_data(raw.data(), raw.size());
        if (++frames_pumped_ % 100 == 0) {
            std::cerr << "ext_pcm: pumped " << frames_pumped_ << " frames" << std::endl;
        }
        return LLM_NONE;
    }

    // cap RPC — ASR calls this via unit_call("ext_pcm", "cap", ...)
    std::string cap_action(pzmq *, const std::shared_ptr<pzmq_data> &)
    {
        return sys_pcm_cap_channel_;
    }

    std::string cap_stop_action(pzmq *, const std::shared_ptr<pzmq_data> &)
    {
        pub_ctx_.reset();
        return LLM_NONE;
    }
};

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    llm_ext_pcm srv;
    while (!main_exit_flag) {
        sleep(1);
    }
    return 0;
}
