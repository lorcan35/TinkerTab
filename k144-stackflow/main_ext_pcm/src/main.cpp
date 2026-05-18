/*
 * SPDX-FileCopyrightText: 2026 TinkerTab (TT #131)
 * SPDX-License-Identifier: MIT
 *
 * ext_pcm — host-PCM-injection StackFlow unit for K144.
 *
 * Two ingestion paths feed the same ZMQ PUB at sys_pcm_cap_channel
 * (default ipc:///tmp/llm/pcm.cap.socket):
 *
 *  1. TCP listener on port 9999 (dev-only — for ADB-forwarded testing
 *     from a workstation).  Accepts raw int16 LE 16 kHz mono PCM bytes
 *     and forwards each recv() chunk on the PUB.
 *
 *  2. JSON inference frames over the standard StackFlow wire (the
 *     production path: Tab5 → UART → llm_sys → ext_pcm inference_url).
 *     Frame shape:
 *         {"action":"inference","work_id":"ext_pcm.NNNN",
 *          "object":"audio.pcm.base64",
 *          "data":"<base64-encoded int16 LE 16 kHz mono>"}
 *     setup() attaches a subscriber to our own inference_url that
 *     decodes base64 + forwards on the PUB.
 *
 *  Sequence for ASR consumption (the asr.setup with input=sys.pcm
 *  triggers audio's lazy _cap() which steals our IPC bind):
 *
 *    1. ext_pcm.setup           — register + bind
 *    2. asr.setup input=sys.pcm — audio steals bind; ASR subscribes
 *    3. audio.cap_stop_all      — audio releases its bind
 *    4. ext_pcm.setup           — re-register + rebind (reclaim)
 *    5. inference frames        — flow through to ASR
 *
 *  Each setup() call idempotently:
 *    a. (re)binds pub_ctx_ to sys_pcm_cap_channel
 *    b. (re)attaches the inference subscriber to our channel's bus
 *    c. sends an ACK so callers can resolve the live work_id_num
 */
#include "StackFlow.h"
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using StackFlows::StackFlow;
using StackFlows::pzmq;
using StackFlows::pzmq_data;
using StackFlows::decode_base64;
using StackFlows::sample_get_work_id_num;

static int main_exit_flag = 0;
static void on_sigint(int) { main_exit_flag = 1; }

#define EXT_PCM_TCP_PORT 9999

class llm_ext_pcm : public StackFlow {
private:
    std::string sys_pcm_cap_channel_ = "ipc:///tmp/llm/pcm.cap.socket";
    std::unique_ptr<pzmq> pub_ctx_;
    std::atomic<uint64_t> frames_pumped_{0};
    std::atomic<uint64_t> frames_pumped_tcp_{0};
    std::atomic<uint64_t> frames_pumped_uart_{0};
    std::atomic<bool> tcp_running_{false};
    std::thread tcp_thread_;

    void bind_pub() {
        try {
            pub_ctx_ = std::make_unique<pzmq>(sys_pcm_cap_channel_, ZMQ_PUB);
            std::cerr << "ext_pcm: PUB bound to " << sys_pcm_cap_channel_ << std::endl;
        } catch (const std::exception &e) {
            std::cerr << "ext_pcm: PUB bind failed: " << e.what() << std::endl;
            pub_ctx_.reset();
        }
    }

    // Handler attached to OUR OWN inference subscriber.  Receives
    // (object, data) pairs from llm_sys's inference routing.  data is
    // base64-encoded PCM; decode + push on the PUB.
    void on_inference_frame(const std::string &/*object*/, const std::string &b64) {
        if (!pub_ctx_) return;
        std::string pcm;
        if (decode_base64(b64, pcm) <= 0 || pcm.empty()) {
            // Treat as raw if decode failed
            pub_ctx_->send_data(b64.data(), (int)b64.size());
            frames_pumped_uart_++;
            frames_pumped_++;
            return;
        }
        pub_ctx_->send_data(pcm.data(), (int)pcm.size());
        if (++frames_pumped_uart_ % 100 == 0) {
            std::cerr << "ext_pcm uart: pumped " << frames_pumped_uart_
                      << " frames (" << pcm.size() << "B last)" << std::endl;
        }
        frames_pumped_++;
    }

public:
    llm_ext_pcm() : StackFlow("ext_pcm")
    {
        rpc_ctx_->register_rpc_action(
            "cap",
            std::bind(&llm_ext_pcm::cap_action, this, std::placeholders::_1, std::placeholders::_2));
        rpc_ctx_->register_rpc_action(
            "cap_stop",
            std::bind(&llm_ext_pcm::cap_stop_action, this, std::placeholders::_1, std::placeholders::_2));
        // Custom RPC actions — reachable via the daemon's remote_call path
        // for any JSON envelope with action != "inference":
        //   {"work_id":"ext_pcm","action":"rebind"} — reclaim the URL
        //   {"work_id":"ext_pcm","action":"push","data":"<base64>"} — pump PCM
        rpc_ctx_->register_rpc_action(
            "rebind",
            std::bind(&llm_ext_pcm::rebind_action, this, std::placeholders::_1, std::placeholders::_2));
        rpc_ctx_->register_rpc_action(
            "ingest",
            std::bind(&llm_ext_pcm::push_action, this, std::placeholders::_1, std::placeholders::_2));
        bind_pub();
        tcp_running_ = true;
        tcp_thread_ = std::thread([this]() { this->tcp_listener_loop(); });
    }

    ~llm_ext_pcm() {
        tcp_running_ = false;
        if (tcp_thread_.joinable()) tcp_thread_.join();
    }

    void tcp_listener_loop() {
        int srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) { std::cerr << "ext_pcm tcp: socket() failed\n"; return; }
        int yes = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(EXT_PCM_TCP_PORT);
        if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "ext_pcm tcp: bind " << EXT_PCM_TCP_PORT << " failed\n";
            close(srv); return;
        }
        if (listen(srv, 1) < 0) { close(srv); return; }
        std::cerr << "ext_pcm tcp: listening on port " << EXT_PCM_TCP_PORT << std::endl;

        while (tcp_running_) {
            int cli = accept(srv, nullptr, nullptr);
            if (cli < 0) continue;
            std::cerr << "ext_pcm tcp: client connected" << std::endl;
            char buf[8192];
            while (tcp_running_) {
                ssize_t n = recv(cli, buf, sizeof(buf), 0);
                if (n <= 0) break;
                if (pub_ctx_) {
                    pub_ctx_->send_data(buf, (int)n);
                    if (++frames_pumped_tcp_ % 100 == 0) {
                        std::cerr << "ext_pcm tcp: pumped " << frames_pumped_tcp_
                                  << " chunks (" << n << "B last)" << std::endl;
                    }
                    frames_pumped_++;
                }
            }
            close(cli);
            std::cerr << "ext_pcm tcp: client disconnected" << std::endl;
        }
        close(srv);
    }

    int setup(const std::string &work_id, const std::string &/*object*/,
              const std::string &data) override
    {
        nlohmann::json body;
        try { body = nlohmann::json::parse(data); } catch (...) { body = nlohmann::json::object(); }
        if (body.contains("sys_pcm_cap_channel") && body["sys_pcm_cap_channel"].is_string()) {
            std::string new_url = body["sys_pcm_cap_channel"];
            if (new_url != sys_pcm_cap_channel_) sys_pcm_cap_channel_ = new_url;
        }

        // Always rebind the PUB.  Audio's lazy _cap() on asr.setup steals
        // /tmp/llm/pcm.cap.socket from our boot-time bind, so callers
        // do {ext_pcm.setup; asr.setup; audio.cap_stop_all; ext_pcm.setup}
        // to reclaim ownership.
        pub_ctx_.reset();
        bind_pub();
        if (!pub_ctx_) {
            nlohmann::json err;
            err["code"]    = -19;
            err["message"] = "ext_pcm: PUB bind failed";
            send("None", "None", err, work_id);
            return -1;
        }

        // Attach the inference subscriber for THIS work_id's channel.
        // subscriber_work_id("", callback) subscribes to our own
        // inference_url_, receiving every "inference" frame the daemon
        // routes to us.
        auto llm_channel = get_channel(work_id);
        if (llm_channel) {
            llm_channel->subscriber_work_id(
                "",
                std::bind(&llm_ext_pcm::on_inference_frame, this,
                          std::placeholders::_1, std::placeholders::_2));
        }

        send(LLM_NONE, LLM_NONE, LLM_NO_ERROR, work_id);
        std::cerr << "ext_pcm: setup ack work_id=" << work_id
                  << " url=" << sys_pcm_cap_channel_ << std::endl;
        return 0;
    }

    int exit(const std::string &, const std::string &, const std::string &) override
    {
        pub_ctx_.reset();
        std::cerr << "ext_pcm: exit after " << frames_pumped_ << " frames (tcp="
                  << frames_pumped_tcp_ << " uart=" << frames_pumped_uart_ << ")" << std::endl;
        return 0;
    }

    void link(const std::string &, const std::string &, const std::string &) override {}
    void unlink(const std::string &, const std::string &, const std::string &) override {}
    void taskinfo(const std::string &, const std::string &, const std::string &) override {}
    void pause(const std::string &, const std::string &, const std::string &) override {}

    std::string cap_action(pzmq *, const std::shared_ptr<pzmq_data> &) {
        return sys_pcm_cap_channel_;
    }

    std::string cap_stop_action(pzmq *, const std::shared_ptr<pzmq_data> &) {
        return LLM_NONE;
    }

    // Custom RPC: rebind — reclaim ownership of sys_pcm_cap_channel_.
    // Tab5 calls this after audio.cap_stop_all to reclaim the URL without
    // restarting the ext_pcm process.
    std::string rebind_action(pzmq *, const std::shared_ptr<pzmq_data> &) {
        pub_ctx_.reset();
        bind_pub();
        std::cerr << "ext_pcm: rebind requested; pub=" << (pub_ctx_ ? "ok" : "fail") << std::endl;
        return sys_pcm_cap_channel_;
    }

    // Custom RPC: push — production PCM ingestion path.
    // Wire frame from Tab5 over UART (routed via llm_sys → ext_pcm RPC):
    //   {"work_id":"ext_pcm","action":"push","data":"<base64 16k mono int16>"}
    // The daemon's remote_call sends (com_url, full_json) as a 2-param
    // pzmq_data.  We parse the data field, base64-decode, publish.
    std::string push_action(pzmq *, const std::shared_ptr<pzmq_data> &raw) {
        if (!pub_ctx_) return std::string("no_pub");
        // remote_call passes (com_url, full_json) as the two pzmq_data params.
        std::string payload = raw->get_param(1);
        if (payload.empty()) payload = raw->string();
        std::string b64;
        try {
            auto j = nlohmann::json::parse(payload);
            if (j.contains("data") && j["data"].is_string()) {
                b64 = j["data"].get<std::string>();
            }
        } catch (...) {}
        if (b64.empty()) {
            std::cerr << "ext_pcm: ingest no_data (payload len=" << payload.size() << ")" << std::endl;
            return std::string("no_data");
        }
        std::string pcm;
        if (decode_base64(b64, pcm) <= 0 || pcm.empty()) {
            std::cerr << "ext_pcm: ingest decode_err (b64 len=" << b64.size() << ")" << std::endl;
            return std::string("decode_err");
        }
        pub_ctx_->send_data(pcm.data(), (int)pcm.size());
        if (++frames_pumped_uart_ % 100 == 0) {
            std::cerr << "ext_pcm uart: pumped " << frames_pumped_uart_
                      << " frames (" << pcm.size() << "B last)" << std::endl;
        }
        frames_pumped_++;
        return std::string("ok");
    }
};

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    llm_ext_pcm srv;
    while (!main_exit_flag) sleep(1);
    return 0;
}
