/*
 * SPDX-FileCopyrightText: 2026 TinkerTab (TT #131)
 * SPDX-License-Identifier: MIT
 *
 * ext_pcm — host-PCM-injection StackFlow unit for K144.
 * Binds sys.pcm ZMQ PUB + a TCP listener on port 9999 for raw PCM
 * bytes from outside the StackFlow daemon (avoiding llm_sys's
 * action-whitelist routing for custom RPCs).
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

static int main_exit_flag = 0;
static void on_sigint(int) { main_exit_flag = 1; }

#define EXT_PCM_TCP_PORT 9999

class llm_ext_pcm : public StackFlow {
private:
    std::string sys_pcm_cap_channel_ = "ipc:///tmp/llm/pcm.cap.socket";
    std::unique_ptr<pzmq> pub_ctx_;
    std::atomic<uint64_t> frames_pumped_{0};
    std::atomic<bool> tcp_running_{false};
    std::thread tcp_thread_;

public:
    llm_ext_pcm() : StackFlow("ext_pcm")
    {
        rpc_ctx_->register_rpc_action(
            "cap",
            std::bind(&llm_ext_pcm::cap_action, this, std::placeholders::_1, std::placeholders::_2));
        rpc_ctx_->register_rpc_action(
            "cap_stop",
            std::bind(&llm_ext_pcm::cap_stop_action, this, std::placeholders::_1, std::placeholders::_2));
        // Auto-bind on startup so it's ready before any setup call.
        try {
            pub_ctx_ = std::make_unique<pzmq>(sys_pcm_cap_channel_, ZMQ_PUB);
            std::cerr << "ext_pcm: PUB pre-bound to " << sys_pcm_cap_channel_ << std::endl;
        } catch (const std::exception &e) {
            std::cerr << "ext_pcm: PUB pre-bind failed: " << e.what() << std::endl;
        }
        // Spawn TCP listener
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
            std::cerr << "ext_pcm tcp: bind " << EXT_PCM_TCP_PORT << " failed\n"; close(srv); return;
        }
        if (listen(srv, 1) < 0) { close(srv); return; }
        std::cerr << "ext_pcm tcp: listening on port " << EXT_PCM_TCP_PORT << std::endl;

        while (tcp_running_) {
            int cli = accept(srv, nullptr, nullptr);
            if (cli < 0) continue;
            std::cerr << "ext_pcm tcp: client connected" << std::endl;
            // Read raw PCM bytes — 16 kHz int16 mono.  Forward each
            // packet to the ZMQ PUB.  Frame size up to caller (3200 B
            // = 100 ms is typical).
            char buf[8192];
            while (tcp_running_) {
                ssize_t n = recv(cli, buf, sizeof(buf), 0);
                if (n <= 0) break;
                if (pub_ctx_) {
                    pub_ctx_->send_data(buf, (int)n);
                    if (++frames_pumped_ % 100 == 0) {
                        std::cerr << "ext_pcm: pumped " << frames_pumped_
                                  << " frames (" << n << "B last)" << std::endl;
                    }
                }
            }
            close(cli);
            std::cerr << "ext_pcm tcp: client disconnected" << std::endl;
        }
        close(srv);
    }

    int setup(const std::string &, const std::string &, const std::string &data) override
    {
        nlohmann::json body;
        try { body = nlohmann::json::parse(data); } catch (...) { body = nlohmann::json::object(); }
        if (body.contains("sys_pcm_cap_channel") && body["sys_pcm_cap_channel"].is_string()) {
            std::string new_url = body["sys_pcm_cap_channel"];
            if (new_url != sys_pcm_cap_channel_) {
                sys_pcm_cap_channel_ = new_url;
                pub_ctx_.reset();
                try {
                    pub_ctx_ = std::make_unique<pzmq>(sys_pcm_cap_channel_, ZMQ_PUB);
                    std::cerr << "ext_pcm: PUB rebound to " << sys_pcm_cap_channel_ << std::endl;
                } catch (const std::exception &e) {
                    std::cerr << "ext_pcm: PUB rebind failed: " << e.what() << std::endl;
                    return -1;
                }
            }
        }
        return 0;
    }

    int exit(const std::string &, const std::string &, const std::string &) override
    {
        pub_ctx_.reset();
        std::cerr << "ext_pcm: exit after " << frames_pumped_ << " frames" << std::endl;
        return 0;
    }

    void link(const std::string &, const std::string &, const std::string &) override {}
    void unlink(const std::string &, const std::string &, const std::string &) override {}
    void taskinfo(const std::string &, const std::string &, const std::string &) override {}
    void pause(const std::string &, const std::string &, const std::string &) override {}
    void work(const std::string &, const std::string &, const std::string &) override {}

    std::string cap_action(pzmq *, const std::shared_ptr<pzmq_data> &) {
        return sys_pcm_cap_channel_;
    }

    std::string cap_stop_action(pzmq *, const std::shared_ptr<pzmq_data> &) {
        return LLM_NONE;
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
