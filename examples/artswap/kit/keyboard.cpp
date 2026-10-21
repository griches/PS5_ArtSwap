/*
 * Art Swap - text entry through the console's own keyboard dialog
 * (libSceImeDialog), for the search and the API key.
 *
 * The dialog runs beside the title, which keeps drawing under it; a thread
 * waits for it to finish and leaves the answer for App::pump. Adapted from
 * PS5_WindWakerHD's ps5/src/ime_ps5.cpp (MIT), which was proven on the
 * console. Off the console there is no keyboard (make_keyboard returns null).
 * Only exported functions of the system software are called.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app.hpp"

#include <memory>

#if defined(__PROSPERO__)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
int sceSysmoduleLoadModule(uint32_t id);
int sceUserServiceGetInitialUser(int32_t *user);
int sceImeDialogInit(const void *param, const void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(void *result);
int sceImeDialogTerm(void);
int sceKernelUsleep(uint32_t microseconds);
}

namespace artswap
{

namespace
{

constexpr uint32_t kModuleImeDialog = 0x0096;
enum { kTypeBasicLatin = 1 };
enum { kStatusRunning = 1 };
enum { kEndOk = 0 };

struct DialogParam
{
    int32_t user;
    int32_t type;
    uint64_t supported_languages;
    int32_t enter_label;
    int32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_text_length;
    char16_t *input_text_buffer;
    float x, y; // of a 1920x1080 screen
    int32_t horizontal_alignment, vertical_alignment;
    const char16_t *placeholder;
    const char16_t *title;
    int8_t reserved[16];
};
static_assert(sizeof(DialogParam) == 96, "the dialog's parameters are 96 bytes");

struct DialogResult
{
    int32_t end_status;
    int32_t reserved[12];
};

std::u16string to_utf16(const std::string &text)
{
    std::u16string out;
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t code = c;
        int extra = 0;
        if (c >= 0xf0)
            code = c & 0x07, extra = 3;
        else if (c >= 0xe0)
            code = c & 0x0f, extra = 2;
        else if (c >= 0xc0)
            code = c & 0x1f, extra = 1;
        ++i;
        for (int k = 0; k < extra && i < text.size(); ++k, ++i)
            code = (code << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        if (code >= 0x10000)
        {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
        }
        else
            out.push_back(static_cast<char16_t>(code));
    }
    return out;
}

std::string to_utf8(const std::u16string &text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        uint32_t code = text[i];
        if (code >= 0xd800 && code < 0xdc00 && i + 1 < text.size())
            code = 0x10000 + ((code - 0xd800) << 10) + (text[++i] - 0xdc00);
        if (code < 0x80)
            out.push_back(static_cast<char>(code));
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else if (code < 0x10000)
        {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    return out;
}

class ConsoleKeyboard final : public Keyboard
{
  public:
    bool open(const std::string &title, const std::string &initial, int max_length,
              std::function<void(bool, std::string)> done) override
    {
        if (max_length <= 0 || busy_.exchange(true))
            return false;
        static const int loaded = sceSysmoduleLoadModule(kModuleImeDialog);
        // what the dialog reads stays alive until it has finished: the waiting thread owns it
        struct Job
        {
            std::vector<char16_t> text;
            std::u16string title;
            DialogParam param{};
        };
        auto *job = new Job;
        job->text.assign(static_cast<std::size_t>(max_length) + 1, 0);
        const std::u16string start = to_utf16(initial);
        std::memcpy(job->text.data(), start.data(),
                    std::min(start.size(), static_cast<std::size_t>(max_length)) * sizeof(char16_t));
        job->title = to_utf16(title);
        int32_t user = 0;
        sceUserServiceGetInitialUser(&user);
        DialogParam &p = job->param;
        p.user = user;
        p.type = kTypeBasicLatin;
        p.max_text_length = static_cast<uint32_t>(max_length);
        p.input_text_buffer = job->text.data();
        p.x = 960;
        p.y = 540;
        p.horizontal_alignment = p.vertical_alignment = 1;
        p.title = job->title.c_str();
        const int started = sceImeDialogInit(&p, nullptr);
        if (started != 0)
        {
            std::fprintf(stderr, "[artswap] the keyboard did not open (%#x, module %#x)\n",
                         static_cast<unsigned>(started), static_cast<unsigned>(loaded));
            delete job;
            busy_ = false;
            return false;
        }
        done_ = std::move(done);
        std::thread([this, job] {
            while (sceImeDialogGetStatus() == kStatusRunning)
                sceKernelUsleep(50000);
            DialogResult result{};
            const int got = sceImeDialogGetResult(&result);
            sceImeDialogTerm();
            const bool ok = got == 0 && result.end_status == kEndOk;
            std::string text = to_utf8(std::u16string(job->text.data()));
            delete job;
            std::lock_guard<std::mutex> hold(lock_);
            answer_ok_ = ok;
            answer_ = ok ? text : std::string();
            answered_ = true;
        }).detach();
        return true;
    }

    void pump() override
    {
        bool ok;
        std::string text;
        {
            std::lock_guard<std::mutex> hold(lock_);
            if (!answered_)
                return;
            answered_ = false;
            ok = answer_ok_;
            text = std::move(answer_);
        }
        busy_ = false;
        auto done = std::move(done_);
        done_ = nullptr;
        if (done)
            done(ok, std::move(text));
    }

  private:
    std::atomic<bool> busy_{false};
    std::mutex lock_;
    bool answered_ = false, answer_ok_ = false;
    std::string answer_;
    std::function<void(bool, std::string)> done_;
};

} // namespace

std::unique_ptr<Keyboard> make_keyboard()
{
    return std::make_unique<ConsoleKeyboard>();
}

} // namespace artswap

#else

namespace artswap
{
std::unique_ptr<Keyboard> make_keyboard()
{
    return nullptr;
}
} // namespace artswap

#endif
