/*
 * Art Swap - the person's own pictures: icons, backgrounds and logos they
 * made or found themselves, first in the picker's grid on every tab.
 *
 * They are kept on the console in /data/artswap/pictures. A phone or a
 * computer on the same network sends them: the add cell, first in the grid,
 * opens a sheet with a QR code and the helper serves a page for the code
 * shown (PHONE <code> pictures, phone.c), as it does for the key and music.
 * FTP into the folder works as well. The helper reads one as "mine:<name>"
 * wherever it reads a SteamGridDB picture, so the frame page, the preview and
 * APPLY or LOGO treat them alike. With no SteamGridDB key the picker opens all
 * the same, with the person's own pictures only.
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
#include <cstdio>
#include <cstdlib>

namespace artswap
{

namespace
{

namespace gfx = hui::gfx;
namespace ui = hui::ui;
using gfx::Color;
using gfx::Rect;
using hui::Action;
using hui::audio::Cue;

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);
const Color kNight = Color::rgb(0x05070d);
const Color kAmber = Color::rgb(0xf5a524);
const Color kGood = Color::rgb(0x3ddc84);
constexpr float kPanelX = 1268.0f;
constexpr float kPanelW = 556.0f;
constexpr float kGridTop = 378.0f;

// "Logo.PNG" -> "PNG", "photo.jpeg" -> "JPEG"
std::string format_of(const std::string &name)
{
    const std::size_t dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    for (char &c : ext)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext == "JPG" ? "JPEG" : ext;
}

// A phone, drawn small: its outline, its screen's top bar, and an arrow up out of it
void draw_phone_glyph(gfx::DrawList &list, float cx, float cy, float s, Color ink)
{
    list.bordered_rect({cx - 14 * s, cy - 22 * s, 28 * s, 44 * s}, 6 * s, Color::rgb(0x000000, 0.0f), 2.5f * s, ink);
    list.line(cx - 4 * s, cy + 16 * s, cx + 4 * s, cy + 16 * s, 2.5f * s, ink);
    list.line(cx, cy + 6 * s, cx, cy - 12 * s, 2.5f * s, ink);
    list.line(cx - 6 * s, cy - 6 * s, cx, cy - 12 * s, 2.5f * s, ink);
    list.line(cx + 6 * s, cy - 6 * s, cx, cy - 12 * s, 2.5f * s, ink);
}

} // namespace

// The person's pictures first, then SteamGridDB's art after them (set_tab). They are asked for before SteamGridDB's
// list, so their places at the front of the grid never move under a thumbnail on its way.
void App::load_mine()
{
    mine_loading_ = true;
    const int gen = generation_;
    helper_.ask({"PICTURES"}, [this, gen](Reply &reply) {
        if (gen != generation_)
            return;
        mine_loading_ = false;
        auto add = std::make_unique<Art>();
        add->add = true;
        add->asked = true; // nothing to show but itself
        arts_.push_back(std::move(add));
        // an older helper answers no PICTURES: the add cell alone, and its sheet says what went wrong
        for (const auto &row : reply.ok ? reply.rows() : std::vector<std::vector<std::string>>{})
        {
            if (row.size() >= 2 && row[0] == "folder")
                mine_folder_ = row[1];
            else if (row.size() >= 6 && row[0] == "F")
            {
                auto art = std::make_unique<Art>();
                art->url = "mine:" + row[1];
                art->author = row[1];
                art->style = format_of(row[1]);
                art->w = std::atoi(row[3].c_str());
                art->h = std::atoi(row[4].c_str());
                art->alpha = row[5] == "1";
                art->mine = true;
                arts_.push_back(std::move(art));
            }
        }
        mine_arts_ = static_cast<int>(arts_.size());
        // a picture just sent is chosen; otherwise SteamGridDB's first takes the focus when it comes
        art_focus_ = 0;
        for (int i = 1; i < mine_arts_ && !mine_focus_.empty(); ++i)
            if (arts_[static_cast<std::size_t>(i)]->url == "mine:" + mine_focus_)
            {
                art_focus_ = i;
                art_moved_ = true;
            }
        if (art_focus_ == 0 && mine_arts_ > 1 && key_state_ != "user")
            art_focus_ = 1;
        mine_focus_.clear();
        cell_ring_.snap(cell_rect(art_focus_));
        request_thumbs();
        if (art_focus_ > 0)
            preview_due_ = 0.0f;
        if (key_state_ == "user")
            match_and_list();
    }, true, gen);
}

// The focused picture of the person's own deleted from the console. The grid is made again, as after pictures
// arrive (thumbnails on their way find their cells by place), with the next of theirs chosen.
void App::delete_picture()
{
    sheet_ = Sheet::none;
    const Art *art = focused_art();
    if (art == nullptr || !art->mine)
        return;
    const std::string name = art->url.substr(5);
    std::string next;
    const int after = art_focus_ + 1 < mine_arts_ ? art_focus_ + 1 : art_focus_ - 1;
    if (after >= 0 && after < mine_arts_ && arts_[static_cast<std::size_t>(after)]->mine)
        next = arts_[static_cast<std::size_t>(after)]->url.substr(5);
    helper_.ask({"PICDEL", name}, [this, name, next](Reply &reply) {
        if (!reply.ok)
        {
            toast(name + " was not deleted", reply.error, true);
            return;
        }
        toast(name + " is deleted", "Games already using it keep it.");
        if (screen_ != Screen::picker)
            return;
        mine_focus_ = next;
        set_tab(tab_);
    }, true);
}

void App::open_pictures_phone()
{
    sheet_ = Sheet::pictures_phone;
    // a code the page sends with each picture, so only someone who can see the TV sends them
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char code[8];
    std::snprintf(code, sizeof(code), "%04d", static_cast<int>((now / 7919) % 9000 + 1000));
    pairing_code_ = code;
    phone_address_.clear();
    phone_error_.clear();
    pictures_seen_.clear();
    pictures_arrived_.clear();
    for (const auto &art : arts_)
        if (art->mine)
            pictures_seen_.push_back(art->url.substr(5));
    pictures_poll_ = 1.5f;
    helper_.ask({"PHONE", pairing_code_, "pictures"}, [this](Reply &reply) {
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
            phone_error_ = "This console has no network address, so a phone cannot reach it. Copy pictures to " +
                           (mine_folder_.empty() ? std::string("/data/artswap/pictures") : mine_folder_) +
                           " over FTP instead.";
        else
            phone_address_ = lan + ":" + port;
    }, true);
}

void App::update_pictures_sheet(const hui::InputFrame &input, ui::Feedback &feedback)
{
    // the folder, asked for again while the phone sends: each new picture ticked off
    if ((pictures_poll_ -= dt_) <= 0.0f)
    {
        pictures_poll_ = 1.5f;
        helper_.ask({"PICTURES"}, [this](Reply &reply) {
            if (sheet_ != Sheet::pictures_phone || !reply.ok)
                return;
            for (const auto &row : reply.rows())
                if (row.size() >= 2 && row[0] == "F" &&
                    std::find(pictures_seen_.begin(), pictures_seen_.end(), row[1]) == pictures_seen_.end())
                {
                    pictures_seen_.push_back(row[1]);
                    pictures_arrived_.push_back(row[1]);
                }
        }, true);
    }
    if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm))
    {
        feedback.play(Cue::modal_close);
        close_phone();
        sheet_ = Sheet::none;
        // the grid again, with what came; the last to arrive chosen
        if (!pictures_arrived_.empty())
        {
            mine_focus_ = pictures_arrived_.back();
            set_tab(tab_);
        }
    }
}

// The phone page's address as a QR code on a white plate, or why there is none
void App::draw_qr(gfx::DrawList &list, const Rect &plate) const
{
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
}

void App::draw_pictures_sheet(gfx::DrawList &list, const Rect &panel, float x, float y) const
{
    const Title *title = picker_title_ >= 0 ? titles_[static_cast<std::size_t>(picker_title_)].get() : nullptr;
    const Color accent = title ? title->accent : Color::rgb(0x9fd8ff);
    ui::text(list, fonts_.semibold, "SEND PICTURES", x, y, 19, accent, gfx::Align::left, 4.0f);
    ui::text(list, fonts_.display, "From your phone or computer", x - 2, y + 62, 48, kWhite);
    y += 120;
    const Rect plate{x, y, 330, 330};
    draw_qr(list, plate);
    const float info_x = plate.x + plate.w + 48;
    const float width = panel.x + panel.w - 64 - info_x;
    float ty = ui::paragraph(list, fonts_.regular,
                             "Scan the code, or open the address in a browser on the same network, and choose "
                             "pictures: PNG or JPEG (others are turned into PNG). They show first in every tab. A "
                             "logo wants a PNG with a see-through background.",
                             info_x, plate.y + 24, 24, width, 36, kWhite.with_alpha(0.85f), 5) +
               30;
    ui::text(list, fonts_.semibold, "OR OPEN", info_x, ty, 15, kWhite.with_alpha(0.5f), gfx::Align::left, 3.0f);
    ui::text(list, fonts_.mono, phone_address_.empty() ? "\xE2\x80\xA6" : phone_address_, info_x, ty + 40, 28, kWhite);
    ui::text(list, fonts_.semibold, "AND ENTER THE CODE", info_x, ty + 96, 15, kWhite.with_alpha(0.5f),
             gfx::Align::left, 3.0f);
    ui::text(list, fonts_.mono, pairing_code_, info_x, ty + 150, 46, kWhite, gfx::Align::left, 12.0f);
    // what has come, each ticked off
    if (!pictures_arrived_.empty())
    {
        float ay = plate.y + plate.h + 58;
        const int shown = std::min<int>(3, static_cast<int>(pictures_arrived_.size()));
        for (int i = static_cast<int>(pictures_arrived_.size()) - shown; i < static_cast<int>(pictures_arrived_.size());
             ++i)
        {
            list.circle(x + 12, ay - 8, 11, kGood.with_alpha(0.2f));
            list.line(x + 6, ay - 8, x + 10, ay - 4, 2.5f, kGood);
            list.line(x + 10, ay - 4, x + 18, ay - 13, 2.5f, kGood);
            ui::text(list, fonts_.regular,
                     fonts_.regular.font->fit(pictures_arrived_[static_cast<std::size_t>(i)] + " is here", 23,
                                              panel.x + panel.w - 64 - (x + 40)),
                     x + 40, ay, 23, kWhite);
            ay += 36;
        }
    }
    else if (phone_error_.empty())
    {
        draw_spinner(list, x + 14, plate.y + plate.h + 50, 13);
        ui::text(list, fonts_.regular, "Waiting for pictures\xE2\x80\xA6", x + 40, plate.y + plate.h + 58, 23,
                 kWhite.with_alpha(0.8f));
    }
}

// The first cell of the grid: more pictures of the person's own
void App::draw_add_cell(gfx::DrawList &list, const Rect &r, bool focused) const
{
    list.rounded_rect(r, 14, kWhite.with_alpha(focused ? 0.12f : 0.06f));
    list.bordered_rect(r.inset(6), 10, Color::rgb(0x000000, 0.0f), 2.0f, kWhite.with_alpha(focused ? 0.55f : 0.3f));
    const Color ink = kWhite.with_alpha(focused ? 1.0f : 0.8f);
    if (r.h >= 180)
    {
        // a square: the phone, then two lines under it
        draw_phone_glyph(list, r.cx(), r.y + r.h * 0.36f, 1.1f, ink);
        ui::text(list, fonts_.semibold, "Your own", r.cx(), r.y + r.h * 0.7f, 20, ink, gfx::Align::center);
        ui::text(list, fonts_.regular, "from a phone", r.cx(), r.y + r.h * 0.7f + 26, 18, ink.with_alpha(0.7f),
                 gfx::Align::center);
    }
    else
    {
        // a wide cell: the phone at the left, the words beside it
        draw_phone_glyph(list, r.x + 56, r.cy(), 1.0f, ink);
        ui::text(list, fonts_.semibold, "Your own pictures", r.x + 96, r.cy() - 4, 22, ink);
        ui::text(list, fonts_.regular, "from a phone or computer", r.x + 96, r.cy() + 24, 18, ink.with_alpha(0.7f));
    }
}

// The picker's right side on the add cell: what it does, and where the pictures are kept
void App::draw_add_panel(gfx::DrawList &list) const
{
    const float x = kPanelX, w = kPanelW;
    float y = kGridTop + 12;
    ui::text(list, fonts_.semibold, "YOUR OWN PICTURES", x, y, 17, kWhite.with_alpha(0.6f), gfx::Align::left, 3.0f);
    y += 54;
    const int count = std::max(0, mine_arts_ - 1);
    ui::text(list, fonts_.display,
             count == 0 ? std::string("None yet") : std::to_string(count) + (count == 1 ? " picture" : " pictures"), x,
             y, 44, kWhite);
    y += 46;
    y = ui::paragraph(list, fonts_.regular,
                      "Send icons, backgrounds or logos from a phone or computer on the same network: they show here, "
                      "first in every tab, for every game. A logo wants a PNG with a see-through background. OPTIONS "
                      "on one deletes it.",
                      x, y, 24, w, 36, kWhite.with_alpha(0.85f), 6) +
        20;
    ui::draw_button(list, fonts_, ui::GlyphStyle::dark(), ui::Button::cross, x, y, 30);
    ui::text(list, fonts_.semibold, "Show the QR code", x + 44, y + 8, 23, kWhite);
    y += 70;
    ui::text(list, fonts_.semibold, "OR COPY THEM OVER FTP TO", x, y, 15, kWhite.with_alpha(0.5f), gfx::Align::left,
             3.0f);
    ui::text(list, fonts_.mono,
             fonts_.mono.font->fit(mine_folder_.empty() ? std::string("/data/artswap/pictures") : mine_folder_, 22, w),
             x, y + 36, 22, kWhite.with_alpha(0.85f));
    // with no key, the only pictures there are: where SteamGridDB's come from
    if (key_state_ != "user")
    {
        y += 96;
        list.circle(x + 6, y - 8, 5, kAmber);
        ui::paragraph(list, fonts_.regular,
                      "SteamGridDB has art for most games, made by its community. Add your own free key to see it "
                      "here too.",
                      x + 24, y, 21, w - 24, 31, kWhite.with_alpha(0.7f), 3);
    }
}

} // namespace artswap
