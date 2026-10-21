/*
 * Art Swap - the title's side of the helper (helper_client.hpp).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "helper_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace artswap
{

namespace
{

constexpr int kWorkers = 4;
// where a helper may be listening, from the base port: the first that was free when it started
constexpr int kPortSteps[] = {0, 10, 20};

void log_line(const char *format, const std::string &a = "", const std::string &b = "")
{
    std::fprintf(stderr, "[artswap] ");
    std::fprintf(stderr, format, a.c_str(), b.c_str());
    std::fprintf(stderr, "\n");
}

int open_local(int port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const void *data, std::size_t len)
{
    const auto *p = static_cast<const std::uint8_t *>(data);
    while (len > 0)
    {
        const ssize_t put = send(fd, p, len, 0);
        if (put <= 0)
            return false;
        p += put;
        len -= static_cast<std::size_t>(put);
    }
    return true;
}

bool recv_all(int fd, void *data, std::size_t len)
{
    auto *p = static_cast<std::uint8_t *>(data);
    while (len > 0)
    {
        const ssize_t got = recv(fd, p, len, 0);
        if (got <= 0)
            return false;
        p += got;
        len -= static_cast<std::size_t>(got);
    }
    return true;
}

bool recv_line(int fd, std::string &line)
{
    line.clear();
    char c;
    while (line.size() < 4096)
    {
        if (recv(fd, &c, 1, 0) != 1)
            return false;
        if (c == '\n')
            return true;
        line.push_back(c);
    }
    return false;
}

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

std::vector<std::vector<std::string>> Reply::rows() const
{
    std::vector<std::vector<std::string>> out;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        if (end > start)
        {
            std::vector<std::string> fields;
            std::size_t f = start;
            for (;;)
            {
                const std::size_t tab = text.find('\t', f);
                if (tab == std::string::npos || tab > end)
                {
                    fields.push_back(text.substr(f, end - f));
                    break;
                }
                fields.push_back(text.substr(f, tab - f));
                f = tab + 1;
            }
            out.push_back(std::move(fields));
        }
        start = end + 1;
    }
    return out;
}

Helper::Helper(std::string elf_path, int port, int loader_port)
    : elf_path_(std::move(elf_path)), port_(port), base_port_(port), loader_port_(loader_port)
{
    status_ = "Starting the helper";
}

Helper::~Helper()
{
    {
        std::lock_guard<std::mutex> hold(lock_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (launcher_.joinable())
        launcher_.join();
    for (std::thread &thread : threads_)
        if (thread.joinable())
            thread.join();
}

std::string Helper::status() const
{
    std::lock_guard<std::mutex> hold(lock_);
    return status_;
}

std::string Helper::backend() const
{
    std::lock_guard<std::mutex> hold(lock_);
    return backend_;
}

std::string Helper::key_source() const
{
    std::lock_guard<std::mutex> hold(lock_);
    return key_source_;
}

std::string Helper::key_hint() const
{
    std::lock_guard<std::mutex> hold(lock_);
    return key_hint_;
}

std::string Helper::lan_address() const
{
    std::lock_guard<std::mutex> hold(lock_);
    return lan_address_;
}

void Helper::set_status(const std::string &text)
{
    std::lock_guard<std::mutex> hold(lock_);
    status_ = text;
    log_line("%s", text);
}

void Helper::start()
{
    if (launcher_.joinable())
        launcher_.join();
    state_ = State::starting;
    launcher_ = std::thread([this] { launch(); });
}

bool Helper::connect_helper(int &fd) const
{
    fd = open_local(port_.load());
    if (fd < 0)
        return false;
    timeval timeout{120, 0}; // an APPLY of four 4K backgrounds stays well inside
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return true;
}

bool Helper::exchange(int &fd, const std::vector<std::string> &fields, Reply &reply) const
{
    std::string line;
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (i)
            line.push_back('\t');
        for (char c : fields[i])
            line.push_back(c == '\t' || c == '\n' || c == '\r' ? ' ' : c);
    }
    line.push_back('\n');
    if (!send_all(fd, line.data(), line.size()))
        return false;
    std::string head;
    if (!recv_line(fd, head))
        return false;
    reply = Reply{};
    if (head.rfind("ERR ", 0) == 0)
    {
        reply.error = head.substr(4);
        return true;
    }
    unsigned long long length = 0;
    int w = 0, h = 0;
    if (std::sscanf(head.c_str(), "OK %llu %d %d", &length, &w, &h) != 3 || length > (512ull << 20))
        return false;
    if (w > 0 && h > 0)
    {
        if (length != static_cast<unsigned long long>(w) * h * 4)
            return false;
        reply.picture.w = w;
        reply.picture.h = h;
        reply.picture.rgba.resize(length);
        if (length && !recv_all(fd, reply.picture.rgba.data(), length))
            return false;
    }
    else
    {
        reply.text.resize(length);
        if (length && !recv_all(fd, reply.text.data(), length))
            return false;
    }
    reply.ok = true;
    return true;
}

bool Helper::push_elf(std::string &why) const
{
    std::ifstream file(elf_path_, std::ios::binary);
    std::string elf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (elf.empty())
    {
        why = "The helper (" + elf_path_ + ") is missing from the title folder";
        return false;
    }
    const int loader = open_local(loader_port_);
    if (loader < 0)
    {
        why = "No ELF loader is listening on port " + std::to_string(loader_port_) +
              ": start your loader (etaHEN or elfldr), then try again";
        return false;
    }
    const bool sent = send_all(loader, elf.data(), elf.size());
    close(loader);
    if (!sent)
        why = "The ELF loader did not take the helper";
    return sent;
}

void Helper::revive()
{
    std::lock_guard<std::mutex> hold(revive_lock_);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_revive_ < std::chrono::seconds(8))
        return;
    last_revive_ = now;
    std::string version;
    if (const int port = find_helper(version))
    {
        port_ = port; // another worker has brought it back
        return;
    }
    std::string why;
    log_line("%s", "the helper has gone: starting it again");
    if (!push_elf(why))
        log_line("%s", why);
    for (int i = 0; i < 25; ++i)
    {
        sleep_ms(200);
        if (const int port = find_helper(version))
        {
            port_ = port;
            return;
        }
    }
}

int Helper::find_helper(std::string &version)
{
    for (const int step : kPortSteps)
    {
        const int port = base_port_ + step;
        const int fd = open_local(port);
        if (fd < 0)
            continue;
        // a helper that crashed holds its port and accepts, but never answers
        timeval timeout{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        int probe = fd;
        Reply hello;
        const bool answered = exchange(probe, {"HELLO"}, hello) && hello.ok;
        close(fd);
        if (!answered)
            continue;
        version.clear();
        for (const auto &row : hello.rows())
        {
            if (row.size() >= 2 && row[0] == "artswapd")
                version = row[1];
            std::lock_guard<std::mutex> lock(lock_);
            if (row.size() >= 2 && row[0] == "key")
            {
                key_source_ = row[1];
                key_hint_ = row.size() >= 3 ? row[2] : "";
            }
            if (row.size() >= 2 && row[0] == "lan")
                lan_address_ = row[1];
            if (row.size() >= 2 && row[0] == "http")
            {
                backend_ = row[1];
                if (row[1] == "unavailable" && row.size() > 2)
                    backend_ += ": " + row[2];
            }
        }
        return port;
    }
    return 0;
}

// Finds the helper, or starts it first; then checks it is this title's version
void Helper::launch()
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::string version;
        int port = find_helper(version);
        if (port == 0)
        {
            set_status("Starting the helper");
            std::string why;
            if (!push_elf(why))
            {
                set_status(why);
                state_ = State::failed;
                return;
            }
            for (int i = 0; i < 40 && port == 0; ++i)
            {
                sleep_ms(250);
                port = find_helper(version);
            }
            if (port == 0)
            {
                set_status("The helper did not start: the ELF loader took it, but nothing answers");
                state_ = State::failed;
                return;
            }
        }
        if (version != kHelperVersion && attempt == 0)
        {
            // a helper of another version: ask it to leave, then start ours
            set_status("Replacing helper " + version);
            port_ = port;
            int fd = -1;
            if (connect_helper(fd))
            {
                Reply ignored;
                exchange(fd, {"BYE"}, ignored);
                close(fd);
            }
            sleep_ms(1500);
            continue;
        }
        port_ = port;
        set_status("Ready");
        {
            std::lock_guard<std::mutex> hold(lock_);
            for (int i = static_cast<int>(threads_.size()); i < kWorkers; ++i)
                threads_.emplace_back([this] { work(); });
        }
        state_ = State::ready;
        wake_.notify_all();
        return;
    }
    set_status("The running helper is another version and would not leave");
    state_ = State::failed;
}

void Helper::work()
{
    int fd = -1;
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> hold(lock_);
            wake_.wait(hold, [this] { return stopping_ || !urgent_.empty() || !queued_.empty(); });
            if (stopping_)
                break;
            std::deque<Job> &from = urgent_.empty() ? queued_ : urgent_;
            job = std::move(from.front());
            from.pop_front();
            if (std::find(dropped_.begin(), dropped_.end(), job.generation) != dropped_.end())
            {
                job.reply.error = "dropped";
                finished_.push_back(std::move(job));
                continue;
            }
        }
        bool sent = false;
        for (int attempt = 0; attempt < 2 && !sent; ++attempt)
        {
            if (fd < 0 && !connect_helper(fd))
            {
                // the helper has gone (it closes 90 s after it was last needed, or it failed)
                fd = -1;
                revive();
                continue;
            }
            sent = exchange(fd, job.fields, job.reply);
            if (!sent)
            {
                close(fd);
                fd = -1;
            }
        }
        if (!sent)
            job.reply.error = "The helper stopped answering";
        std::lock_guard<std::mutex> hold(lock_);
        finished_.push_back(std::move(job));
    }
    if (fd >= 0)
        close(fd);
}

void Helper::ask(std::vector<std::string> fields, std::function<void(Reply &)> done, bool urgent, int generation)
{
    Job job;
    job.fields = std::move(fields);
    job.done = std::move(done);
    job.generation = generation;
    {
        std::lock_guard<std::mutex> hold(lock_);
        if (state_ == State::failed)
        {
            job.reply.error = status_;
            finished_.push_back(std::move(job));
            return;
        }
        (urgent ? urgent_ : queued_).push_back(std::move(job));
    }
    wake_.notify_one();
}

void Helper::drop(int generation)
{
    std::lock_guard<std::mutex> hold(lock_);
    if (std::find(dropped_.begin(), dropped_.end(), generation) == dropped_.end())
        dropped_.push_back(generation);
    if (dropped_.size() > 64)
        dropped_.erase(dropped_.begin());
}

void Helper::pump(int limit)
{
    for (int i = 0; i < limit; ++i)
    {
        Job job;
        {
            std::lock_guard<std::mutex> hold(lock_);
            if (finished_.empty())
                return;
            job = std::move(finished_.front());
            finished_.pop_front();
        }
        if (job.done)
            job.done(job.reply);
    }
}

void Helper::bye()
{
    ask({"BYE"}, nullptr, true);
}

} // namespace artswap
