/*
 * Art Swap - the music a game's tile plays (app.hpp).
 *
 * The home screen plays a title's snd0.at9 while its tile is selected. This
 * page lists the person's own songs (/data/artswap/music: sent from a phone
 * with □, or copied over FTP), shows how loud the chosen one is along its
 * length, and the clip of it kept: ← → move its start a second at a time,
 * L1 R1 ten, L2 R2 make it five seconds shorter or longer. △ plays the clip
 * as the home screen will (the helper prepares it: 48 kHz stereo at the
 * home screen's loudness), ✕ makes it the game's music. The helper encodes
 * it on the console (helper/src/music.c).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app.hpp"
#include "title.hpp"

#include "audio/cues.hpp"
#include "qrcodegen.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace artswap
{

namespace
{

namespace gfx = hui::gfx;
namespace ui = hui::ui;
namespace tween = hui::tween;
using gfx::Color;
using gfx::Rect;
using hui::Action;
using hui::Direction;
using hui::audio::Cue;

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);
const Color kNight = Color::rgb(0x05070d);
const Color kAmber = Color::rgb(0xf5a524);
const Color kRed = Color::rgb(0xff6b6b);
constexpr float kMargin = 96.0f;
constexpr float kTop = 378.0f;
constexpr float kListW = 1104.0f;
constexpr float kRowPitch = 64.0f;
constexpr int kRowsShown = 8;
constexpr float kPanelX = 1268.0f;
constexpr float kPanelW = 556.0f;
constexpr double kStep = 1.0;     // ← →
constexpr double kSkip = 10.0;    // L1 R1
constexpr double kLength = 5.0;   // L2 R2
constexpr double kShortest = 5.0;

// 65 -> "1:05"
std::string clock_text(double seconds)
{
    const int s = static_cast<int>(std::floor(std::max(0.0, seconds) + 0.5));
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", s / 60, s % 60);
    return text;
}

std::string number(double value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f", value);
    return text;
}

std::string size_text(long long bytes)
{
    char text[24];
    if (bytes >= 1024 * 1024)
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(text, sizeof(text), "%lld KB", (bytes + 1023) / 1024);
    return text;
}

} // namespace

bool App::can_have_music(const Title &title) const
{
    // music to change, or a folder ShadowMountPlus installs the game from, to add some to
    return title.has_music() || title.refresh;
}

void App::open_music()
{
    screen_ = Screen::music;
    age_ = 0.0f;
    page_age_ = 0.0f;
    songs_.clear();
    songs_error_.clear();
    song_focus_ = 0;
    song_focus_position_.snap(0.0f);
    song_name_.clear();
    song_levels_.clear();
    song_error_.clear();
    song_seconds_ = 0.0;
    clip_start_ = 0.0;
    clip_seconds_ = 30.0;
    music_applying_ = false;
    load_songs();
}

void App::leave_music()
{
    stop_clip();
    screen_ = Screen::library;
    age_ = 0.3f;
}

void App::load_songs()
{
    if (songs_loading_)
        return;
    songs_loading_ = true;
    helper_.ask({"SONGS"}, [this](Reply &reply) {
        songs_loading_ = false;
        if (!reply.ok)
        {
            songs_error_ = reply.error;
            return;
        }
        songs_error_.clear();
        const std::string focus =
            song_focus_ < static_cast<int>(songs_.size()) ? songs_[static_cast<std::size_t>(song_focus_)].name : "";
        std::vector<Song> list;
        for (const auto &row : reply.rows())
        {
            if (row.size() >= 2 && row[0] == "folder")
                music_folder_ = row[1];
            else if (row.size() >= 2 && row[0] == "max")
                music_max_ = std::max(kShortest, std::atof(row[1].c_str()));
            else if (row.size() >= 3 && row[0] == "S")
                list.push_back({row[1], std::atoll(row[2].c_str())});
        }
        // what a phone has sent since its sheet opened: the newest is chosen
        std::string arrived;
        if (sheet_ == Sheet::music_phone)
            for (const Song &song : list)
                if (std::find(songs_seen_.begin(), songs_seen_.end(), song.name) == songs_seen_.end())
                {
                    songs_seen_.push_back(song.name);
                    arrived = song.name;
                    toast("Your phone sent " + song.name, "It is in your music: choose the clip to keep.");
                }
        songs_ = std::move(list);
        const std::string want = arrived.empty() ? focus : arrived;
        song_focus_ = 0;
        for (int i = 0; i < static_cast<int>(songs_.size()); ++i)
            if (songs_[static_cast<std::size_t>(i)].name == want)
                song_focus_ = i;
        clip_seconds_ = std::min(clip_seconds_, music_max_);
        song_rest_ = 0.0f;
    }, true);
}

void App::load_song()
{
    if (songs_.empty())
        return;
    const std::string name = songs_[static_cast<std::size_t>(song_focus_)].name;
    song_name_ = name;
    song_loading_ = true;
    song_error_.clear();
    song_levels_.clear();
    song_seconds_ = 0.0;
    clip_start_ = 0.0;
    helper_.ask({"SONG", name}, [this, name](Reply &reply) {
        if (name != song_name_)
            return;
        song_loading_ = false;
        if (!reply.ok)
        {
            song_error_ = reply.error;
            return;
        }
        for (const auto &row : reply.rows())
        {
            if (row.size() >= 3 && row[0] == "song")
                song_seconds_ = std::atof(row[2].c_str());
            else if (!row.empty() && row[0] == "W")
                for (std::size_t i = 1; i < row.size(); ++i)
                    song_levels_.push_back(std::atoi(row[i].c_str()));
        }
    }, true);
}

// The helper prepares the clip as the home screen will play it, and sends it as 16-bit stereo at 48 kHz
void App::play_clip()
{
    if (songs_.empty() || song_name_.empty() || song_loading_ || !song_error_.empty())
        return;
    stop_clip();
    const int serial = ++clip_serial_;
    clip_loading_ = true;
    helper_.ask({"CLIP", song_name_, number(clip_start_), number(clip_seconds_)}, [this, serial](Reply &reply) {
        if (serial != clip_serial_)
            return;
        clip_loading_ = false;
        if (!reply.ok)
        {
            toast("The clip did not play", reply.error, true);
            return;
        }
        const std::string &text = reply.text;
        const std::size_t line = text.find('\n');
        std::size_t frames = 0;
        double source = 0.0, final = 0.0;
        if (line == std::string::npos ||
            std::sscanf(text.c_str(), "clip\t%zu\t%lf\t%lf", &frames, &source, &final) != 3 ||
            text.size() < line + 1 + frames * 4)
        {
            toast("The clip did not play", "The helper's answer was cut short.", true);
            return;
        }
        std::vector<float> pcm(frames * 2);
        const unsigned char *p = reinterpret_cast<const unsigned char *>(text.data()) + line + 1;
        for (std::size_t i = 0; i < frames * 2; ++i)
            pcm[i] = static_cast<float>(static_cast<std::int16_t>(p[2 * i] | p[2 * i + 1] << 8)) / 32768.0f;
        clip_pcm_ = std::move(pcm);
        clip_length_ = static_cast<double>(frames) / 48000.0;
        clip_lufs_ = final;
        if (speaker_)
            speaker_->play(clip_pcm_.data(), frames);
        clip_playing_ = true;
        clip_started_ = clock_;
    }, true);
}

void App::stop_clip()
{
    clip_serial_++;
    clip_loading_ = false;
    if (clip_playing_ && speaker_)
        speaker_->stop();
    clip_playing_ = false;
    // the audio thread may read it for a moment yet
    if (!clip_pcm_.empty())
    {
        clip_retired_.push_back(std::move(clip_pcm_));
        clip_pcm_.clear();
        clip_retire_ = 0.0f;
    }
}

void App::apply_music()
{
    const Title *title = focused_title();
    if (title == nullptr || song_name_.empty())
        return;
    stop_clip();
    music_applying_ = true;
    const std::string id = title->id, name = title->name;
    const bool adding = !title->has_music();
    helper_.ask({"MUSIC", id, song_name_, number(clip_start_), number(clip_seconds_)},
                [this, id, name, adding](Reply &reply) {
                    music_applying_ = false;
                    sheet_ = Sheet::none;
                    if (!reply.ok)
                    {
                        toast("The music was not changed", reply.error, true);
                        return;
                    }
                    note_pending(id, adding);
                    toast(std::string(adding ? "Music added to " : "New music for ") + name,
                          adding ? "Your game had none, so Restore deletes it. Apply changes (R2) installs the game "
                                   "again, so its tile plays it."
                                 : "Your original is kept, so Restore can put it back. Apply changes (R2) to hear it "
                                   "on the home screen.");
                    leave_music();
                    load_titles(false);
                },
                true);
}

void App::open_music_phone()
{
    sheet_ = Sheet::music_phone;
    // a code the phone's page sends with each song, so only someone who can see the TV sends music
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char code[8];
    std::snprintf(code, sizeof(code), "%04d", static_cast<int>((now / 7919) % 9000 + 1000));
    pairing_code_ = code;
    phone_address_.clear();
    phone_error_.clear();
    songs_seen_.clear();
    for (const Song &song : songs_)
        songs_seen_.push_back(song.name);
    songs_poll_ = 1.5f;
    helper_.ask({"PHONE", pairing_code_, "music"}, [this](Reply &reply) {
        if (!reply.ok)
        {
            phone_error_ = reply.error;
            return;
        }
        std::string lan, port = "9118";
        for (const auto &row : reply.rows())
        {
            if (row.size() >= 2 && row[0] == "lan")
                lan = row[1];
            if (row.size() >= 2 && row[0] == "port")
                port = row[1];
        }
        if (lan.empty())
            phone_error_ = "This console has no network address, so a phone cannot reach it. Copy music to " +
                           music_folder_ + " over FTP instead.";
        else
            phone_address_ = lan + ":" + port;
    }, true);
}

void App::update_music_sheet(const hui::InputFrame &input, float, ui::Feedback &feedback)
{
    if (sheet_ == Sheet::music_phone)
    {
        // the songs, asked for again while the phone sends them
        if ((songs_poll_ -= dt_) <= 0.0f)
        {
            songs_poll_ = 1.5f;
            load_songs();
        }
        if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm))
        {
            feedback.play(Cue::modal_close);
            close_phone();
            sheet_ = Sheet::none;
        }
        return;
    }
    if (music_applying_)
        return;
    if (input.is_pressed(Action::confirm))
    {
        feedback.play(Cue::launch);
        feedback.rumble(0.5f, 0.12f);
        apply_music();
    }
    else if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::modal_close);
        sheet_ = Sheet::none;
    }
}

void App::update_music(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    const int count = static_cast<int>(songs_.size());
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        leave_music();
        return;
    }
    if (input.is_pressed(Action::west))
    {
        feedback.play(Cue::modal_open);
        open_music_phone();
        return;
    }
    // ↑ ↓ the song
    if (count > 0 && (input.nav == Direction::up || input.nav == Direction::down))
    {
        const int next = std::clamp(song_focus_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
        if (next != song_focus_)
        {
            song_focus_ = next;
            song_rest_ = 0.0f;
            stop_clip();
            feedback.play(Cue::focus, 1.0f, -0.3f);
        }
        else if (!input.nav_repeat)
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
    }
    // the song is read once the focus rests on it
    song_rest_ += dt;
    if (count > 0 && songs_[static_cast<std::size_t>(song_focus_)].name != song_name_ && song_rest_ > 0.25f &&
        !song_loading_)
        load_song();
    const bool ready = count > 0 && !song_name_.empty() && !song_loading_ && song_error_.empty();
    // ← → the start, a second at a time; L1 R1 ten
    double move = 0.0;
    if (input.nav == Direction::left || input.nav == Direction::right)
        move = input.nav == Direction::right ? kStep : -kStep;
    if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
        move = input.is_pressed(Action::page_next) ? kSkip : -kSkip;
    if (ready && move != 0.0)
    {
        const double next = std::clamp(clip_start_ + move, 0.0, std::max(0.0, std::floor(song_seconds_ - 1.0)));
        if (next != clip_start_)
        {
            clip_start_ = next;
            stop_clip();
            feedback.play(Cue::focus, 1.2f, 0.0f, 0.6f);
        }
        else if (!input.nav_repeat)
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
    }
    // L2 R2 the length, five seconds at a time
    if (ready && (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next)))
    {
        const double step = input.is_pressed(Action::jump_next) ? kLength : -kLength;
        const double next = std::clamp(std::round((clip_seconds_ + step) / kLength) * kLength, kShortest,
                                       std::floor(music_max_));
        if (next != clip_seconds_)
        {
            clip_seconds_ = next;
            stop_clip();
            feedback.play(Cue::tab);
        }
        else
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
    }
    // △ plays the clip as the home screen will, or stops it
    if (input.is_pressed(Action::north) && ready)
    {
        if (clip_playing_ || clip_loading_)
        {
            stop_clip();
            feedback.play(Cue::back);
        }
        else
            play_clip();
    }
    // ✕ makes it the game's music
    if (input.is_pressed(Action::confirm) && ready)
    {
        stop_clip();
        feedback.play(Cue::modal_open);
        sheet_ = Sheet::music_confirm;
        music_applying_ = false;
    }
    song_focus_position_.target = static_cast<float>(song_focus_);
    song_focus_position_.update(dt, 22.0f);
}

void App::draw_music(gfx::DrawList &list) const
{
    const Title *title = focused_title();
    if (title == nullptr)
        return;
    const float in = tween::stagger(age_, 1, 0.06f, 0.5f);
    list.push_opacity(in);
    ui::text(list, fonts_.semibold, "CHANGE THE MUSIC", kMargin, 168, 20, title->accent, gfx::Align::left, 4.0f);
    ui::text(list, fonts_.display, fonts_.display.font->fit(title->name, 60, 1100), kMargin - 3, 234, 60, kWhite);
    ui::text(list, fonts_.regular, "What its tile plays while it is selected on the home screen", kMargin, 276, 23,
             kWhite.with_alpha(0.7f));

    // ---- the songs ----
    const int count = static_cast<int>(songs_.size());
    char heading[64];
    std::snprintf(heading, sizeof(heading), "YOUR MUSIC  \xC2\xB7  %d", count);
    ui::text(list, fonts_.semibold, heading, kMargin, kTop - 22, 17, kWhite.with_alpha(0.6f), gfx::Align::left, 3.0f);
    if (count == 0)
    {
        if (songs_loading_)
            draw_spinner(list, kMargin + 22, kTop + 40, 18);
        else
        {
            const std::string why = !songs_error_.empty() ? songs_error_
                                                          : "No music yet. Send songs from your phone, or copy MP3, "
                                                            "FLAC, WAV or AIFF files to " +
                                                                (music_folder_.empty() ? std::string("/data/artswap/music")
                                                                                       : music_folder_) +
                                                                " over FTP.";
            const float y = ui::paragraph(list, fonts_.regular, why, kMargin, kTop + 40, 27, kListW - 60, 40,
                                          kWhite.with_alpha(0.85f), 4);
            ui::draw_button(list, fonts_, ui::GlyphStyle::dark(), ui::Button::square, kMargin + 16, y + 34, 30);
            ui::text(list, fonts_.semibold, "From your phone", kMargin + 50, y + 42, 23, kWhite.with_alpha(0.85f));
        }
    }
    else
    {
        const int first = std::clamp(song_focus_ - kRowsShown / 2, 0, std::max(0, count - kRowsShown));
        const float y0 = kTop;
        list.rounded_rect({kMargin - 20, y0 + (song_focus_position_.value - first) * kRowPitch, kListW + 40,
                           kRowPitch - 8},
                          28, kWhite);
        for (int i = first; i < std::min(count, first + kRowsShown); ++i)
        {
            const Song &song = songs_[static_cast<std::size_t>(i)];
            const bool f = i == song_focus_;
            const float y = y0 + (i - first) * kRowPitch;
            ui::text(list, fonts_.semibold, fonts_.semibold.font->fit(song.name, 26, kListW * 0.78f), kMargin + 8,
                     y + 39, 26, f ? kInk : kWhite.with_alpha(0.9f));
            ui::text(list, fonts_.regular, size_text(song.bytes), kMargin + kListW - 8, y + 39, 22,
                     f ? kInk.with_alpha(0.7f) : kWhite.with_alpha(0.55f), gfx::Align::right);
        }
    }

    // ---- the clip ----
    ui::text(list, fonts_.semibold, "THE CLIP", kPanelX, kTop - 22, 17, kWhite.with_alpha(0.6f), gfx::Align::left, 3.0f);
    const Rect wave{kPanelX, kTop, kPanelW, 160};
    list.rounded_rect(wave, 14, kNight.with_alpha(0.7f));
    list.bordered_rect(wave, 14, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.12f));
    const double length = std::min(clip_seconds_, std::max(0.0, song_seconds_ - clip_start_));
    if (song_loading_ || (count > 0 && song_name_.empty()))
        draw_spinner(list, wave.cx(), wave.cy(), 20);
    else if (!song_error_.empty())
        ui::paragraph(list, fonts_.regular, song_error_, wave.x + 20, wave.y + 44, 21, wave.w - 40, 30, kRed, 3);
    else if (!song_levels_.empty() && song_seconds_ > 0.0)
    {
        // how loud it is along its length; the clip lit in the game's colour
        const int bars = static_cast<int>(song_levels_.size());
        const float bw = (wave.w - 24) / bars;
        const double a = clip_start_ / song_seconds_, b = (clip_start_ + length) / song_seconds_;
        for (int i = 0; i < bars; ++i)
        {
            const float level = std::max(0.04f, song_levels_[static_cast<std::size_t>(i)] / 99.0f);
            const float h = (wave.h - 28) * level;
            const double at = (i + 0.5) / bars;
            const bool lit = at >= a && at <= b;
            list.rounded_rect({wave.x + 12 + i * bw, wave.cy() - h / 2, std::max(1.0f, bw - 1.0f), h}, 0,
                              lit ? title->accent : kWhite.with_alpha(0.25f));
        }
        // where the clip playing has got to
        if (clip_playing_)
        {
            const double t = clip_start_ + std::min(static_cast<double>(clock_ - clip_started_), clip_length_);
            const float x = wave.x + 12 + static_cast<float>(t / song_seconds_) * (wave.w - 24);
            list.rounded_rect({x - 1.5f, wave.y + 8, 3, wave.h - 16}, 1.5f, kWhite);
        }
    }
    float y = wave.y + wave.h + 52;
    if (count > 0 && song_seconds_ > 0.0)
    {
        ui::text(list, fonts_.semibold, "From " + clock_text(clip_start_) + "  \xC2\xB7  " +
                                            clock_text(length) + " long", kPanelX, y, 30, kWhite);
        y += 38;
        ui::text(list, fonts_.regular,
                 "To " + clock_text(clip_start_ + length) + " of " + clock_text(song_seconds_) + ", looping, at most " +
                     clock_text(std::floor(music_max_)),
                 kPanelX, y, 21, kWhite.with_alpha(0.65f));
        y += 56;
    }
    if (clip_loading_)
    {
        draw_spinner(list, kPanelX + 16, y - 8, 14);
        ui::text(list, fonts_.regular, "Preparing the clip\xE2\x80\xA6", kPanelX + 44, y, 23, kWhite.with_alpha(0.85f));
    }
    else if (clip_playing_)
    {
        list.circle(kPanelX + 10, y - 8, 7, title->accent);
        ui::text(list, fonts_.regular, "Playing, as the home screen will", kPanelX + 30, y, 23, kWhite.with_alpha(0.85f));
    }
    // what the tile plays now
    const float ny = 840;
    ui::text(list, fonts_.semibold, "NOW", kPanelX, ny, 17, kWhite.with_alpha(0.6f), gfx::Align::left, 3.0f);
    int seconds = 0;
    const bool has = std::sscanf(title->music.c_str(), "at9 %ds", &seconds) == 1;
    ui::paragraph(list, fonts_.regular,
                  has ? std::to_string(seconds) + " seconds of music" +
                            (title->music_added ? ", added by Art Swap" : title->kept ? "" : ", its own")
                      : std::string("None: Art Swap adds it, and Apply changes installs the game again"),
                  kPanelX, ny + 34, 21, kPanelW, 30, kWhite.with_alpha(0.7f), 2);
    list.pop_opacity();
}

void App::draw_music_sheet(gfx::DrawList &list, const Rect &panel, float x, float y) const
{
    const Title *title = focused_title();
    const std::string name = title ? title->name : "";
    const Color accent = title ? title->accent : Color::rgb(0x9fd8ff);
    auto heading = [&](const std::string &label, const std::string &text) {
        ui::text(list, fonts_.semibold, label, x, y, 19, accent, gfx::Align::left, 4.0f);
        ui::text(list, fonts_.display, fonts_.display.font->fit(text, 48, panel.w - 128), x - 2, y + 62, 48, kWhite);
        y += 120;
    };
    if (sheet_ == Sheet::music_confirm)
    {
        heading("NEW MUSIC", "For " + name + "?");
        const double length = std::min(clip_seconds_, std::max(0.0, song_seconds_ - clip_start_));
        const bool adding = title && !title->has_music();
        const std::string text =
            std::to_string(static_cast<int>(std::lround(length))) + " seconds of " + song_name_ + ", from " +
            clock_text(clip_start_) + ", looping as the music its tile plays, as loud as the home screen expects. " +
            (adding ? "It has none now: Art Swap adds it to the game's folder, and Apply changes installs the game "
                      "again so the home screen takes it in. Restore deletes it."
                    : "Its original is kept, so Restore can always put it back. Apply changes to hear it.");
        ui::paragraph(list, fonts_.regular, text, x, y, 25, panel.w - 128, 38, kWhite.with_alpha(0.85f), 7);
        if (music_applying_)
        {
            draw_spinner(list, x + 22, panel.y + panel.h - 70, 20);
            ui::text(list, fonts_.semibold, "Making the music\xE2\x80\xA6", x + 64, panel.y + panel.h - 60, 26, kWhite);
        }
        return;
    }
    // sending music from a phone: the code to scan
    heading("SEND MUSIC", "From your phone");
    const Rect plate{x, y, 330, 330};
    const float info_x = plate.x + plate.w + 48;
    if (!phone_error_.empty())
    {
        list.bordered_rect(plate, 18, kNight.with_alpha(0.4f), 1.5f, kAmber.with_alpha(0.5f));
        ui::paragraph(list, fonts_.regular, phone_error_, plate.x + 24, plate.y + 56, 22, plate.w - 48, 32, kAmber, 7);
    }
    else if (phone_address_.empty())
    {
        list.bordered_rect(plate, 18, kNight.with_alpha(0.4f), 1.5f, kWhite.with_alpha(0.08f));
        draw_spinner(list, plate.cx(), plate.cy(), 22);
    }
    else
    {
        const std::string url = "http://" + phone_address_ + "/" + pairing_code_;
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        list.rounded_rect(plate, 18, kWhite);
        const float cell = (plate.w - 48.0f) / static_cast<float>(qr.getSize());
        for (int my = 0; my < qr.getSize(); ++my)
            for (int mx = 0; mx < qr.getSize(); ++mx)
                if (qr.getModule(mx, my))
                    list.rounded_rect({plate.x + 24 + mx * cell, plate.y + 24 + my * cell, cell + 0.6f, cell + 0.6f}, 0,
                                      kInk);
    }
    const float width = panel.x + panel.w - 64 - info_x;
    float ty = ui::paragraph(list, fonts_.regular,
                             "Scan the code, choose songs on your phone (MP3, FLAC, WAV or AIFF), and they come "
                             "into Your music as they arrive. Use music that is yours to use.",
                             info_x, plate.y + 24, 24, width, 36, kWhite.with_alpha(0.85f), 5) +
               30;
    ui::text(list, fonts_.semibold, "OR OPEN", info_x, ty, 15, kWhite.with_alpha(0.5f), gfx::Align::left, 3.0f);
    ui::text(list, fonts_.mono, phone_address_.empty() ? "\xE2\x80\xA6" : phone_address_, info_x, ty + 40, 28, kWhite);
    ui::text(list, fonts_.semibold, "AND ENTER THE CODE", info_x, ty + 96, 15, kWhite.with_alpha(0.5f),
             gfx::Align::left, 3.0f);
    ui::text(list, fonts_.mono, pairing_code_, info_x, ty + 150, 46, kWhite, gfx::Align::left, 12.0f);
    if (phone_error_.empty())
    {
        draw_spinner(list, x + 14, plate.y + plate.h + 50, 13);
        ui::text(list, fonts_.regular, "Waiting for your phone\xE2\x80\xA6", x + 40, plate.y + plate.h + 58, 23,
                 kWhite.with_alpha(0.8f));
    }
}

} // namespace artswap
