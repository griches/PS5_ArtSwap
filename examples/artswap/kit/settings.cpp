/*
 * Art Swap - the full pages: Settings, and Connect SteamGridDB (adding the key).
 *
 * Both follow the kit's Settings design, as page.hpp draws it.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app.hpp"
#include "page.hpp"

#include "audio/cues.hpp"
#include "qrcodegen.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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
using namespace page;

constexpr int kCategories = 3;
constexpr const char *kCategoryNames[kCategories] = {"SteamGridDB", "Games", "About"};

// the connect page
constexpr float kLeftW = 780.0f;
constexpr Rect kCodePanel{936.0f, 236.0f, 888.0f, 692.0f};
constexpr float kConnectRowsY = 640.0f;
} // namespace

// ================================================================ settings

int App::settings_rows(int category) const
{
    return category == 0 ? 3 : category == 1 ? 2 : 1;
}

Rect App::settings_rail_rect(int category) const
{
    return {kMargin, kRailY + static_cast<float>(category) * kRailPitch, kRailW, kRailH};
}

Rect App::settings_row_rect(int category, int row) const
{
    // under the page's tiles: one row of them, or two on About
    const float tiles = category == 2 ? 2 * 128.0f + 16.0f : 140.0f;
    const float top = kPanel.y + 100.0f + tiles + 24.0f;
    return {kPanel.x + kPanelPad, top + static_cast<float>(row) * kRowPitch, kPanel.w - 2 * kPanelPad, kRowH};
}

void App::open_settings()
{
    settings_back_ = screen_;
    screen_ = Screen::settings;
    if (key_state_ != "user")
        settings_category_ = 0;
    settings_in_rows_ = false;
    remove_armed_ = 0.0f;
    page_age_ = 0.0f;
    age_ = 0.0f;
    rail_marker_.snap(settings_rail_rect(settings_category_).y);
    page_focus_.snap(settings_rail_rect(settings_category_));
}

void App::update_settings(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    remove_armed_ = std::max(0.0f, remove_armed_ - dt);
    const int rows = settings_rows(settings_category_);
    auto change_category = [&](int next) {
        next = std::clamp(next, 0, kCategories - 1);
        if (next == settings_category_)
        {
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
            return;
        }
        settings_category_ = next;
        remove_armed_ = 0.0f;
        page_age_ = 0.0f;
        feedback.play(Cue::tab, 1.08f - 0.06f * static_cast<float>(next));
    };
    if (input.is_pressed(Action::page_prev))
        change_category(settings_category_ - 1);
    else if (input.is_pressed(Action::page_next))
        change_category(settings_category_ + 1);
    else if (!settings_in_rows_)
    {
        if (input.nav == Direction::up || input.nav == Direction::down)
            change_category(settings_category_ + (input.nav == Direction::down ? 1 : -1));
        else if ((input.nav == Direction::right || input.is_pressed(Action::confirm)) && rows > 0)
        {
            settings_in_rows_ = true;
            settings_row_[settings_category_] = std::clamp(settings_row_[settings_category_], 0, rows - 1);
            feedback.play(Cue::focus, 1.0f, 0.3f);
        }
        else if (input.is_pressed(Action::back))
        {
            feedback.play(Cue::back);
            screen_ = settings_back_;
            age_ = 0.3f;
            return;
        }
    }
    else
    {
        int &row = settings_row_[settings_category_];
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = std::clamp(row + (input.nav == Direction::down ? 1 : -1), 0, rows - 1);
            if (next != row)
            {
                row = next;
                remove_armed_ = 0.0f;
                feedback.play(Cue::focus, 1.0f, 0.3f);
            }
            else if (!input.nav_repeat)
                feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
        }
        else if (input.nav == Direction::left || input.is_pressed(Action::back))
        {
            settings_in_rows_ = false;
            remove_armed_ = 0.0f;
            feedback.play(Cue::back);
        }
        else if (input.is_pressed(Action::confirm))
            settings_select(feedback);
    }
    rail_marker_.target = settings_rail_rect(settings_category_).y;
    rail_marker_.update(dt, 20.0f);
    page_focus_.target(settings_in_rows_ ? settings_row_rect(settings_category_, settings_row_[settings_category_])
                                         : settings_rail_rect(settings_category_));
    page_focus_.update(dt, 22.0f);
}

void App::settings_select(ui::Feedback &feedback)
{
    const int row = settings_row_[settings_category_];
    if (settings_category_ == 0)
    {
        if (row == 0)
        {
            feedback.play(Cue::open);
            open_connect("");
        }
        else if (row == 1)
        {
            feedback.play(Cue::select);
            type_key();
        }
        else if (key_state_ != "user")
            feedback.play(Cue::error);
        else if (remove_armed_ <= 0.0f)
        {
            // a second ✕ removes it
            remove_armed_ = 3.0f;
            feedback.play(Cue::toggle);
        }
        else
        {
            remove_armed_ = 0.0f;
            feedback.play(Cue::erase);
            helper_.ask({"KEY", ""}, [this](Reply &reply) {
                if (reply.ok)
                {
                    key_state_ = "none";
                    key_hint_.clear();
                    toast("Your SteamGridDB key is removed", "Add one again to find new art.");
                }
                else
                    toast("The key was not removed", reply.error, true);
            }, true);
        }
    }
    else if (settings_category_ == 1 && row == 0)
    {
        feedback.play(Cue::select);
        load_titles(true);
        toast("Refreshing the game list", "");
    }
    else if (settings_category_ == 1)
    {
        // PlayStation Store logos: asked of Sony only once the person has turned them on
        feedback.play(Cue::toggle);
        const bool on = !store_logos_;
        helper_.ask({"STORE", on ? "on" : "off"}, [this, on](Reply &reply) {
            if (!reply.ok)
            {
                toast("The setting was not changed", reply.error, true);
                return;
            }
            store_logos_ = on;
            store_unreachable_ = false; // asked again, the next time
        }, true);
    }
    else
    {
        feedback.play(Cue::modal_open);
        sheet_ = Sheet::log;
        log_text_ = "Reading the helper's log\xE2\x80\xA6";
        helper_.ask({"LOG"}, [this](Reply &reply) { log_text_ = reply.ok ? reply.text : reply.error; }, true);
    }
}

void App::draw_settings(gfx::DrawList &list) const
{
    const float in = tween::stagger(age_, 0, 0.06f, 0.5f);
    draw_header(list, fonts_, "Settings", in);
    const bool have = key_state_ == "user";

    // the rail: the plate of the active category, the focus, then the names on top
    const float rail_in = tween::stagger(age_, 1, 0.05f, 0.45f);
    list.push_opacity(rail_in);
    list.rounded_rect({kMargin, rail_marker_.value, kRailW, kRailH}, kRadius, kText.with_alpha(0.06f));
    list.circle(kMargin + 6, 866, 4, kAccent);
    ui::paragraph(list, fonts_.regular, "Each person adds their own free SteamGridDB key.", kMargin + 24, 874, 22,
                  kRailW - 40, 32, kText.with_alpha(0.55f), 2);
    list.pop_opacity();

    const float panel_in = tween::stagger(age_, 3, 0.06f, 0.55f);
    list.push_opacity(panel_in);
    draw_panel(list, kPanel);
    list.pop_opacity();

    list.push_opacity(tween::stagger(age_, 4, 0.06f, 0.4f));
    draw_focus(list, page_focus_.value(), ui::breathe(clock_));
    list.pop_opacity();

    static const Glyph glyphs[kCategories] = {Glyph::key, Glyph::games, Glyph::about};
    for (int i = 0; i < kCategories; ++i)
    {
        const float item_in = tween::stagger(age_, 1 + i, 0.05f, 0.45f);
        const Rect r = settings_rail_rect(i);
        const bool active = i == settings_category_;
        const float x = r.x - 28.0f * (1.0f - item_in);
        list.push_opacity(item_in);
        draw_rail_icon(list, glyphs[i], x + 46, r.cy(), active ? kAccent : kText.with_alpha(0.6f));
        ui::text(list, active ? fonts_.semibold : fonts_.regular, kCategoryNames[i], x + 88, centred(r.cy(), 28), 28,
                 kText.with_alpha(active ? 1.0f : 0.68f));
        // the key's state, beside its category
        if (i == 0)
            list.circle(r.x + r.w - 28, r.cy(), 6, have ? kGood : kWarn);
        list.pop_opacity();
    }
    list.rounded_rect({kMargin + 8, rail_marker_.value + 20, 4, kRailH - 40}, 2, kAccent.with_alpha(rail_in));

    // the page
    const int c = settings_category_;
    const float rise = 18.0f;
    const float title_in = tween::stagger(page_age_, 0, 0.05f, 0.3f);
    list.push_opacity(title_in * panel_in);
    static const char *titles[kCategories] = {"Your SteamGridDB key", "Your games", "About Art Swap"};
    ui::text(list, fonts_.semibold, titles[c], kPanel.x + kPanelPad + kRowPad, kPanel.y + 66 + rise * (1.0f - title_in),
             36, kText);
    list.pop_opacity();

    const float tile_w = (kPanel.w - 2 * kPanelPad - 16.0f) * 0.5f;
    auto tile_rect = [&](int i, float h) {
        return Rect{kPanel.x + kPanelPad + static_cast<float>(i % 2) * (tile_w + 16.0f),
                    kPanel.y + 100.0f + static_cast<float>(i / 2) * (h + 16.0f), tile_w, h};
    };
    auto staggered = [&](int index, const auto &draw) {
        const float t = tween::stagger(page_age_, index, 0.05f, 0.32f);
        list.push_opacity(t * panel_in);
        list.push_transform(1.0f, 0, 0, 0, rise * (1.0f - t));
        draw();
        list.pop_transform();
        list.pop_opacity();
    };

    std::vector<RowInfo> rows;
    std::string description;
    const int kept = kept_count();
    if (c == 0)
    {
        staggered(1, [&] {
            draw_tile(list, fonts_, tile_rect(0, 140), "STATUS", have ? "Key added" : "No key yet",
                      have ? (key_hint_.empty() ? "Art Swap can find art" : "Ends in " + key_hint_)
                           : "Add one to find new icons and backgrounds",
                      have ? kGood : kWarn);
        });
        staggered(2, [&] {
            draw_tile(list, fonts_, tile_rect(1, 140), "WHERE TO GET ONE", "steamgriddb.com",
                      "Free with an account: Preferences, then API");
        });
        rows = {{"Add your key from your phone", "QR code", true},
                {have ? "Type a new key" : "Type your key", "Keyboard", true},
                {"Remove your key", remove_armed_ > 0.0f ? "Select again to remove" : "", false, have, true}};
        static const char *notes[] = {
            "Shows a QR code: your phone opens a page on this console and sends the key to it.",
            "Type the 32 letters and numbers with the console's keyboard.",
            "Art Swap forgets the key and stops asking SteamGridDB until you add one."};
        description = settings_in_rows_ ? notes[settings_row_[0]]
                                        : "Art Swap finds new art on SteamGridDB, with a key of your own.";
    }
    else if (c == 1)
    {
        staggered(1, [&] {
            draw_tile(list, fonts_, tile_rect(0, 140), "GAMES FOUND", std::to_string(titles_.size()),
                      "PS5, PS4 and homebrew titles with art to change");
        });
        staggered(2, [&] {
            draw_tile(list, fonts_, tile_rect(1, 140), "ART CHANGED", std::to_string(kept),
                      kept ? (kept == 1 ? "Game with new art" : "Games with new art") : "Nothing changed yet");
        });
        rows = {{"Refresh game list", std::to_string(titles_.size()) + (titles_.size() == 1 ? " game" : " games")},
                {"PlayStation Store logos", store_logos_ ? "On" : "Off"}};
        static const char *notes[] = {
            "Finds games, apps and homebrew installed since Art Swap opened.",
            "Store games' own logos, from Sony's catalog: no account or console details, but Sony sees your IP."};
        description = settings_in_rows_ ? notes[settings_row_[1]]
                                        : "The titles on this console that have home-screen art.";
    }
    else
    {
        const std::string backend = helper_.backend().empty() ? "starting" : helper_.backend();
        staggered(1, [&] { draw_tile(list, fonts_, tile_rect(0, 128), "APPLICATION", std::string("Art Swap ") + kVersion, "PPSA99318"); });
        staggered(2, [&] {
            draw_tile(list, fonts_, tile_rect(1, 128), "HELPER", std::string("artswapd ") + kHelperVersion,
                      "Network: " + backend);
        });
        staggered(3, [&] {
            draw_tile(list, fonts_, tile_rect(2, 128), "THE ART", "SteamGridDB", "Made and shared by its community");
        });
        staggered(4, [&] {
            draw_tile(list, fonts_, tile_rect(3, 128), "MADE WITH", "PS5 Vulkan Template",
                      "UI kit by BlackBearReloaded, GPL-3.0");
        });
        rows = {{"Helper log", "/data/artswap", true}};
        description = settings_in_rows_ ? "What Art Swap's helper did: the first thing to send with a problem report."
                                        : "Versions, where the art comes from, and the helper's log.";
    }
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        staggered(3 + i, [&] {
            draw_row(list, fonts_, settings_row_rect(c, i), rows[static_cast<std::size_t>(i)],
                     settings_in_rows_ && i == settings_row_[c]);
        });
    // under PlayStation Store logos: when it cannot work
    if (c == 1)
        staggered(5, [&] {
            const Rect row = settings_row_rect(1, 1);
            const float x = row.x + kRowPad, y = row.y + row.h + 44;
            list.circle(x + 5, y - 8, 4, kWarn);
            ui::paragraph(list, fonts_.regular,
                          "Most jailbroken consoles are kept from Sony's servers, to stop updates. If yours is, "
                          "PlayStation Store cannot be reached and this does nothing: the logo tab says so once, and "
                          "SteamGridDB's logos show as before. SteamGridDB has the official logo of most store games "
                          "too.",
                          x + 22, y, 22, row.w - 2 * kRowPad - 22, 32, kText.with_alpha(0.6f), 3);
        });
    list.push_opacity(tween::stagger(page_age_, 6, 0.05f, 0.36f) * panel_in);
    draw_description(list, fonts_, kPanel, description);
    list.pop_opacity();
}

// ================================================================ connect

void App::open_connect(const std::string &slot)
{
    pending_slot_ = slot;
    connect_back_ = screen_ == Screen::settings ? Screen::settings : Screen::library;
    screen_ = Screen::connect;
    connect_row_ = 0;
    page_age_ = 0.0f;
    age_ = 0.0f;
    // a code the phone's page must send back, so only someone who can see the TV sets the key
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char code[8];
    std::snprintf(code, sizeof(code), "%04d", static_cast<int>((now / 7919) % 9000 + 1000));
    pairing_code_ = code;
    phone_address_.clear();
    phone_error_.clear();
    phone_poll_ = 1.0f;
    page_focus_.snap({kMargin, kConnectRowsY, kLeftW, kRowH});
    helper_.ask({"PHONE", pairing_code_}, [this](Reply &reply) {
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
            phone_error_ = "This console has no network address, so a phone cannot reach it. Type the key instead.";
        else
            phone_address_ = lan + ":" + port;
    }, true);
}

void App::close_phone()
{
    helper_.ask({"PHONE"}, nullptr, true);
}

void App::leave_connect()
{
    close_phone();
    pending_slot_.clear();
    screen_ = connect_back_;
    age_ = 0.3f;
    if (screen_ == Screen::settings)
        page_focus_.snap(settings_in_rows_ ? settings_row_rect(settings_category_, settings_row_[settings_category_])
                                           : settings_rail_rect(settings_category_));
}

void App::key_added(const std::string &how)
{
    key_state_ = "user";
    helper_.ask({"KEYSTATE"}, [this](Reply &reply) {
        for (const auto &row : reply.rows())
            if (row.size() >= 3 && row[0] == "key")
                key_hint_ = row[2];
    }, true);
    toast("Your SteamGridDB key is in", how + " Art Swap uses it from now on.");
    if (screen_ != Screen::connect)
        return;
    close_phone();
    const std::string slot = pending_slot_;
    pending_slot_.clear();
    screen_ = connect_back_;
    age_ = 0.3f;
    if (!slot.empty() && screen_ == Screen::library && focused_title())
        open_picker(focus_, slot);
}

// The picker opens with or without a key: without one it has the person's own pictures (pictures.cpp), and □
// adds a key
void App::want_picker(const std::string &slot, ui::Feedback &feedback)
{
    feedback.play(Cue::open);
    open_picker(focus_, slot);
}

void App::update_connect(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = std::clamp(connect_row_ + (input.nav == Direction::down ? 1 : -1), 0, 1);
        if (next != connect_row_)
        {
            connect_row_ = next;
            feedback.play(Cue::focus, 1.0f, -0.3f);
        }
        else if (!input.nav_repeat)
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        leave_connect();
        return;
    }
    if (input.is_pressed(Action::confirm))
    {
        feedback.play(Cue::select);
        if (connect_row_ == 0)
            type_key();
        else
        {
            leave_connect();
            return;
        }
    }
    // has the key page taken a key?
    if (!phone_address_.empty() && !phone_asking_ && (phone_poll_ -= dt) <= 0.0f)
    {
        phone_poll_ = 1.0f;
        phone_asking_ = true;
        helper_.ask({"KEYSTATE"}, [this](Reply &reply) {
            phone_asking_ = false;
            if (!reply.ok || screen_ != Screen::connect)
                return;
            for (const auto &row : reply.rows())
                if (row.size() >= 2 && row[0] == "key" && row[1] == "user")
                    key_added("Your phone sent it and SteamGridDB accepted it.");
        }, true);
    }
    page_focus_.target({kMargin, kConnectRowsY + static_cast<float>(connect_row_) * kRowPitch, kLeftW, kRowH});
    page_focus_.update(dt, 22.0f);
}

void App::draw_connect(gfx::DrawList &list) const
{
    const float in = tween::stagger(age_, 0, 0.06f, 0.5f);
    draw_header(list, fonts_, "Connect SteamGridDB", in);

    // ---- the left: what this is, the steps, the other ways ----
    const float left_in = tween::stagger(age_, 1, 0.06f, 0.5f);
    list.push_opacity(left_in);
    ui::paragraph(list, fonts_.regular,
                  "Art Swap gets icons and backgrounds from SteamGridDB, where people share game art. "
                  "Add a free API key of your own to start.",
                  kMargin, 262, 26, kLeftW - 20, 38, kText.with_alpha(0.72f), 3);
    const Rect steps{kMargin, 376, kLeftW, 222};
    list.bordered_rect(steps, kRadius, kDeep.with_alpha(0.32f), 1.5f, kText.with_alpha(0.06f));
    ui::text(list, fonts_.semibold, "THREE STEPS", steps.x + kRowPad, steps.y + 40, 15, kText.with_alpha(0.5f),
             gfx::Align::left, 3.0f);
    static const char *step_text[] = {"Sign in at steamgriddb.com, or make a free account",
                                      "Open Preferences, then API, and copy your key",
                                      "Scan the code with your phone and paste the key"};
    for (int i = 0; i < 3; ++i)
    {
        const float cy = steps.y + 84 + static_cast<float>(i) * 50;
        list.ring(steps.x + kRowPad + 15, cy, 15, 2.0f, kAccent);
        ui::text(list, fonts_.semibold, std::to_string(i + 1), steps.x + kRowPad + 15, centred(cy, 17), 17, kAccent,
                 gfx::Align::center);
        ui::text(list, fonts_.regular, step_text[i], steps.x + kRowPad + 46, centred(cy, 24), 24, kText.with_alpha(0.9f));
    }
    list.pop_opacity();

    list.push_opacity(tween::stagger(age_, 3, 0.06f, 0.4f));
    draw_focus(list, page_focus_.value(), ui::breathe(clock_));
    list.pop_opacity();
    const RowInfo rows[2] = {{"Type it with the keyboard instead", "32 characters", true},
                             {connect_back_ == Screen::settings ? "Back to Settings" : "Not now", ""}};
    for (int i = 0; i < 2; ++i)
    {
        list.push_opacity(tween::stagger(age_, 3 + i, 0.06f, 0.4f));
        draw_row(list, fonts_, {kMargin, kConnectRowsY + static_cast<float>(i) * kRowPitch, kLeftW, kRowH}, rows[i],
                 i == connect_row_);
        list.pop_opacity();
    }
    list.push_opacity(tween::stagger(age_, 6, 0.07f, 0.5f));
    list.circle(kMargin + 6, 866, 4, kAccent);
    ui::paragraph(list, fonts_.regular,
                  "Without a key you can still use pictures of your own, and put back your games' original art.",
                  kMargin + 24, 874, 22, kLeftW - 40, 32, kText.with_alpha(0.55f), 2);
    list.pop_opacity();

    // ---- the right: the code to scan ----
    const float panel_in = tween::stagger(age_, 2, 0.06f, 0.55f);
    list.push_opacity(panel_in);
    draw_panel(list, kCodePanel);
    const Rect &p = kCodePanel;
    ui::text(list, fonts_.semibold, "Scan with your phone", p.x + kPanelPad + kRowPad, p.y + 66, 36, kText);
    ui::text(list, fonts_.regular, "Any phone or computer on the same network", p.x + kPanelPad + kRowPad, p.y + 104, 22,
             kText.with_alpha(0.55f));
    const Rect plate{p.x + kPanelPad + kRowPad, p.y + 140, 360, 360};
    const float info_x = plate.x + plate.w + 44;
    if (!phone_error_.empty())
    {
        list.bordered_rect(plate, kRadius, kDeep.with_alpha(0.32f), 1.5f, kWarn.with_alpha(0.5f));
        ui::paragraph(list, fonts_.regular, phone_error_, plate.x + 28, plate.y + 60, 24, plate.w - 56, 36, kWarn, 6);
    }
    else if (phone_address_.empty())
    {
        list.bordered_rect(plate, kRadius, kDeep.with_alpha(0.32f), 1.5f, kText.with_alpha(0.06f));
        draw_spinner(list, plate.cx(), plate.cy(), 22);
    }
    else
    {
        const std::string url = "http://" + phone_address_ + "/" + pairing_code_;
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        list.glow(plate, kRadius, 24, kAccent.with_alpha(0.18f));
        list.rounded_rect(plate, kRadius, Color::rgb(0xffffff));
        const float cell = (plate.w - 52.0f) / static_cast<float>(qr.getSize());
        for (int my = 0; my < qr.getSize(); ++my)
            for (int mx = 0; mx < qr.getSize(); ++mx)
                if (qr.getModule(mx, my))
                    list.rounded_rect({plate.x + 26 + mx * cell, plate.y + 26 + my * cell, cell + 0.6f, cell + 0.6f}, 0,
                                      kDeep);
    }
    // the address and the code, for typing
    ui::text(list, fonts_.semibold, "OR OPEN", info_x, plate.y + 36, 15, kText.with_alpha(0.5f), gfx::Align::left, 3.0f);
    ui::text(list, fonts_.mono, phone_address_.empty() ? "\xE2\x80\xA6" : phone_address_, info_x, plate.y + 82, 30, kText);
    ui::text(list, fonts_.semibold, "AND ENTER THE CODE", info_x, plate.y + 152, 15, kText.with_alpha(0.5f),
             gfx::Align::left, 3.0f);
    const Rect code{info_x, plate.y + 172, 260, 92};
    list.bordered_rect(code, kRadius, kAccent.with_alpha(0.12f), 1.5f, kAccent.with_alpha(0.5f));
    ui::text(list, fonts_.mono, pairing_code_, code.cx() + 8, centred(code.cy(), 52), 52, kText, gfx::Align::center,
             14.0f);
    // the state
    const float sy = plate.y + plate.h - 30;
    if (phone_error_.empty())
    {
        draw_spinner(list, info_x + 14, sy, 13);
        ui::text(list, fonts_.regular, "Waiting for your phone\xE2\x80\xA6", info_x + 40, centred(sy, 23), 23,
                 kText.with_alpha(0.8f));
    }
    draw_description(list, fonts_, p, "The page closes when you leave this screen, or after ten minutes.");
    list.pop_opacity();
}

} // namespace artswap
