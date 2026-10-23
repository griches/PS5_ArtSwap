/*
 * Art Swap - the title's side of the helper (helper/protocol.md).
 *
 * A title cannot see /user/appmeta, so the helper payload does the reading,
 * the downloading and the writing. start() finds it on 127.0.0.1 (9117, or
 * 9127 or 9137 when an earlier helper that crashed still holds a port: on the
 * console a crashed payload is stopped, not ended), or first hands
 * helper/artswapd.elf (in the title folder) to the ELF loader on 9021. Requests then go out on four
 * worker threads, each with its own connection, urgent ones first; their
 * answers come back through pump(), on the thread that draws.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace artswap
{

constexpr const char *kHelperVersion = "0.5.1";

struct Picture
{
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;
};

struct Reply
{
    bool ok = false;
    std::string error; // why not, when !ok
    std::string text;  // a text answer
    Picture picture;   // a picture answer
    // The text as tab-separated rows
    std::vector<std::vector<std::string>> rows() const;
};

class Helper
{
  public:
    enum class State
    {
        starting,
        ready,
        failed,
    };

    Helper(std::string elf_path, int port = 9117, int loader_port = 9021);
    ~Helper();

    void start();
    State state() const
    {
        return state_.load();
    }
    std::string status() const;
    std::string backend() const;   // the helper's network: http2, http1, curl or unavailable
    std::string key_source() const; // user or none
    std::string key_hint() const;   // the key's last four characters
    std::string lan_address() const; // the console's address on the network, when the helper knows it

    // A request; done runs in pump(). A queued request whose generation has
    // been dropped (drop()) is answered with an error without being sent.
    void ask(std::vector<std::string> fields, std::function<void(Reply &)> done, bool urgent = false,
             int generation = 0);
    void drop(int generation);
    // Runs the answers that have arrived (on the drawing thread)
    void pump(int limit = 64);
    // Asks the helper to leave, without waiting
    void bye();

  private:
    struct Job
    {
        std::vector<std::string> fields;
        std::function<void(Reply &)> done;
        int generation = 0;
        Reply reply;
    };

    void launch();
    void work();
    // Hands artswapd.elf to the ELF loader; why says what went wrong
    bool push_elf(std::string &why) const;
    // A worker found no helper: start it again, at most every few seconds
    void revive();
    bool connect_helper(int &fd) const;
    // The port of a helper that answers HELLO (3 s each), filling what it says; 0 for none
    int find_helper(std::string &version);
    bool exchange(int &fd, const std::vector<std::string> &fields, Reply &reply) const;
    void set_status(const std::string &text);

    std::string elf_path_;
    std::atomic<int> port_;
    int base_port_;
    int loader_port_;
    std::atomic<State> state_{State::starting};
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::string status_;
    std::string backend_;
    std::string key_source_;
    std::string key_hint_;
    std::string lan_address_;
    std::deque<Job> urgent_;
    std::deque<Job> queued_;
    std::deque<Job> finished_;
    std::vector<int> dropped_;
    std::vector<std::thread> threads_;
    std::thread launcher_;
    bool stopping_ = false;
    std::mutex revive_lock_;
    std::chrono::steady_clock::time_point last_revive_{};
};

} // namespace artswap
