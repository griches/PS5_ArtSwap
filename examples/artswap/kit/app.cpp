/*
 * Art Swap - the screens (app.hpp).
 *
 * The library is laid out like the console's own home screen, after the
 * kit's Aurora Shelf design: the focused title's background fills the
 * screen, its name and what can be changed on the left, its icon large on
 * the right, every title's icon on a shelf below. The picker keeps that frame
 * and swaps the shelf for a grid of SteamGridDB's art, with a preview of
 * exactly what would be written (the helper makes it) beside the current
 * art. Changing anything goes through a confirmation that says the original
 * is kept, so Restore can put it back.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app.hpp"

#include "audio/cues.hpp"
#include "qrcodegen.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

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
constexpr float kCard = 196.0f;
constexpr float kCardGap = 26.0f;
constexpr float kCardGrow = 1.2f;
constexpr float kShelfY = 770.0f;
constexpr float kGridX = 96.0f;
constexpr float kGridTop = 378.0f;
constexpr float kGridWidth = 1104.0f;
constexpr float kGridBottom = 950.0f;
constexpr float kGridGap = 24.0f;
constexpr float kPanelX = 1268.0f;
constexpr float kPanelW = 556.0f;

struct Kind
{
    const char *tab;   // shown
    const char *slot;  // the art it changes: icon, pic or logo
    const char *kind;  // the helper's LIST kind
    float cell_w, cell_h;
    bool fit;          // the framing it starts with
};

// the picker's tabs: SteamGridDB's square grids for the tile, its heroes (made for backgrounds) for pic0, its
// logos (transparent, mostly white lettering) for the game's page, where its name would show
const Kind kKinds[] = {
    {"Icons", "icon", "square", 200, 200, false},
    {"Backgrounds", "pic", "heroes", 520, 168, false},
    {"Logos", "logo", "logos", 344, 150, false},
};

const Kind &kind_for(const std::vector<int> &kinds, int tab)
{
    if (kinds.empty())
        return kKinds[0];
    return kKinds[kinds[static_cast<std::size_t>(std::clamp(tab, 0, static_cast<int>(kinds.size()) - 1))]];
}

// The logo on a title's page, in words
std::string logo_label(const std::string &logo)
{
    return logo == "art" ? "custom" : logo == "other" ? "its own" : "none";
}

// The size in a slot's description ("dds-bc7 3840x2160 x4"), 0 when there is none
void desc_size(const std::string &desc, int &w, int &h)
{
    w = h = 0;
    std::size_t start = 0;
    while (start < desc.size())
    {
        const std::size_t end = desc.find(' ', start);
        const std::string word = desc.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (std::sscanf(word.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
            return;
        w = h = 0;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
}

// A number for a request: three decimals, as the helper reads it
std::string number(float value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(value));
    return text;
}

std::string upper(const std::string &text)
{
    return ui::upper(text);
}

// "png 512x512 x2" -> "PNG 512×512, 2 files"
std::string describe(const std::string &desc)
{
    if (desc.empty() || desc == "-")
        return "none";
    char format[32] = "", size[32] = "";
    int copies = 1;
    if (std::sscanf(desc.c_str(), "%31s %31s x%d", format, size, &copies) < 2)
        return desc;
    std::string out = upper(format);
    for (std::size_t at; (at = out.find("DDS-")) != std::string::npos;)
        out.replace(at, 4, "DDS ");
    std::string dims = size;
    const std::size_t x = dims.find('x');
    if (x != std::string::npos)
        dims.replace(x, 1, "\xC3\x97");
    out += " " + dims;
    if (copies > 1)
        out += ", " + std::to_string(copies) + " files";
    return out;
}

std::string kind_label(const std::string &kind)
{
    if (kind == "homebrew")
        return "HOMEBREW";
    if (kind == "ps4")
        return "PS4 GAME";
    return "PS5 GAME";
}

// The texture rectangle that fills a w x h frame from a tw x th picture: the largest part of the
// frame's shape, made zoom times smaller, placed by pan -1..1 across and down (as the helper's
// image_cover cuts it)
Rect cover_uv(int tw, int th, float w, float h, float pan_x = 0.0f, float pan_y = 0.0f, float zoom = 1.0f)
{
    if (tw <= 0 || th <= 0)
        return gfx::kFullUv;
    const float a = static_cast<float>(tw) / th, d = w / h;
    zoom = std::clamp(zoom, 1.0f, 8.0f);
    const float uw = (a > d ? d / a : 1.0f) / zoom, vh = (a > d ? 1.0f : a / d) / zoom;
    return {(1.0f - uw) * (std::clamp(pan_x, -1.0f, 1.0f) + 1.0f) * 0.5f,
            (1.0f - vh) * (std::clamp(pan_y, -1.0f, 1.0f) + 1.0f) * 0.5f, uw, vh};
}

// The rectangle inside frame that shows a tw x th picture whole
Rect contain(const Rect &frame, int tw, int th)
{
    if (tw <= 0 || th <= 0)
        return frame;
    const float s = std::min(frame.w / tw, frame.h / th);
    const float w = tw * s, h = th * s;
    return {frame.x + (frame.w - w) * 0.5f, frame.y + (frame.h - h) * 0.5f, w, h};
}

// Dark, middle and accent colours from a picture, for the backdrop and the glow
void palette_of(const Picture &p, Color &dark, Color &mid, Color &accent)
{
    double r = 0, g = 0, b = 0;
    int n = 0;
    float best = -1.0f;
    Color vivid = Color::rgb(0x7ff0d8);
    for (int y = 0; y < p.h; y += 6)
        for (int x = 0; x < p.w; x += 6)
        {
            const std::uint8_t *px = &p.rgba[(static_cast<std::size_t>(y) * p.w + x) * 4];
            if (px[3] < 128)
                continue;
            r += px[0];
            g += px[1];
            b += px[2];
            n++;
            const int hi = std::max({px[0], px[1], px[2]}), lo = std::min({px[0], px[1], px[2]});
            const float score = static_cast<float>(hi - lo) * (0.4f + hi / 425.0f);
            if (score > best)
            {
                best = score;
                vivid = {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f, 1.0f};
            }
        }
    if (n == 0)
        return;
    mid = {static_cast<float>(r / n / 255.0), static_cast<float>(g / n / 255.0), static_cast<float>(b / n / 255.0),
           1.0f};
    dark = gfx::mix(mid, kNight, 0.7f);
    // the accent bright enough to read on the dark page
    const float top = std::max({vivid.r, vivid.g, vivid.b});
    if (top > 0.01f && top < 0.85f)
    {
        const float lift = 0.85f / top;
        vivid = {std::min(1.0f, vivid.r * lift), std::min(1.0f, vivid.g * lift), std::min(1.0f, vivid.b * lift), 1.0f};
    }
    accent = best > 30.0f ? vivid : Color::rgb(0x9fd8ff);
}

} // namespace

struct App::Title
{
    std::string id, name, kind, icon_desc, pic_desc, sgdb_name;
    bool kept = false;      // an original of its art is kept
    bool name_kept = false; // its original name is kept (renamed)
    bool refresh = true;    // ShadowMountPlus can install it again (R2); otherwise only a restart shows new art
    long store_concept = 0; // its PlayStation Store concept: 0 for homebrew
    std::string logo = "-"; // the logo on its page: art (Art Swap's), other (its own) or - (none: its name shows)
    bool listed = true;     // the home screen has an entry for it, so it can have a logo
    bool added = false;     // Art Swap added its background (it had none): Restore deletes it
    // a background to change, or one Art Swap can add (ShadowMountPlus installs the game again for it)
    bool has_pic() const
    {
        return !(pic_desc == "-" || pic_desc.empty());
    }
    long sgdb_id = 0;
    // changed by Art Swap: art (originals kept), a name or a logo, so Restore has something to put back
    bool changed() const
    {
        return kept || name_kept || logo == "art";
    }
    std::uint32_t icon = 0;
    bool icon_asked = false, icon_failed = false;
    std::uint32_t background = 0;
    int background_w = 0, background_h = 0;
    bool background_asked = false, background_failed = false;
    float background_used = 0.0f;
    Color dark = Color::rgb(0x0e121c), mid = Color::rgb(0x27324a), accent = Color::rgb(0x9fd8ff);
};

struct App::Art
{
    std::string url, thumb, style, author, mime;
    bool store = false; // the game's own logo, from PlayStation Store
    int w = 0, h = 0;
    std::uint32_t texture = 0;
    int tw = 0, th = 0;
    bool asked = false, failed = false, tried_full = false;
};

App::App(const ui::Fonts &fonts, Helper &helper, TextureSink &textures, Keyboard *keyboard)
    : fonts_(fonts), helper_(helper), textures_(textures), keyboard_(keyboard)
{
    palette_[0].snap(Color::rgb(0x070910));
    palette_[1].snap(Color::rgb(0x0e1424));
    palette_[2].snap(Color::rgb(0x1d2a44));
    palette_[3].snap(Color::rgb(0x3b5b8c));
    helper_.start();
}

App::~App()
{
    for (auto &title : titles_)
    {
        forget(title->icon);
        forget(title->background);
    }
    for (auto &art : arts_)
        forget(art->texture);
    forget(preview_);
    forget(current_art_);
    forget(frame_picture_);
}

Color App::accent() const
{
    const Title *title = focused_title();
    return title ? title->accent : Color::rgb(0x9fd8ff);
}

// ---------------------------------------------------------------- textures

void App::upload(Picture picture, std::function<void(std::uint32_t, const Picture &)> done)
{
    uploads_.push_back({std::move(picture), std::move(done)});
}

void App::forget(std::uint32_t &texture)
{
    if (texture != 0)
        textures_.destroy(texture);
    texture = 0;
}

void App::pump()
{
    helper_.pump();
    if (keyboard_)
        keyboard_->pump();
    // a few uploads a frame: each waits for the queue
    for (int i = 0; i < 6 && !uploads_.empty(); ++i)
    {
        Upload next = std::move(uploads_.front());
        uploads_.erase(uploads_.begin());
        const std::uint32_t texture =
            next.picture.w > 0 ? textures_.create(next.picture.w, next.picture.h, next.picture.rgba.data()) : 0;
        if (next.done)
            next.done(texture, next.picture);
        else if (texture)
            textures_.destroy(texture);
    }
}

// ---------------------------------------------------------------- titles

App::Title *App::focused_title()
{
    if (focus_ < 0 || focus_ >= static_cast<int>(titles_.size()))
        return nullptr;
    return titles_[static_cast<std::size_t>(focus_)].get();
}

int App::kept_count() const
{
    int kept = 0;
    for (const auto &title : titles_)
        kept += title->kept || title->logo == "art";
    return kept;
}

const App::Title *App::focused_title() const
{
    if (focus_ < 0 || focus_ >= static_cast<int>(titles_.size()))
        return nullptr;
    return titles_[static_cast<std::size_t>(focus_)].get();
}

void App::load_titles(bool rescan)
{
    if (titles_loading_)
        return;
    titles_loading_ = true;
    std::vector<std::string> request{"APPS"};
    if (rescan)
        request.push_back("rescan");
    helper_.ask(request, [this, rescan](Reply &reply) {
        titles_loading_ = false;
        if (!reply.ok)
        {
            titles_error_ = reply.error;
            if (screen_ == Screen::starting)
                screen_ = Screen::library;
            return;
        }
        titles_error_.clear();
        std::string keep_focus = focused_title() ? focused_title()->id : "";
        std::vector<std::unique_ptr<Title>> old = std::move(titles_);
        titles_.clear();
        for (const auto &row : reply.rows())
        {
            if (row.size() < 9 || row[0] != "A")
                continue;
            auto title = std::make_unique<Title>();
            title->id = row[1];
            title->name = row[2];
            title->kind = row[3];
            title->icon_desc = row[4];
            title->pic_desc = row[5];
            title->kept = row[6] == "1";
            title->name_kept = row.size() > 9 && row[9] == "1";
            title->refresh = row.size() <= 10 || row[10] == "1";
            if (row.size() > 13)
            {
                title->store_concept = std::strtol(row[11].c_str(), nullptr, 10);
                title->logo = row[12];
                title->listed = row[13] == "1";
            }
            title->added = row.size() > 14 && row[14] == "1";
            title->sgdb_id = std::strtol(row[7].c_str(), nullptr, 10);
            title->sgdb_name = row[8];
            // keep the pictures already loaded, unless this is a fresh look
            for (auto &before : old)
                if (before && before->id == title->id && !rescan)
                {
                    title->icon = before->icon;
                    title->icon_asked = before->icon_asked;
                    title->background = before->background;
                    title->background_w = before->background_w;
                    title->background_h = before->background_h;
                    title->background_asked = before->background_asked;
                    title->dark = before->dark;
                    title->mid = before->mid;
                    title->accent = before->accent;
                    before->icon = before->background = 0;
                }
            titles_.push_back(std::move(title));
        }
        for (auto &before : old)
            if (before)
            {
                if (before->background == backdrop_ || before->background == backdrop_old_)
                    backdrop_ = backdrop_old_ = 0;
                forget(before->icon);
                forget(before->background);
            }
        focus_ = 0;
        for (int i = 0; i < static_cast<int>(titles_.size()); ++i)
            if (titles_[static_cast<std::size_t>(i)]->id == keep_focus)
                focus_ = i;
        shown_ = -1;
        load_pending();
        helper_.ask({"STORE"}, [this](Reply &answer) {
            for (const auto &r : answer.rows())
                if (r.size() >= 2 && r[0] == "store")
                    store_logos_ = r[1] == "on";
        }, true);
        if (screen_ == Screen::starting)
        {
            screen_ = Screen::library;
            age_ = 0.0f;
        }
        // icons outward from the focus, so the first ones on screen come first
        const int count = static_cast<int>(titles_.size());
        for (int d = 0; d < count; ++d)
            for (int side : {1, -1})
            {
                const int i = focus_ + side * d;
                if (i >= 0 && i < count && (d > 0 || side == 1))
                    request_icon(*titles_[static_cast<std::size_t>(i)]);
            }
        grow_.assign(titles_.size(), tween::Spring{});
        if (!grow_.empty())
            grow_[static_cast<std::size_t>(focus_)].snap(1.0f);
        if (key_state_ == "none" && !key_explained_ && sheet_ == Sheet::none && screen_ == Screen::library)
        {
            key_explained_ = true;
            open_connect("");
        }
    }, true);
}

void App::request_icon(Title &title)
{
    if (title.icon_asked)
        return;
    title.icon_asked = true;
    const std::string id = title.id;
    helper_.ask({"ART", id, "icon", "256", "256"}, [this, id](Reply &reply) {
        Title *t = nullptr;
        for (auto &each : titles_)
            if (each->id == id)
                t = each.get();
        if (t == nullptr)
            return;
        if (!reply.ok)
        {
            t->icon_failed = true;
            return;
        }
        upload(std::move(reply.picture), [this, id](std::uint32_t texture, const Picture &picture) {
            for (auto &each : titles_)
                if (each->id == id)
                {
                    forget(each->icon);
                    each->icon = texture;
                    palette_of(picture, each->dark, each->mid, each->accent);
                    return;
                }
            if (texture)
                textures_.destroy(texture);
        });
    });
}

void App::request_background(Title &title)
{
    if (title.background_asked || title.pic_desc == "-" || title.pic_desc.empty())
        return;
    title.background_asked = true;
    const std::string id = title.id;
    helper_.ask({"ART", id, "pic", "960", "540"}, [this, id](Reply &reply) {
        Title *t = nullptr;
        for (auto &each : titles_)
            if (each->id == id)
                t = each.get();
        if (t == nullptr)
            return;
        if (!reply.ok)
        {
            t->background_failed = true;
            return;
        }
        upload(std::move(reply.picture), [this, id](std::uint32_t texture, const Picture &picture) {
            Title *target = nullptr;
            for (auto &each : titles_)
                if (each->id == id)
                    target = each.get();
            if (target == nullptr)
            {
                if (texture)
                    textures_.destroy(texture);
                return;
            }
            forget(target->background);
            target->background = texture;
            target->background_w = picture.w;
            target->background_h = picture.h;
            target->background_used = clock_;
            // at most eight backgrounds held: the least recently shown go
            std::vector<Title *> held;
            for (auto &each : titles_)
                if (each->background && each->background != backdrop_ && each->background != backdrop_old_ &&
                    each.get() != target)
                    held.push_back(each.get());
            std::sort(held.begin(), held.end(),
                      [](Title *a, Title *b) { return a->background_used < b->background_used; });
            for (std::size_t i = 0; i + 7 < held.size(); ++i)
            {
                forget(held[i]->background);
                held[i]->background_asked = false;
            }
        });
    }, true);
}

// ---------------------------------------------------------------- the picker

std::vector<int> App::tab_kinds() const
{
    std::vector<int> out{0};
    // Backgrounds only for a title with a pic0 or pic1 to replace; Logos for one the home screen lists
    if (picker_title_ >= 0)
    {
        const Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
        if (title.has_pic() || title.refresh)
            out.push_back(1);
        if (title.listed)
            out.push_back(2);
    }
    return out;
}

std::vector<const char *> App::tabs() const
{
    std::vector<const char *> out;
    for (int kind : tab_kinds())
        out.push_back(kKinds[kind].tab);
    return out;
}

App::Art *App::focused_art()
{
    if (art_focus_ < 0 || art_focus_ >= static_cast<int>(arts_.size()))
        return nullptr;
    return arts_[static_cast<std::size_t>(art_focus_)].get();
}

const App::Art *App::focused_art() const
{
    if (art_focus_ < 0 || art_focus_ >= static_cast<int>(arts_.size()))
        return nullptr;
    return arts_[static_cast<std::size_t>(art_focus_)].get();
}

void App::open_picker(int title, const std::string &slot)
{
    picker_title_ = title;
    slot_.clear();
    screen_ = Screen::picker;
    age_ = 0.0f;
    list_error_.clear();
    tab_ = 0;
    const std::vector<int> kinds = tab_kinds();
    for (int i = 0; i < static_cast<int>(kinds.size()); ++i)
        if (slot == kKinds[kinds[static_cast<std::size_t>(i)]].slot)
            tab_ = i;
    tab_position_.snap(static_cast<float>(tab_));
    set_tab(tab_);
}

void App::load_current_art()
{
    forget(current_art_);
    current_art_ = 0;
    const int title = picker_title_;
    const std::string slot = slot_;
    const std::string w = slot == "icon" ? "512" : slot == "logo" ? "800" : "960",
                      h = slot == "icon" ? "512" : slot == "logo" ? "400" : "540";
    helper_.ask({"ART", titles_[static_cast<std::size_t>(title)]->id, slot, w, h},
                [this, title, slot](Reply &reply) {
                    if (screen_ != Screen::picker || picker_title_ != title || slot_ != slot || !reply.ok)
                        return;
                    upload(std::move(reply.picture), [this, title, slot](std::uint32_t texture, const Picture &picture) {
                        if (screen_ != Screen::picker || picker_title_ != title || slot_ != slot)
                        {
                            forget(texture);
                            return;
                        }
                        forget(current_art_);
                        current_art_ = texture;
                        current_w_ = picture.w;
                        current_h_ = picture.h;
                    });
                },
                true);
}

void App::close_picker()
{
    helper_.drop(generation_);
    generation_++;
    // the dropped requests never answer, so nothing is still on its way
    listing_ = false;
    matching_ = false;
    for (auto &art : arts_)
        forget(art->texture);
    arts_.clear();
    forget(preview_);
    forget(current_art_);
    forget(frame_picture_);
    frame_url_.clear();
    screen_ = Screen::library;
    age_ = 0.3f;
    picker_title_ = -1;
}

void App::set_tab(int tab)
{
    helper_.drop(generation_);
    generation_++;
    // the dropped requests never answer, so nothing is still on its way
    listing_ = false;
    matching_ = false;
    for (auto &art : arts_)
        forget(art->texture);
    arts_.clear();
    forget(preview_);
    tab_ = tab;
    const Kind &kind = kind_for(tab_kinds(), tab);
    if (slot_ != kind.slot)
    {
        slot_ = kind.slot;
        load_current_art();
    }
    art_focus_ = 0;
    page_ = 0;
    total_ = 0;
    hidden_ = 0;
    store_arts_ = 0;
    store_asked_ = false;
    list_error_.clear();
    grid_scroll_.position.snap(0.0f);
    reset_framing();
    preview_due_ = -1.0f;
    preview_serial_++;
    preview_loading_ = false;
    cell_ring_.snap(cell_rect(0));
    match_and_list();
}

void App::match_and_list()
{
    if (picker_title_ < 0)
        return;
    Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
    if (title.sgdb_id > 0)
    {
        load_page();
        return;
    }
    // not matched yet: SteamGridDB's closest name, kept for next time
    matching_ = true;
    const int gen = generation_;
    const std::string id = title.id, name = title.name;
    helper_.ask({"SEARCH", name}, [this, gen, id, name](Reply &reply) {
        if (gen != generation_)
            return;
        matching_ = false;
        if (!reply.ok)
        {
            list_error_ = reply.error;
            return;
        }
        for (const auto &row : reply.rows())
            if (row.size() >= 3 && row[0] == "G")
            {
                for (auto &t : titles_)
                    if (t->id == id)
                    {
                        t->sgdb_id = std::strtol(row[1].c_str(), nullptr, 10);
                        t->sgdb_name = row[2];
                    }
                helper_.ask({"MATCH", id, row[1], row[2]}, nullptr);
                load_page();
                return;
            }
        list_error_ = "SteamGridDB has no game called \xE2\x80\x9C" + name +
                      "\xE2\x80\x9D. Choose Another game to look for it under another name.";
        // a store game's own logo can show all the same
        for (auto &t : titles_)
            if (t->id == id && slot_ == "logo" && t->store_concept > 0 && store_logos_)
                load_page();
    }, true, gen);
}

void App::load_page()
{
    if (picker_title_ < 0 || listing_)
        return;
    const Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
    listing_ = true;
    const int gen = generation_, page = page_;
    // a store game's own logo first, from PlayStation Store, when the person has turned that on
    if (slot_ == "logo" && page == 0 && title.store_concept > 0 && store_logos_ && !store_asked_ && !store_unreachable_)
    {
        store_asked_ = true;
        helper_.ask({"STORELOGO", title.id}, [this, gen](Reply &reply) {
            if (gen != generation_)
                return;
            listing_ = false;
            if (!reply.ok)
            {
                // once a run: a console that cannot reach the store will not reach it the next time either
                store_unreachable_ = true;
                toast("PlayStation Store's logo did not load", reply.error, true);
            }
            for (const auto &row : reply.rows())
                if (row.size() >= 9 && row[0] == "I")
                {
                    auto art = std::make_unique<Art>();
                    art->url = row[2];
                    art->thumb = row[3];
                    art->w = std::atoi(row[4].c_str());
                    art->h = std::atoi(row[5].c_str());
                    art->style = row[6];
                    art->author = row[7];
                    art->mime = row[8];
                    art->store = true;
                    arts_.push_back(std::move(art));
                    store_arts_++;
                }
            request_thumbs();
            if (!arts_.empty())
                cell_ring_.snap(cell_rect(art_focus_));
            if (titles_[static_cast<std::size_t>(picker_title_)]->sgdb_id > 0)
                load_page();
        }, true, gen);
        return;
    }
    helper_.ask({"LIST", std::to_string(title.sgdb_id), kind_for(tab_kinds(), tab_).kind, std::to_string(page)},
                [this, gen](Reply &reply) {
                    if (gen != generation_)
                        return;
                    listing_ = false;
                    if (!reply.ok)
                    {
                        list_error_ = reply.error;
                        return;
                    }
                    const bool first = static_cast<int>(arts_.size()) == store_arts_;
                    for (const auto &row : reply.rows())
                    {
                        if (row.size() >= 4 && row[0] == "P")
                            total_ = std::atoi(row[2].c_str());
                        else if (row.size() >= 2 && row[0] == "H")
                            hidden_ += std::atoi(row[1].c_str());
                        else if (row.size() >= 9 && row[0] == "I")
                        {
                            auto art = std::make_unique<Art>();
                            art->url = row[2];
                            art->thumb = row[3];
                            art->w = std::atoi(row[4].c_str());
                            art->h = std::atoi(row[5].c_str());
                            art->style = row[6];
                            art->author = row[7];
                            art->mime = row[8];
                            arts_.push_back(std::move(art));
                        }
                    }
                    page_++;
                    request_thumbs();
                    if (first && !arts_.empty())
                    {
                        preview_due_ = 0.0f;
                        cell_ring_.snap(cell_rect(art_focus_));
                    }
                },
                true, gen);
}

void App::request_thumbs()
{
    const Rect cell = cell_size();
    const int cols = columns();
    const float pitch = cell.h + kGridGap;
    const int first_row = std::max(0, static_cast<int>(grid_scroll_.offset() / pitch) - 1);
    const int last_row = first_row + static_cast<int>((kGridBottom - kGridTop) / pitch) + 3;
    const int gen = generation_;
    const int want_w = static_cast<int>(cell.w * 2), want_h = static_cast<int>(cell.h * 2);
    for (int i = first_row * cols; i < std::min(static_cast<int>(arts_.size()), (last_row + 1) * cols); ++i)
    {
        Art &art = *arts_[static_cast<std::size_t>(i)];
        if (art.asked)
            continue;
        art.asked = true;
        const std::string url = art.thumb.empty() ? art.url : art.thumb;
        helper_.ask({"THUMB", url, std::to_string(want_w), std::to_string(want_h)},
                    [this, gen, i, want_w, want_h](Reply &reply) {
                        if (gen != generation_ || i >= static_cast<int>(arts_.size()))
                            return;
                        Art &a = *arts_[static_cast<std::size_t>(i)];
                        if (!reply.ok)
                        {
                            // the thumbnail failed: once from the full picture
                            if (!a.tried_full && !a.url.empty())
                            {
                                a.tried_full = true;
                                helper_.ask({"THUMB", a.url, std::to_string(want_w), std::to_string(want_h)},
                                            [this, gen, i](Reply &again) {
                                                if (gen != generation_ || i >= static_cast<int>(arts_.size()))
                                                    return;
                                                if (!again.ok)
                                                {
                                                    arts_[static_cast<std::size_t>(i)]->failed = true;
                                                    return;
                                                }
                                                upload(std::move(again.picture),
                                                       [this, gen, i](std::uint32_t texture, const Picture &p) {
                                                           if (gen != generation_ ||
                                                               i >= static_cast<int>(arts_.size()))
                                                           {
                                                               if (texture)
                                                                   textures_.destroy(texture);
                                                               return;
                                                           }
                                                           Art &b = *arts_[static_cast<std::size_t>(i)];
                                                           b.texture = texture;
                                                           b.tw = p.w;
                                                           b.th = p.h;
                                                       });
                                            },
                                            false, gen);
                            }
                            else
                                a.failed = true;
                            return;
                        }
                        upload(std::move(reply.picture), [this, gen, i](std::uint32_t texture, const Picture &p) {
                            if (gen != generation_ || i >= static_cast<int>(arts_.size()))
                            {
                                if (texture)
                                    textures_.destroy(texture);
                                return;
                            }
                            Art &b = *arts_[static_cast<std::size_t>(i)];
                            b.texture = texture;
                            b.tw = p.w;
                            b.th = p.h;
                        });
                    },
                    false, gen);
    }
}

void App::request_preview()
{
    const Art *art = focused_art();
    preview_loading_ = false;
    // a logo is written as it is: its own thumbnail shows what it makes
    if (art == nullptr || picker_title_ < 0 || slot_ == "logo")
        return;
    const int serial = ++preview_serial_;
    preview_loading_ = true;
    const bool icon = slot_ == "icon";
    const std::string url = art->url;
    const Framing framing = framing_;
    helper_.ask({"PREVIEW", art->url, slot_, framing.fit ? "fit" : "cover", number(framing.pan_x),
                 number(framing.pan_y), icon ? "512" : "960", icon ? "512" : "540", number(framing.zoom)},
                [this, serial, url, framing](Reply &reply) {
                    if (serial != preview_serial_)
                        return;
                    if (!reply.ok)
                    {
                        preview_loading_ = false;
                        toast("The preview did not load", reply.error, true);
                        return;
                    }
                    upload(std::move(reply.picture), [this, serial, url, framing](std::uint32_t texture, const Picture &p) {
                        if (serial != preview_serial_)
                        {
                            if (texture)
                                textures_.destroy(texture);
                            return;
                        }
                        preview_loading_ = false;
                        forget(preview_);
                        preview_ = texture;
                        preview_url_ = url;
                        preview_framing_ = framing;
                        preview_w_ = p.w;
                        preview_h_ = p.h;
                    });
                },
                true, generation_);
}

// ---------------------------------------------------------------- framing

float App::file_aspect() const
{
    return slot_ == "icon" ? 1.0f : 16.0f / 9.0f;
}

void App::reset_framing()
{
    framing_ = Framing{};
    framing_.fit = kind_for(tab_kinds(), tab_).fit;
    has_custom_ = false;
}

void App::reframed(float delay)
{
    preview_due_ = delay;
    preview_serial_++;
}

std::vector<std::string> App::presets() const
{
    std::vector<std::string> out{"Fit"};
    const Art *art = focused_art();
    const float a = art && art->h > 0 ? static_cast<float>(art->w) / art->h : 1.0f, d = file_aspect();
    if (a > d * 1.02f)
        out.insert(out.end(), {"Left", "Centre", "Right"});
    else if (a < d / 1.02f)
        out.insert(out.end(), {"Top", "Middle", "Bottom"});
    else
        out.emplace_back("Whole");
    if (has_custom_)
        out.emplace_back("Custom");
    return out;
}

App::Framing App::preset(int index) const
{
    const auto names = presets();
    const std::string &name = names[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(names.size()) - 1))];
    Framing f;
    if (name == "Fit")
        f.fit = true;
    else if (name == "Custom")
        f = custom_;
    else if (name == "Left" || name == "Right")
        f.pan_x = name == "Left" ? -1.0f : 1.0f;
    else if (name == "Top" || name == "Bottom")
        f.pan_y = name == "Top" ? -1.0f : 1.0f;
    return f;
}

int App::preset_index() const
{
    const int count = static_cast<int>(presets().size());
    for (int i = 0; i < count; ++i)
        if (preset(i) == framing_)
            return i;
    return count - 1;
}

// The framing in words, under the picker's preview
std::string App::framing_label() const
{
    std::string name = presets()[static_cast<std::size_t>(preset_index())];
    if (name == "Fit")
        return "Fit: all of it, on a blurred fill";
    if (name == "Whole")
        return "Fill: all of it";
    if (name == "Custom")
    {
        if (framing_.zoom < 1.01f)
            return "Custom: the part you chose";
        char text[64];
        std::snprintf(text, sizeof(text), "Custom: the part you chose, %.1f\xC3\x97", static_cast<double>(framing_.zoom));
        return text;
    }
    for (char &c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return "Fill: the " + name + " of it";
}

void App::move_frame(float du, float dv, float zoom)
{
    constexpr float kMaxZoom = 3.0f;
    const Art *art = focused_art();
    if (art == nullptr || art->w <= 0 || art->h <= 0)
        return;
    Framing f = framing_;
    if (f.fit)
        f = Framing{}; // from Fit: the middle, filled
    const float d = file_aspect();
    const Rect before = cover_uv(art->w, art->h, d, 1.0f, f.pan_x, f.pan_y, f.zoom);
    const float cu = before.x + before.w * (0.5f + du), cv = before.y + before.h * (0.5f + dv);
    f.zoom = std::clamp(f.zoom * zoom, 1.0f, kMaxZoom);
    const Rect after = cover_uv(art->w, art->h, d, 1.0f, 0.0f, 0.0f, f.zoom);
    const float slack_u = 1.0f - after.w, slack_v = 1.0f - after.h;
    f.pan_x = slack_u > 1e-4f ? std::clamp(2.0f * (cu - after.w * 0.5f) / slack_u - 1.0f, -1.0f, 1.0f) : 0.0f;
    f.pan_y = slack_v > 1e-4f ? std::clamp(2.0f * (cv - after.h * 0.5f) / slack_v - 1.0f, -1.0f, 1.0f) : 0.0f;
    if (f == framing_)
        return;
    framing_ = f;
    custom_ = f;
    has_custom_ = true;
    reframed(0.3f);
}

// The frame page: the whole picture, the part kept lit, and what it makes
void App::open_frame()
{
    const Art *art = focused_art();
    if (art == nullptr)
        return;
    screen_ = Screen::frame;
    page_age_ = 0.0f;
    preset_position_.snap(static_cast<float>(preset_index()));
    if (frame_url_ == art->url)
        return;
    forget(frame_picture_);
    frame_url_ = art->url;
    frame_w_ = frame_h_ = 0;
    const std::string url = art->url;
    helper_.ask({"THUMB", url, "1600", "1600"}, [this, url](Reply &reply) {
        if (!reply.ok || frame_url_ != url)
            return;
        upload(std::move(reply.picture), [this, url](std::uint32_t texture, const Picture &p) {
            if (frame_url_ != url)
            {
                if (texture)
                    textures_.destroy(texture);
                return;
            }
            forget(frame_picture_);
            frame_picture_ = texture;
            frame_w_ = p.w;
            frame_h_ = p.h;
        });
    }, true, generation_);
}

void App::update_frame(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    if (focused_art() == nullptr || input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        screen_ = Screen::picker;
        return;
    }
    if (input.is_pressed(Action::confirm))
    {
        feedback.play(Cue::modal_open);
        screen_ = Screen::picker;
        sheet_ = Sheet::confirm;
        if (preview_due_ >= 0.0f)
        {
            preview_due_ = -1.0f;
            request_preview();
        }
        return;
    }
    // L1 R1: the framings offered
    if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
    {
        const int count = static_cast<int>(presets().size()), now = preset_index();
        const int next = std::clamp(now + (input.is_pressed(Action::page_next) ? 1 : -1), 0, count - 1);
        if (next != now)
        {
            framing_ = preset(next);
            reframed(0.0f);
            feedback.play(Cue::tab);
        }
        else
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
        return;
    }
    // the left stick moves the part kept (its own size in a second at full tilt), the D-pad a step at a time;
    // R2 and L2 bring it closer and back
    float du = input.stick_x * dt, dv = input.stick_y * dt;
    if (!input.nav_from_stick)
    {
        constexpr float kStep = 0.06f;
        du += input.nav == Direction::right ? kStep : input.nav == Direction::left ? -kStep : 0.0f;
        dv += input.nav == Direction::down ? kStep : input.nav == Direction::up ? -kStep : 0.0f;
    }
    const float closer = std::max(input.trigger_r, input.is_held(Action::jump_next) ? 1.0f : 0.0f);
    const float back = std::max(input.trigger_l, input.is_held(Action::jump_prev) ? 1.0f : 0.0f);
    const float zoom = std::exp((closer - back) * dt * 0.9f);
    if (std::fabs(du) > 1e-5f || std::fabs(dv) > 1e-5f || std::fabs(zoom - 1.0f) > 1e-5f)
        move_frame(du, dv, zoom);
}

void App::draw_pills(gfx::DrawList &list, const std::vector<std::string> &names, float position) const
{
    if (names.empty())
        return;
    const ui::GlyphStyle dark = ui::GlyphStyle::dark();
    float tx = kMargin + 54;
    ui::draw_button(list, fonts_, dark, ui::Button::l1, kMargin, 316, 30);
    std::vector<Rect> pills;
    for (const std::string &name : names)
    {
        const float w = fonts_.semibold.measure(name, 22) + 44;
        pills.push_back({tx, 294, w, 44});
        tx += w + 10;
    }
    // the highlight springs between the pills
    const int last = static_cast<int>(pills.size()) - 1;
    const int lo = std::clamp(static_cast<int>(std::floor(position)), 0, last);
    const int hi = std::clamp(lo + 1, 0, last);
    const float f = std::clamp(position - lo, 0.0f, 1.0f);
    const Rect highlight{pills[lo].x + (pills[hi].x - pills[lo].x) * f, 294,
                         pills[lo].w + (pills[hi].w - pills[lo].w) * f, 44};
    list.rounded_rect(highlight, 22, kWhite);
    const int active = std::clamp(static_cast<int>(std::lround(position)), 0, last);
    for (int i = 0; i <= last; ++i)
        ui::text(list, fonts_.semibold, names[static_cast<std::size_t>(i)], pills[static_cast<std::size_t>(i)].cx(),
                 pills[static_cast<std::size_t>(i)].y + 30, 22, i == active ? kInk : kWhite.with_alpha(0.75f),
                 gfx::Align::center);
    ui::draw_button(list, fonts_, dark, ui::Button::r1, tx + 6, 316, 30);
}

void App::draw_frame(gfx::DrawList &list) const
{
    const Art *art = focused_art();
    if (picker_title_ < 0 || art == nullptr)
        return;
    const Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
    const bool icon = slot_ == "icon";
    list.push_opacity(tween::stagger(page_age_, 1, 0.06f, 0.4f));

    ui::text(list, fonts_.semibold, icon ? "FRAME THE ICON" : "FRAME THE BACKGROUND", kMargin, 168, 20, title.accent,
             gfx::Align::left, 4.0f);
    ui::text(list, fonts_.display, fonts_.display.font->fit(title.name, 60, 1100), kMargin - 3, 234, 60, kWhite);
    ui::text(list, fonts_.regular, "Choose the part of the picture to keep", kMargin, 276, 23, kWhite.with_alpha(0.7f));
    draw_pills(list, presets(), preset_position_.value);

    // the whole picture; the part kept lit, the rest dimmed
    const Rect area{kGridX, kGridTop, kGridWidth, kGridBottom - kGridTop};
    const bool sharp = frame_picture_ && frame_url_ == art->url;
    const std::uint32_t texture = sharp ? frame_picture_ : art->texture;
    const int tw = sharp ? frame_w_ : art->tw, th = sharp ? frame_h_ : art->th;
    const int pw = art->w > 0 ? art->w : tw, ph = art->h > 0 ? art->h : th;
    const float d = file_aspect();
    if (texture == 0)
    {
        const Rect whole = contain(area, pw, ph);
        list.rounded_rect(whole, 10, kWhite.with_alpha(0.06f));
        draw_spinner(list, whole.cx(), whole.cy(), 22);
    }
    else if (framing_.fit)
    {
        // the file's frame, with the picture whole inside it over a fill of itself
        const Rect file = contain(area, static_cast<int>(d * 1000), 1000);
        const Rect whole = contain(file, pw, ph);
        list.shadow({file.x, file.y + 18, file.w, file.h}, 12, 36, Color::rgb(0x000000, 0.55f));
        list.rounded_rect(file, 12, kNight);
        list.image(texture, file, cover_uv(tw, th, file.w, file.h), kWhite.with_alpha(0.3f), 12);
        list.image(texture, whole, gfx::kFullUv, kWhite, 0);
        list.bordered_rect(file.inset(-2), 14, Color::rgb(0x000000, 0.0f), 3, kWhite);
    }
    else
    {
        Rect whole = contain(area, pw, ph);
        whole.y = area.y; // under the pills, level with the result
        const Rect uv = cover_uv(tw, th, d, 1.0f, framing_.pan_x, framing_.pan_y, framing_.zoom);
        const Rect kept{whole.x + uv.x * whole.w, whole.y + uv.y * whole.h, uv.w * whole.w, uv.h * whole.h};
        list.shadow({whole.x, whole.y + 18, whole.w, whole.h}, 10, 36, Color::rgb(0x000000, 0.55f));
        list.image(texture, whole, gfx::kFullUv, kWhite, 10);
        list.rounded_rect(whole, 10, kNight.with_alpha(0.66f));
        list.image(texture, kept, uv, kWhite, 0);
        // thirds, faint, to place what matters
        for (int i = 1; i < 3; ++i)
        {
            list.rounded_rect({kept.x + kept.w * i / 3.0f - 0.75f, kept.y, 1.5f, kept.h}, 0, kWhite.with_alpha(0.22f));
            list.rounded_rect({kept.x, kept.y + kept.h * i / 3.0f - 0.75f, kept.w, 1.5f}, 0, kWhite.with_alpha(0.22f));
        }
        list.bordered_rect(kept.inset(-2), 4, Color::rgb(0x000000, 0.0f), 3, kWhite);
    }

    // ---- what it makes ----
    const Rect fresh = icon ? Rect{kPanelX + 88, kGridTop - 10, 380, 380} : Rect{kPanelX, kGridTop, kPanelW, kPanelW * 9 / 16};
    ui::text(list, fonts_.semibold, "RESULT", fresh.x, fresh.y - 16, 17, kWhite.with_alpha(0.6f), gfx::Align::left, 3.0f);
    list.shadow({fresh.x, fresh.y + 18, fresh.w, fresh.h}, icon ? 32 : 18, 36, Color::rgb(0x000000, 0.55f));
    const float radius = icon ? 32.0f : 14.0f;
    draw_new_art(list, fresh, radius);
    list.bordered_rect(fresh, radius, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.2f));
    const float left = icon ? kPanelX : fresh.x;
    float y = fresh.y + fresh.h + 44;
    ui::text(list, fonts_.semibold, framing_label(), left, y, 23, kWhite);
    y += 34;
    // how much of the picture is kept, and how far it is enlarged to the file's size
    int file_w = 0, file_h = 0;
    desc_size(icon ? title.icon_desc : title.pic_desc, file_w, file_h);
    if (!icon && file_w == 0)
    {
        // a background Art Swap adds is 3840x2160
        file_w = 3840;
        file_h = 2160;
    }
    char line[160];
    float enlarged = 0.0f;
    if (framing_.fit)
    {
        std::snprintf(line, sizeof(line), "%d\xC3\x97%d, all of it", pw, ph);
        if (file_w > 0 && pw > 0 && ph > 0)
            enlarged = std::min(static_cast<float>(file_w) / pw, static_cast<float>(file_h) / ph);
    }
    else
    {
        const Rect uv = cover_uv(pw, ph, d, 1.0f, framing_.pan_x, framing_.pan_y, framing_.zoom);
        const int kw = static_cast<int>(std::lround(uv.w * pw)), kh = static_cast<int>(std::lround(uv.h * ph));
        std::snprintf(line, sizeof(line), "%d\xC3\x97%d of its %d\xC3\x97%d", kw, kh, pw, ph);
        if (file_w > 0 && kw > 0)
            enlarged = static_cast<float>(file_w) / kw;
    }
    ui::text(list, fonts_.regular, line, left, y, 21, kWhite.with_alpha(0.65f));
    if (enlarged > 1.05f)
    {
        char more[96];
        std::snprintf(more, sizeof(more), "Enlarged %.1f\xC3\x97 to %d\xC3\x97%d%s", static_cast<double>(enlarged),
                      file_w, file_h, enlarged > 2.5f ? ": it will look soft" : "");
        ui::text(list, fonts_.regular, more, left, y + 30, 21, enlarged > 2.5f ? kAmber : kWhite.with_alpha(0.65f));
    }
    list.pop_opacity();
}

// ---------------------------------------------------------------- actions

void App::toast(const std::string &title, const std::string &detail, bool error)
{
    toasts_.push_back({title, detail, error, 0.0f, error ? 8.0f : 6.0f});
    if (toasts_.size() > 3)
        toasts_.erase(toasts_.begin());
}

void App::apply()
{
    const Art *art = focused_art();
    if (art == nullptr || picker_title_ < 0)
        return;
    Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
    sheet_ = Sheet::working;
    if (slot_ == "logo")
    {
        // the page shows it at once: nothing waits for Apply changes
        working_text_ = "Putting the logo on the game's page";
        const std::string id = title.id, name = title.name;
        helper_.ask({"LOGO", id, art->url}, [this, id, name](Reply &reply) {
            sheet_ = Sheet::none;
            if (!reply.ok)
            {
                toast("The logo was not set", reply.error, true);
                return;
            }
            for (auto &t : titles_)
                if (t->id == id)
                    t->logo = "art";
            toast("New logo for " + name,
                  "It shows on the game's page on the home screen now, in place of its name. Restore takes it off "
                  "again.");
            close_picker();
        }, true);
        return;
    }
    working_text_ = slot_ == "icon" ? "Writing the new icon" : "Writing the new background";
    const std::string id = title.id, name = title.name, slot = slot_;
    helper_.ask({"APPLY", id, slot, art->url, framing_.fit ? "fit" : "cover", number(framing_.pan_x),
                 number(framing_.pan_y), number(framing_.zoom)},
                [this, id, name, slot](Reply &reply) {
                    sheet_ = Sheet::none;
                    if (!reply.ok)
                    {
                        toast("That did not work", reply.error, true);
                        return;
                    }
                    bool added = false;
                    for (auto &t : titles_)
                        if (t->id == id && slot == "pic" && !t->has_pic())
                            added = true;
                    if (added)
                    {
                        // a new file in the game's folder: the home screen takes it in when the game is installed
                        toast("Background added to " + name,
                              "Your game had none, so Restore deletes it. Apply changes (R2) installs the game again "
                              "to show it on the home screen.");
                        note_pending(id, true);
                        close_picker();
                        load_titles(false);
                        return;
                    }
                    for (auto &t : titles_)
                        if (t->id == id)
                        {
                            t->kept = true;
                            if (slot == "icon")
                            {
                                t->icon_asked = false;
                                request_icon(*t);
                            }
                            else
                            {
                                t->background_asked = false;
                                request_background(*t);
                            }
                        }
                    toast(std::string(slot == "icon" ? "New icon" : "New background") + " for " + name,
                          shows_after("Your original is kept, so Restore can put it back. ", "it"));
                    note_pending(id);
                    close_picker();
                },
                true);
}

// The end of a notice: how the home screen comes to show what changed (what: "it" or "them")
std::string App::shows_after(const std::string &before, const char *what) const
{
    return before + "Apply changes (R2) to show " + what + " on the home screen.";
}

// ---------------------------------------------------------------- Apply changes

// The titles changed since the home screen last restarted, as the helper keeps them (pending.txt)
void App::load_pending()
{
    helper_.ask({"PENDING"}, [this](Reply &reply) {
        if (!reply.ok)
            return;
        pending_.clear();
        renamed_.clear();
        for (const auto &row : reply.rows())
            if (row.size() >= 2 && row[0] == "P")
                note_pending(row[1], row.size() >= 3 && row[2] == "name");
    }, true);
}

void App::note_pending(const std::string &id, bool name)
{
    if (!is_pending(id))
        pending_.push_back(id);
    if (name && std::find(renamed_.begin(), renamed_.end(), id) == renamed_.end())
        renamed_.push_back(id);
}

bool App::is_pending(const std::string &id) const
{
    return std::find(pending_.begin(), pending_.end(), id) != pending_.end();
}

void App::open_apply()
{
    sheet_ = Sheet::apply;
    applying_ = false;
}

// The helper restarts the home screen once Art Swap has closed, so the person sees it come back (home.c)
void App::start_apply()
{
    applying_ = true;
    helper_.ask({"HOME", std::to_string(static_cast<long>(getpid()))}, [this](Reply &reply) {
        if (sheet_ != Sheet::apply || !applying_)
            return;
        if (!reply.ok)
        {
            applying_ = false;
            sheet_ = Sheet::none;
            toast("The home screen was not restarted", reply.error, true);
            return;
        }
        close_ = true;
    }, true);
}

// How the last restart ended: Art Swap had closed while it ran. Only a failure needs saying
void App::check_last_home()
{
    if (home_checked_ || helper_.state() != Helper::State::ready)
        return;
    home_checked_ = true;
    helper_.ask({"HOMESTATE"}, [this](Reply &reply) {
        for (const auto &row : reply.rows())
        {
            if (row.size() < 5 || row[0] != "home" || row[2] != "1")
                continue;
            if (row[1] == "failed")
                toast("The home screen did not restart", row[4], true);
            helper_.ask({"HOMESTATE", "seen"}, nullptr, true);
        }
    }, true);
}

void App::restore()
{
    Title *title = focused_title();
    if (title == nullptr)
        return;
    sheet_ = Sheet::working;
    working_text_ = title->name_kept || title->logo == "art" ? "Putting the originals back" : "Putting the original art back";
    const std::string id = title->id, name = title->name;
    const bool named = title->name_kept || title->added, art = title->kept;
    helper_.ask({"RESTORE", id, "all"}, [this, id, name, named, art](Reply &reply) {
        sheet_ = Sheet::none;
        if (!reply.ok)
        {
            toast("Nothing was put back", reply.error, true);
            return;
        }
        for (auto &t : titles_)
            if (t->id == id)
            {
                t->kept = false;
                t->name_kept = false;
                t->added = false;
                if (t->logo == "art")
                    t->logo = "-";
                t->icon_asked = false;
                t->background_asked = false;
                request_icon(*t);
                request_background(*t);
            }
        // a logo leaves the page at once; art and names wait for Apply changes
        if (!art && !named)
        {
            toast("The logo is off " + name + "'s page", "Its page shows its name again.");
            return;
        }
        toast(named ? name + "'s originals are back" : "The original art of " + name + " is back",
              shows_after("", "them"));
        note_pending(id, named);
        if (named)
            load_titles(false); // its name, as the helper reads it again, and its art files
    }, true);
}

// ---------------------------------------------------------------- the home-screen refresh

// The home screen keeps its own copy of a game's art; the helper has ShadowMountPlus
// install the game again (helper/src/smp.c). First: can it, for this title?
void App::open_refresh(bool rename)
{
    const Title *title = focused_title();
    if (title == nullptr)
        return;
    sheet_ = Sheet::refresh;
    renaming_ = rename;
    renamed_to_.clear();
    refresh_ = Refresh::asking;
    refresh_id_ = title->id;
    refresh_why_.clear();
    refresh_step_.clear();
    smp_version_.clear();
    smp_folder_ = false;
    refresh_polling_ = false;
    const std::string id = title->id;
    helper_.ask({"SMP", id}, [this, id](Reply &reply) {
        if (sheet_ != Sheet::refresh || refresh_id_ != id || refresh_ != Refresh::asking)
            return;
        if (!reply.ok)
        {
            refresh_ = Refresh::unable;
            refresh_why_ = reply.error;
            return;
        }
        bool can = false;
        for (const auto &row : reply.rows())
        {
            if (row.size() >= 2 && row[0] == "smp")
                smp_version_ = row[1] == "-" ? "" : row[1];
            else if (row.size() >= 2 && row[0] == "game")
                smp_folder_ = row[1] == "folder";
            else if (row.size() >= 2 && row[0] == "can")
            {
                can = row[1] == "1";
                refresh_why_ = row.size() >= 3 ? row[2] : "";
            }
        }
        refresh_ = can ? Refresh::ready : Refresh::unable;
        // Rename: the keyboard over this sheet for the name, then the refresh that shows it
        if (can && renaming_)
        {
            refresh_ = Refresh::naming;
            type_name();
        }
    }, true);
}

void App::type_name()
{
    const Title *title = focused_title();
    if (title == nullptr)
        return;
    const std::string id = title->id;
    if (!open_keyboard("Name on the home screen", title->name, 127, [this, id](bool ok, std::string text) {
            if (sheet_ != Sheet::refresh || refresh_ != Refresh::naming || refresh_id_ != id)
                return;
            const Title *now = focused_title();
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            while (!text.empty() && text.front() == ' ')
                text.erase(text.begin());
            if (ok && now && !text.empty() && text != now->name)
                rename(text);
            else
                sheet_ = Sheet::none; // cancelled, or the same name
        }))
    {
        sheet_ = Sheet::none;
        toast("The keyboard did not open", "", true);
    }
}

// The helper writes the name into the game's param.json; the home screen shows it once the game is
// installed again, so the refresh follows
void App::rename(const std::string &name)
{
    const Title *title = focused_title();
    if (title == nullptr)
        return;
    const std::string id = title->id;
    helper_.ask({"RENAME", id, name}, [this, id, name](Reply &reply) {
        const bool here = sheet_ == Sheet::refresh && refresh_id_ == id;
        if (!reply.ok)
        {
            if (here)
                sheet_ = Sheet::none;
            toast("The name was not changed", reply.error, true);
            return;
        }
        for (auto &t : titles_)
            if (t->id == id)
            {
                t->name = name;
                t->name_kept = true;
            }
        if (here)
            sheet_ = Sheet::none;
        note_pending(id, true);
        toast("Renamed " + name, shows_after("Your original is kept, so Restore can put it back. ", "it"));
    }, true);
}

// The helper notes the game's files and copies its saved data aside, then waits for the console to be free:
// Art Swap closes once that is done, and the helper carries on from the home screen
void App::start_refresh()
{
    refresh_ = Refresh::starting;
    refresh_step_ = "Checking the game's files";
    const std::string id = refresh_id_;
    helper_.ask({"REFRESH", id, "120"}, [this, id](Reply &reply) {
        if (sheet_ != Sheet::refresh || refresh_id_ != id)
            return;
        if (!reply.ok)
        {
            refresh_ = Refresh::unable;
            refresh_why_ = reply.error;
            return;
        }
        refresh_polling_ = true;
        refresh_poll_ = 0.3f;
    }, true);
}

void App::poll_refresh(float dt)
{
    if (!refresh_polling_ || sheet_ != Sheet::refresh || refresh_ != Refresh::starting)
        return;
    refresh_poll_ -= dt;
    if (refresh_poll_ > 0.0f)
        return;
    refresh_poll_ = 1e9f; // until this answer
    const std::string id = refresh_id_;
    helper_.ask({"REFRESHSTATE"}, [this, id](Reply &reply) {
        refresh_poll_ = 0.3f;
        if (sheet_ != Sheet::refresh || refresh_id_ != id || refresh_ != Refresh::starting)
            return;
        for (const auto &row : reply.rows())
            if (row.size() >= 4 && row[0] == "refresh" && row[2] == id)
            {
                const std::string &phase = row[1];
                const std::string message = row.size() >= 5 ? row[4] : "";
                if (phase == "failed")
                {
                    refresh_ = Refresh::unable;
                    refresh_why_ = message;
                    refresh_polling_ = false;
                }
                else if (phase != "copying")
                {
                    // the files are noted: the helper waits for Art Swap to close
                    refresh_step_ = "Closing Art Swap";
                    refresh_polling_ = false;
                    close_ = true;
                }
            }
    }, true);
}

// How the last refresh ended: Art Swap had closed while it ran
void App::check_last_refresh()
{
    if (refresh_checked_ || helper_.state() != Helper::State::ready)
        return;
    refresh_checked_ = true;
    helper_.ask({"REFRESHSTATE"}, [this](Reply &reply) {
        for (const auto &row : reply.rows())
        {
            if (row.size() < 5 || row[0] != "refresh" || row[3] != "1")
                continue;
            if (row[1] == "done")
                toast("Game refreshed", row[4] + ". Its saves and files were checked afterwards.");
            else if (row[1] == "failed")
                toast("The game's refresh did not finish", row[4], true);
            else
                continue;
            helper_.ask({"REFRESHSTATE", "seen"}, nullptr, true);
        }
    }, true);
}

void App::open_search(const std::string &term)
{
    sheet_ = Sheet::search;
    sheet_focus_ = results_.empty() ? 0 : sheet_focus_;
    search_term_ = term;
    searching_ = true;
    search_error_.clear();
    results_.clear();
    sheet_focus_ = 0;
    sheet_focus_position_.snap(0.0f);
    helper_.ask({"SEARCH", term}, [this, term](Reply &reply) {
        if (term != search_term_)
            return;
        searching_ = false;
        if (!reply.ok)
        {
            search_error_ = reply.error;
            return;
        }
        for (const auto &row : reply.rows())
            if (row.size() >= 5 && row[0] == "G")
                results_.push_back(row);
        if (results_.empty())
            search_error_ = "Nothing on SteamGridDB is called \xE2\x80\x9C" + term + "\xE2\x80\x9D";
        else
            sheet_focus_ = 1;
    }, true);
}

void App::choose_game(int row)
{
    if (row < 1 || row > static_cast<int>(results_.size()) || picker_title_ < 0)
        return;
    const auto &game = results_[static_cast<std::size_t>(row - 1)];
    Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
    title.sgdb_id = std::strtol(game[1].c_str(), nullptr, 10);
    title.sgdb_name = game[2];
    helper_.ask({"MATCH", title.id, game[1], game[2]}, nullptr, true);
    sheet_ = Sheet::none;
    set_tab(tab_);
}

void App::type_search()
{
    const Title *title = picker_title_ >= 0 ? titles_[static_cast<std::size_t>(picker_title_)].get() : nullptr;
    if (!open_keyboard("Search SteamGridDB", search_term_.empty() && title ? title->name : search_term_, 80,
                         [this](bool ok, std::string text) {
                             if (ok && !text.empty())
                                 open_search(text);
                         }))
        toast("The keyboard did not open", "", true);
}

void App::type_key()
{
    if (!open_keyboard("SteamGridDB API key", "", 64, [this](bool ok, std::string text) {
            if (!ok || text.empty())
                return;
            sheet_ = Sheet::working;
            working_text_ = "Checking the key with SteamGridDB";
            helper_.ask({"KEY", text}, [this](Reply &reply) {
                sheet_ = Sheet::none;
                if (!reply.ok)
                {
                    toast("The key was not taken", reply.error, true);
                    return;
                }
                key_added("SteamGridDB accepted it.");
            }, true);
        }))
        toast("The keyboard did not open", "", true);
}

// ---------------------------------------------------------------- update

// The console's keyboard, with the pad kept from Art Swap while it is up and until every button the person
// pressed there is let go: R2 is the keyboard's Done, and must not then open Apply changes
bool App::open_keyboard(const std::string &title, const std::string &initial, int max_length,
                        std::function<void(bool, std::string)> done)
{
    if (keyboard_ == nullptr)
        return false;
    const bool opened = keyboard_->open(title, initial, max_length, [this, done](bool ok, std::string text) {
        keyboard_up_ = false;
        swallow_input_ = true;
        done(ok, std::move(text));
    });
    if (opened)
        keyboard_up_ = true;
    return opened;
}

void App::update(const hui::InputFrame &pad, float dt, ui::Feedback &feedback)
{
    // the keyboard's buttons stay the keyboard's
    if (swallow_input_ && !keyboard_up_ && pad.held == 0 && pad.trigger_l < 0.1f && pad.trigger_r < 0.1f)
        swallow_input_ = false;
    hui::InputFrame quiet;
    quiet.connected = pad.connected;
    const hui::InputFrame &input = keyboard_up_ || swallow_input_ ? quiet : pad;
    age_ += dt;
    clock_ += dt;
    for (Toast &t : toasts_)
        t.age += dt;
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const Toast &t) { return t.age > t.life; }),
                  toasts_.end());
    if (key_state_.empty() && helper_.state() == Helper::State::ready)
    {
        key_state_ = helper_.key_source();
        key_hint_ = helper_.key_hint();
    }
    check_last_refresh();
    check_last_home();
    poll_refresh(dt);

    // OPTIONS: a short press is the settings; the program leaves on a hold
    if (input.is_pressed(Action::menu))
        menu_held_ = 0.0f;
    else if (menu_held_ >= 0.0f && input.is_held(Action::menu))
        menu_held_ += dt;
    const bool menu_tap = menu_held_ >= 0.0f && (input.released & hui::action_bit(Action::menu)) != 0 &&
                          menu_held_ < 0.45f;
    if (!input.is_held(Action::menu) && !input.is_pressed(Action::menu))
        menu_held_ = -1.0f;

    page_age_ += dt;
    if (sheet_ != Sheet::none)
        update_sheet(input, feedback);
    else if (screen_ == Screen::starting)
        update_starting(input, feedback);
    else if (screen_ == Screen::library)
        update_library(input, dt, feedback);
    else if (screen_ == Screen::picker)
        update_picker(input, dt, feedback);
    else if (screen_ == Screen::frame)
        update_frame(input, dt, feedback);
    else if (screen_ == Screen::settings)
        update_settings(input, dt, feedback);
    else
        update_connect(input, dt, feedback);
    if (menu_tap && sheet_ == Sheet::none && (screen_ == Screen::library || screen_ == Screen::picker))
    {
        open_settings();
        feedback.play(Cue::open);
    }

    // ---- animation ----
    sheet_open_.target = sheet_ != Sheet::none ? 1.0f : 0.0f;
    sheet_open_.update(dt, 14.0f);
    sheet_focus_position_.target = static_cast<float>(sheet_focus_);
    sheet_focus_position_.update(dt, 22.0f);
    nudge_.update(dt, 9.0f);
    hero_.update(dt);
    backdrop_fade_.update(dt);
    tab_position_.target = static_cast<float>(tab_);
    tab_position_.update(dt, 18.0f);
    if (screen_ == Screen::frame)
    {
        preset_position_.target = static_cast<float>(preset_index());
        preset_position_.update(dt, 18.0f);
    }

    const bool picking = (screen_ == Screen::picker || screen_ == Screen::frame) && picker_title_ >= 0;
    const Title *title = picking
                             ? titles_[static_cast<std::size_t>(picker_title_)].get()
                             : focused_title();
    if (title)
    {
        palette_[0].target(gfx::mix(title->dark, kNight, 0.4f));
        palette_[1].target(gfx::mix(title->dark, title->mid, 0.35f));
        palette_[2].target(title->mid);
        palette_[3].target(gfx::mix(title->mid, title->accent, 0.55f));
    }
    for (ui::SpringColor &colour : palette_)
        colour.update(dt, 4.0f);

    // the background behind the screen follows the focus, cross-faded
    if (Title *focused = picking ? titles_[static_cast<std::size_t>(picker_title_)].get() : focused_title())
    {
        focus_rest_ += dt;
        if (focus_rest_ > 0.12f)
            request_background(*focused);
        if (focused->background != 0 && focused->background != backdrop_)
        {
            backdrop_old_ = backdrop_;
            backdrop_ = focused->background;
            backdrop_fade_.start(0.45f);
        }
        else if (!focused->has_pic() && backdrop_ != 0)
        {
            // none of its own: the last title's fades away, rather than stand in for it
            backdrop_old_ = backdrop_;
            backdrop_ = 0;
            backdrop_fade_.start(0.45f);
        }
        if (focused->background)
            focused->background_used = clock_;
    }

    // shelf and grid motion
    if (!titles_.empty())
    {
        const float start = static_cast<float>(focus_) * (kCard + kCardGap);
        shelf_scroll_.reveal(start, start + kCard * kCardGrow, gfx::kVirtualWidth - kMargin, kMargin * 2.2f);
    }
    shelf_scroll_.update(dt, 12.0f);
    // the selection moves; the cards stay where they are, the selected one growing in its place
    for (std::size_t i = 0; i < grow_.size(); ++i)
    {
        grow_[i].target = static_cast<int>(i) == focus_ ? 1.0f : 0.0f;
        grow_[i].update(dt, 18.0f);
    }
    if (screen_ == Screen::picker)
    {
        const Rect cell = cell_rect(art_focus_);
        const float top = cell.y + grid_scroll_.offset() - kGridTop;
        grid_scroll_.reveal(top, top + cell.h, kGridBottom - kGridTop, 40.0f);
        grid_scroll_.update(dt, 14.0f);
        cell_ring_.target(cell_rect(art_focus_));
        cell_ring_.update(dt, 22.0f);
    }
    if ((screen_ == Screen::picker || screen_ == Screen::frame) && preview_due_ >= 0.0f)
    {
        preview_due_ -= dt;
        if (preview_due_ < 0.0f)
            request_preview();
    }
}

void App::update_starting(const hui::InputFrame &input, ui::Feedback &feedback)
{
    if (helper_.state() == Helper::State::ready && !titles_loading_)
        load_titles(false);
    if (helper_.state() == Helper::State::failed && input.is_pressed(Action::confirm))
    {
        feedback.play(Cue::select);
        helper_.start();
    }
}

void App::update_library(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    const int count = static_cast<int>(titles_.size());
    int next = focus_;
    if (input.nav == Direction::left || input.nav == Direction::right)
        next += input.nav == Direction::right ? 1 : -1;
    if (next != focus_ && count > 0)
    {
        const int clamped = std::clamp(next, 0, count - 1);
        if (clamped == focus_)
        {
            if (!input.nav_repeat)
            {
                feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
                nudge_.trigger();
                nudge_direction_ = next > focus_ ? 1.0f : -1.0f;
            }
        }
        else
        {
            focus_ = clamped;
            focus_rest_ = 0.0f;
            hero_.start(0.35f);
            feedback.play(Cue::focus, 1.0f, ui::pan_for_x(card_rect(focus_).cx()));
        }
    }
    Title *title = focused_title();
    if (title == nullptr)
    {
        if (input.is_pressed(Action::confirm) && !titles_loading_)
            load_titles(true);
        else if (input.is_pressed(Action::jump_next))
        {
            feedback.play(Cue::modal_open);
            open_apply();
        }
        return;
    }
    if (input.is_pressed(Action::confirm))
        want_picker("icon", feedback);
    else if (input.is_pressed(Action::north))
    {
        if (!title->has_pic() && !title->refresh)
        {
            feedback.play(Cue::error);
            toast(title->name + " has no background",
                  "Art Swap adds one only to homebrew ShadowMountPlus installed from a folder: the home screen takes a "
                  "new background in when the game is installed again.",
                  true);
        }
        else
            want_picker("pic", feedback);
    }
    else if (input.is_pressed(Action::page_next))
    {
        if (title->listed)
            want_picker("logo", feedback);
        else
        {
            feedback.play(Cue::error);
            toast(title->name + " has no page for a logo",
                  "The home screen does not list it yet, so there is nowhere to show one.", true);
        }
    }
    else if (input.is_pressed(Action::west))
    {
        if (title->changed())
        {
            sheet_ = Sheet::restore;
            feedback.play(Cue::modal_open);
        }
        else
            feedback.play(Cue::error);
    }
    else if (input.is_pressed(Action::jump_next))
    {
        feedback.play(Cue::modal_open);
        open_apply();
    }
    else if (input.is_pressed(Action::jump_prev))
    {
        if (title->refresh)
        {
            feedback.play(Cue::modal_open);
            open_refresh(true);
        }
        else
        {
            feedback.play(Cue::error);
            toast(title->name + " cannot be renamed",
                  "Art Swap renames homebrew that ShadowMountPlus installed from a folder.", true);
        }
    }
    (void)dt;
}

void App::update_picker(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    const int count = static_cast<int>(arts_.size());
    const int cols = columns();
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        close_picker();
        return;
    }
    const int tab_count = static_cast<int>(tabs().size());
    if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
    {
        const int next = std::clamp(tab_ + (input.is_pressed(Action::page_next) ? 1 : -1), 0, tab_count - 1);
        if (next != tab_)
        {
            feedback.play(Cue::tab);
            set_tab(next);
        }
        else
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
        return;
    }
    if (input.is_pressed(Action::west))
    {
        feedback.play(Cue::modal_open);
        const Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
        open_search(title.name);
        return;
    }
    if (count > 0)
    {
        int next = art_focus_;
        switch (input.nav)
        {
        case Direction::left:
            next -= 1;
            break;
        case Direction::right:
            next += 1;
            break;
        case Direction::up:
            next -= cols;
            break;
        case Direction::down:
            next += cols;
            break;
        default:
            break;
        }
        if (next != art_focus_)
        {
            if (next >= count && input.nav == Direction::down && art_focus_ / cols < (count - 1) / cols)
                next = count - 1; // the last row is short
            if (next >= 0 && next < count)
            {
                art_focus_ = next;
                reset_framing();
                preview_due_ = 0.18f;
                preview_serial_++;
                feedback.play(Cue::focus, 1.0f, ui::pan_for_x(cell_rect(next).cx()));
                request_thumbs();
            }
            else if (!input.nav_repeat)
                feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
        }
        // more from SteamGridDB when the last row is reached
        if (art_focus_ / cols >= (count - 1) / cols && count - store_arts_ < total_ && !listing_)
            load_page();
        const Art *art = focused_art();
        // a logo is written whole: nothing to frame
        const bool framed = slot_ != "logo";
        if (input.is_pressed(Action::north) && art && framed)
        {
            feedback.play(Cue::open);
            open_frame();
            return;
        }
        // the right stick slides the part kept, when the frame cuts the picture
        if (art && framed && !framing_.fit && (std::fabs(input.stick2_x) > 0.05f || std::fabs(input.stick2_y) > 0.05f))
            move_frame(input.stick2_x * dt, input.stick2_y * dt, 1.0f);
        if (input.is_pressed(Action::confirm) && art)
        {
            feedback.play(Cue::modal_open);
            sheet_ = Sheet::confirm;
            // the picker stops while the sheet is up: the big preview of this picture now, not after
            if (preview_due_ >= 0.0f)
            {
                preview_due_ = -1.0f;
                request_preview();
            }
        }
    }
    else if (input.is_pressed(Action::confirm) && !list_error_.empty())
    {
        feedback.play(Cue::select);
        set_tab(tab_);
    }
}

void App::update_sheet(const hui::InputFrame &input, ui::Feedback &feedback)
{
    switch (sheet_)
    {
    case Sheet::working:
        return;
    case Sheet::confirm:
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(Cue::launch);
            feedback.rumble(0.5f, 0.12f);
            apply();
        }
        else if (input.is_pressed(Action::back))
        {
            feedback.play(Cue::modal_close);
            sheet_ = Sheet::none;
        }
        return;
    case Sheet::restore:
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(Cue::select);
            restore();
        }
        else if (input.is_pressed(Action::back))
        {
            feedback.play(Cue::modal_close);
            sheet_ = Sheet::none;
        }
        return;
    case Sheet::log:
        if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm))
        {
            feedback.play(Cue::back);
            sheet_ = Sheet::none;
        }
        return;
    case Sheet::apply:
        if (applying_)
            return;
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(Cue::launch);
            start_apply();
        }
        else if (input.is_pressed(Action::back))
        {
            feedback.play(Cue::modal_close);
            sheet_ = Sheet::none;
        }
        return;
    case Sheet::refresh:
        if (refresh_ == Refresh::starting || refresh_ == Refresh::naming)
            return;
        if (refresh_ == Refresh::ready && input.is_pressed(Action::confirm))
        {
            feedback.play(Cue::launch);
            start_refresh();
        }
        else if (input.is_pressed(Action::back) ||
                 (refresh_ == Refresh::unable && input.is_pressed(Action::confirm)))
        {
            feedback.play(Cue::modal_close);
            sheet_ = Sheet::none;
        }
        return;
    default:
        break;
    }
    // the search results
    const int rows = 1 + static_cast<int>(results_.size());
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = std::clamp(sheet_focus_ + (input.nav == Direction::down ? 1 : -1), 0, rows - 1);
        if (next != sheet_focus_)
        {
            sheet_focus_ = next;
            feedback.play(Cue::focus, 1.0f, 0.2f);
        }
        else if (!input.nav_repeat)
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::modal_close);
        sheet_ = Sheet::none;
        pending_slot_.clear();
        return;
    }
    if (input.is_pressed(Action::confirm))
    {
        feedback.play(Cue::select);
        if (sheet_focus_ == 0)
            type_search();
        else
            choose_game(sheet_focus_);
    }
}

// ---------------------------------------------------------------- layout

Rect App::card_rect(int index) const
{
    auto size = [&](int i) {
        const float g = i >= 0 && i < static_cast<int>(grow_.size()) ? grow_[static_cast<std::size_t>(i)].value
                                                                    : (i == focus_ ? 1.0f : 0.0f);
        return kCard * (1.0f + (kCardGrow - 1.0f) * g);
    };
    float x = kMargin - shelf_scroll_.offset();
    for (int i = 0; i < index; ++i)
        x += size(i) + kCardGap;
    const float s = size(index);
    return {x, kShelfY - (s - kCard), s, s};
}

Rect App::cell_size() const
{
    const Kind &k = kind_for(tab_kinds(), tab_);
    return {0, 0, k.cell_w, k.cell_h};
}

int App::columns() const
{
    const Rect cell = cell_size();
    return std::max(1, static_cast<int>((kGridWidth + kGridGap) / (cell.w + kGridGap)));
}

Rect App::cell_rect(int index) const
{
    const Rect cell = cell_size();
    const int cols = columns();
    const int row = index / cols, col = index % cols;
    return {kGridX + col * (cell.w + kGridGap), kGridTop + row * (cell.h + kGridGap) - grid_scroll_.offset(), cell.w,
            cell.h};
}

// ---------------------------------------------------------------- drawing

void App::draw(hui::app::Frame &frame) const
{
    draw_backdrop(frame);
    gfx::DrawList &list = frame.scene;
    const float back = sheet_open_.value;
    list.push_transform(1.0f - 0.03f * back, 960, 540, 0, 0);
    if (screen_ == Screen::starting)
        draw_starting(list);
    else if (screen_ == Screen::settings)
        draw_settings(list);
    else if (screen_ == Screen::connect)
        draw_connect(list);
    else
    {
        draw_top_bar(list);
        if (screen_ == Screen::library)
            draw_library(list);
        else if (screen_ == Screen::frame)
            draw_frame(list);
        else
            draw_picker(list);
    }
    list.pop_transform();
    if (back > 0.01f)
    {
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, kNight.with_alpha(0.5f * back));
        frame.glass = true;
        // Rename's first steps have no sheet: the screen dims, and the system's keyboard comes up over it
        const bool bare = sheet_ == Sheet::refresh && renaming_ &&
                          (refresh_ == Refresh::asking || refresh_ == Refresh::naming);
        if (!bare)
            draw_sheet(frame.overlay, frame.glass_texture);
        else if (refresh_ == Refresh::asking)
            draw_spinner(frame.overlay, 960, 540, 22);
    }
    draw_hints(back > 0.5f ? frame.overlay : frame.scene);
    draw_toasts(frame.overlay);
}

void App::draw_backdrop(hui::app::Frame &frame) const
{
    if (screen_ == Screen::settings || screen_ == Screen::connect)
    {
        // a quiet dot field in the app's own indigo, whatever game is in focus
        frame.backdrop.mode = gfx::BackdropMode::dots;
        frame.backdrop.colors[0] = Color::rgb(0x090a18);
        frame.backdrop.colors[1] = Color::rgb(0x151536);
        frame.backdrop.colors[2] = Color::rgb(0x5a4fc8);
        frame.backdrop.time = clock_;
        return;
    }
    frame.backdrop.mode = gfx::BackdropMode::aurora;
    for (int i = 0; i < 4; ++i)
        frame.backdrop.colors[i] = palette_[i].value();
    frame.backdrop.time = clock_;

    gfx::DrawList &list = frame.scene;
    const Rect screen{0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight};
    const float t = backdrop_fade_.running ? tween::smoothstep(backdrop_fade_.progress()) : 1.0f;
    const float strength = screen_ == Screen::picker || screen_ == Screen::frame ? 0.45f : 1.0f;
    if (backdrop_old_ && t < 1.0f)
        list.image(backdrop_old_, screen, gfx::kFullUv, kWhite.with_alpha(strength * (1.0f - t)));
    if (backdrop_)
        list.image(backdrop_, screen, gfx::kFullUv, kWhite.with_alpha(strength * t));
    // legibility: dark on the left, where the text is, and under the shelf
    list.gradient_rect_h({0, 0, 1350, 1080}, 0, kNight.with_alpha(0.88f), kNight.with_alpha(0.0f));
    list.gradient_rect({0, 0, 1920, 180}, 0, kNight.with_alpha(0.6f), kNight.with_alpha(0.0f));
    list.gradient_rect({0, 600, 1920, 480}, 0, kNight.with_alpha(0.0f), kNight.with_alpha(0.92f));
}

void App::draw_spinner(gfx::DrawList &list, float cx, float cy, float radius) const
{
    list.ring(cx, cy, radius, radius * 0.16f, kWhite.with_alpha(0.12f));
    list.arc(cx, cy, radius, radius * 0.16f, clock_ * 5.0f, 1.9f, kWhite.with_alpha(0.9f));
}

void App::draw_starting(gfx::DrawList &list) const
{
    const float in = tween::stagger(age_, 0, 0.05f, 0.6f);
    list.push_opacity(in);
    ui::text(list, fonts_.display, "Art Swap", 960, 470, 104, kWhite, gfx::Align::center);
    ui::text(list, fonts_.semibold, "HOME-SCREEN ART FROM STEAMGRIDDB", 960, 530, 20, kWhite.with_alpha(0.6f),
             gfx::Align::center, 5.0f);
    const bool failed = helper_.state() == Helper::State::failed;
    if (!failed)
    {
        draw_spinner(list, 960, 640, 26);
        ui::text(list, fonts_.regular, helper_.status() + "\xE2\x80\xA6", 960, 720, 26, kWhite.with_alpha(0.75f),
                 gfx::Align::center);
    }
    else
    {
        list.rounded_rect({560, 600, 800, 4}, 2, kRed.with_alpha(0.7f));
        ui::paragraph(list, fonts_.regular, helper_.status(), 960, 668, 28, 760, 40, kWhite, 4, gfx::Align::center);
    }
    list.pop_opacity();
}

void App::draw_top_bar(gfx::DrawList &list) const
{
    const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
    list.push_opacity(in);
    const float w = ui::text(list, fonts_.semibold, "Art Swap", kMargin, 92, 28, kWhite);
    list.rounded_rect({kMargin, 104, w, 4}, 2, palette_[3].value());
    std::string right;
    Color dot = Color::rgb(0x30d080);
    const std::string backend = helper_.backend();
    if (backend.rfind("unavailable", 0) == 0)
    {
        right = "No network for the helper";
        dot = kRed;
    }
    else if (key_state_ == "none")
    {
        right = "No SteamGridDB key: press OPTIONS to add yours";
        dot = kAmber;
    }
    else
        right = std::string("SteamGridDB") + (key_state_ == "user" ? " \xC2\xB7 your key" : "");
    const float tw = ui::text(list, fonts_.regular, right, 1824, 92, 24, kWhite.with_alpha(0.75f), gfx::Align::right);
    list.circle(1824 - tw - 20, 84, 7, dot);
    // changes the home screen does not show yet: R2 applies them
    if (!pending_.empty() && screen_ == Screen::library)
    {
        const int n = static_cast<int>(pending_.size());
        const std::string label = "Apply changes  \xC2\xB7  " + std::to_string(n) + (n == 1 ? " game" : " games");
        const float cw = fonts_.semibold.measure(label, 22) + 96;
        const Rect chip{1824 - tw - 60 - cw, 60, cw, 48};
        list.bordered_rect(chip, 24, kAmber.with_alpha(0.14f), 1.5f, kAmber.with_alpha(0.75f));
        ui::draw_button(list, fonts_, ui::GlyphStyle::dark(), ui::Button::r2, chip.x + 16, chip.cy(), 26);
        ui::text(list, fonts_.semibold, label, chip.x + 76, chip.cy() + 8, 22, kWhite);
    }
    list.pop_opacity();
}

void App::draw_library(gfx::DrawList &list) const
{
    const Title *title = focused_title();
    if (title == nullptr)
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        if (titles_loading_)
        {
            draw_spinner(list, 960, 520, 26);
            ui::text(list, fonts_.regular, "Finding your games\xE2\x80\xA6", 960, 600, 28, kWhite.with_alpha(0.8f),
                     gfx::Align::center);
        }
        else
        {
            ui::text(list, fonts_.display, titles_error_.empty() ? "No titles with art found" : "The titles did not load",
                     kMargin, 300, 64, kWhite);
            ui::paragraph(list, fonts_.regular,
                          titles_error_.empty()
                              ? "Art Swap looks in /user/appmeta and in the homebrew folders. Settings, then Helper log, "
                                "shows what it read."
                              : titles_error_,
                          kMargin, 370, 28, 1100, 40, kWhite.with_alpha(0.8f), 4);
        }
        list.pop_opacity();
        return;
    }

    // ---- the hero: what this title is and what can change ----
    const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
    const float arrive = hero_.running ? tween::quint_out(hero_.progress()) : 1.0f;
    list.push_opacity(in * (0.25f + 0.75f * arrive));
    const float x = kMargin + 30.0f * (1.0f - arrive);
    ui::text(list, fonts_.semibold, kind_label(title->kind) + "  \xC2\xB7  " + title->id, x, 212, 20, title->accent,
             gfx::Align::left, 4.0f);
    ui::text(list, fonts_.display, fonts_.display.font->fit(title->name, 84, 1150), x - 4, 306, 84, kWhite);
    std::string facts = "Icon  " + describe(title->icon_desc) + "     Background  " + describe(title->pic_desc);
    if (title->listed)
        facts += "     Logo  " + logo_label(title->logo);
    ui::text(list, fonts_.regular, fonts_.regular.font->fit(facts, 25, 1240), x, 362, 25, kWhite.with_alpha(0.78f));
    float y = 412;
    if (title->changed())
    {
        const bool art = title->kept || title->logo == "art";
        const std::string chip = art && title->name_kept ? "Custom art and name  \xC2\xB7  originals kept"
                                 : art                   ? "Custom art  \xC2\xB7  original kept"
                                                         : "New name  \xC2\xB7  original kept";
        const float cw = fonts_.semibold.measure(chip, 21) + 44;
        list.bordered_rect({x, y - 30, cw, 44}, 22, title->accent.with_alpha(0.14f), 1.5f,
                           title->accent.with_alpha(0.7f));
        ui::text(list, fonts_.semibold, chip, x + 22, y, 21, kWhite);
        y += 52;
    }
    if (!title->sgdb_name.empty())
    {
        ui::text(list, fonts_.regular, "SteamGridDB: " + title->sgdb_name, x, y, 22, kWhite.with_alpha(0.55f));
    }

    // the actions, as the console draws its Play button
    const ui::GlyphStyle light = ui::GlyphStyle::light();
    const ui::GlyphStyle dark = ui::GlyphStyle::dark();
    float bx = x;
    const float by = 500;
    {
        const char *label = "Change icon";
        const float w = fonts_.semibold.measure(label, 27) + 100;
        const Rect pill{bx, by, w, 64};
        list.glow(pill, 32, 18, title->accent.with_alpha(0.35f));
        list.rounded_rect(pill, 32, kWhite);
        ui::draw_button(list, fonts_, light, ui::Button::cross, pill.x + 20, pill.cy(), 34);
        ui::text(list, fonts_.semibold, label, pill.x + 70, pill.cy() + 10, 27, kInk);
        bx += w + 20;
    }
    {
        const bool has_pic = title->has_pic() || title->refresh;
        const char *label = title->has_pic() || !title->refresh ? "Change background" : "Add background";
        const float w = fonts_.semibold.measure(label, 27) + 100;
        const Rect pill{bx, by, w, 64};
        list.bordered_rect(pill, 32, kWhite.with_alpha(0.1f), 2, kWhite.with_alpha(has_pic ? 0.35f : 0.12f));
        list.push_opacity(has_pic ? 1.0f : 0.4f);
        ui::draw_button(list, fonts_, dark, ui::Button::triangle, pill.x + 20, pill.cy(), 34);
        ui::text(list, fonts_.semibold, label, pill.x + 70, pill.cy() + 10, 27, kWhite);
        list.pop_opacity();
        bx += w + 20;
    }
    if (title->listed)
    {
        // the logo on the game's page, where its name shows without one; a shoulder glyph is wider
        const char *label = "Change logo";
        const float w = fonts_.semibold.measure(label, 27) + 112;
        const Rect pill{bx, by, w, 64};
        list.bordered_rect(pill, 32, kWhite.with_alpha(0.1f), 2, kWhite.with_alpha(0.35f));
        ui::draw_button(list, fonts_, dark, ui::Button::r1, pill.x + 20, pill.cy(), 34);
        ui::text(list, fonts_.semibold, label, pill.x + 82, pill.cy() + 10, 27, kWhite);
        bx += w + 20;
    }
    // the rest, smaller, on a row of their own; a shoulder button's glyph is wider than a face button's
    struct Secondary
    {
        ui::Button button;
        const char *label;
    };
    std::vector<Secondary> more;
    if (title->changed())
        more.push_back({ui::Button::square, "Restore"});
    if (title->refresh)
        more.push_back({ui::Button::l2, "Rename"});
    bx = x;
    for (const Secondary &item : more)
    {
        const bool face = item.button == ui::Button::square;
        const float w = fonts_.semibold.measure(item.label, 22) + (face ? 80 : 92);
        const Rect pill{bx, by + 82, w, 52};
        list.bordered_rect(pill, 26, kWhite.with_alpha(0.08f), 1.5f, kWhite.with_alpha(0.28f));
        ui::draw_button(list, fonts_, dark, item.button, pill.x + 16, pill.cy(), 28);
        ui::text(list, fonts_.semibold, item.label, pill.x + (face ? 56 : 68), pill.cy() + 8, 22,
                 kWhite.with_alpha(0.9f));
        bx += w + 14;
    }
    // a change of this game the home screen does not show yet
    if (is_pending(title->id))
    {
        const float left = more.empty() ? x : bx + 10;
        const float ny = by + 82 + 26;
        list.circle(left + 8, ny, 6, kAmber);
        ui::text(list, fonts_.regular,
                 fonts_.regular.font->fit("Not on the home screen yet: Apply changes (R2)", 22, 1340 - left - 26),
                 left + 26, ny + 8, 22, kWhite.with_alpha(0.75f));
    }

    // the icon, large, floating
    const float bob = std::sin(clock_ * 0.8f) * 6.0f;
    const Rect art{1376 + 40.0f * (1.0f - arrive), 150 + bob, 400, 400};
    list.glow(art.inset(30), 60, 90, title->accent.with_alpha(0.3f));
    list.shadow({art.x, art.y + 26, art.w, art.h}, 36, 46, Color::rgb(0x000000, 0.55f));
    if (title->icon)
        list.image(title->icon, art, gfx::kFullUv, kWhite, 36);
    else
    {
        list.rounded_rect(art, 36, title->mid.with_alpha(0.6f));
        if (!title->icon_failed)
            draw_spinner(list, art.cx(), art.cy(), 24);
    }
    list.bordered_rect(art, 36, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.16f));
    list.pop_opacity();

    // ---- the shelf ----
    const float shelf_in = tween::stagger(age_, 3, 0.09f, 0.6f);
    list.push_opacity(shelf_in);
    char heading[64];
    std::snprintf(heading, sizeof(heading), "Your games  \xC2\xB7  %d", static_cast<int>(titles_.size()));
    ui::text(list, fonts_.semibold, heading, kMargin, kShelfY - 70, 24, kWhite.with_alpha(0.9f));
    const float lift = 40.0f * (1.0f - shelf_in);
    for (int i = 0; i < static_cast<int>(titles_.size()); ++i)
    {
        if (i == focus_)
            continue;
        Rect rect = card_rect(i);
        rect.y += lift;
        if (rect.x > gfx::kVirtualWidth || rect.x + rect.w < -80.0f)
            continue;
        const Title &t = *titles_[static_cast<std::size_t>(i)];
        if (t.icon)
            list.image(t.icon, rect, gfx::kFullUv, kWhite.with_alpha(0.8f), 22);
        else
        {
            list.rounded_rect(rect, 22, t.mid.with_alpha(0.5f));
            ui::text(list, fonts_.semibold, fonts_.semibold.font->fit(t.name, 20, rect.w - 24), rect.x + 12,
                     rect.y + rect.h - 16, 20, kWhite.with_alpha(0.8f));
        }
        if (t.kept || t.logo == "art")
            list.circle(rect.x + rect.w - 18, rect.y + 18, 7, t.accent);
    }
    // the selected card, in its own place: lifted, lit and ringed as it grows
    Rect card = card_rect(focus_);
    card.y += lift;
    card.x += ui::shake(nudge_.value, clock_, 16.0f, 8.0f) * nudge_direction_;
    const float grown = grow_.empty() ? 1.0f : grow_[static_cast<std::size_t>(focus_)].value;
    list.shadow({card.x, card.y + 18, card.w, card.h}, 26, 34, Color::rgb(0x000000, 0.6f * grown));
    list.glow(card, 26, 26, title->accent.with_alpha((0.4f + 0.15f * ui::breathe(clock_)) * grown));
    if (title->icon)
        list.image(title->icon, card, gfx::kFullUv, kWhite, 26);
    else
        list.rounded_rect(card, 26, title->mid);
    list.bordered_rect(card.inset(-5), 30, Color::rgb(0x000000, 0.0f), 4, kWhite.with_alpha(tween::clamp01(grown * 1.5f)));
    if (title->kept || title->logo == "art")
        list.circle(card.x + card.w - 18, card.y + 18, 7, title->accent);
    list.pop_opacity();
}

void App::draw_picker(gfx::DrawList &list) const
{
    if (picker_title_ < 0)
        return;
    const Title &title = *titles_[static_cast<std::size_t>(picker_title_)];
    const bool icon = slot_ == "icon", logo = slot_ == "logo";
    const float in = tween::stagger(age_, 1, 0.06f, 0.5f);
    list.push_opacity(in);

    ui::text(list, fonts_.semibold, icon ? "CHANGE THE ICON" : logo ? "CHANGE THE LOGO" : "CHANGE THE BACKGROUND",
             kMargin, 168, 20, title.accent, gfx::Align::left, 4.0f);
    ui::text(list, fonts_.display, fonts_.display.font->fit(title.name, 60, 1100), kMargin - 3, 234, 60, kWhite);
    std::string game;
    if (matching_)
        game = "Finding " + title.name + " on SteamGridDB\xE2\x80\xA6";
    else if (!title.sgdb_name.empty())
        game = "SteamGridDB: " + title.sgdb_name;
    const float gw = ui::text(list, fonts_.regular, game, kMargin, 276, 23, kWhite.with_alpha(0.7f));
    if (!game.empty())
    {
        const ui::GlyphStyle dark = ui::GlyphStyle::dark();
        ui::draw_button(list, fonts_, dark, ui::Button::square, kMargin + gw + 22, 268, 28);
        ui::text(list, fonts_.regular, "Another game", kMargin + gw + 60, 276, 21, kWhite.with_alpha(0.55f));
    }

    // the tabs
    const ui::GlyphStyle dark = ui::GlyphStyle::dark();
    const auto names = tabs();
    draw_pills(list, std::vector<std::string>(names.begin(), names.end()), tab_position_.value);

    // ---- the grid ----
    list.push_clip({0, kGridTop - 24, kPanelX - 30, kGridBottom - kGridTop + 48});
    // the glow is the chosen picture's, once the ring has reached it: one that travelled with the ring
    // lit the gaps, and the pictures it passed under, in the game's colour
    if (!arts_.empty())
    {
        const Rect ring = cell_ring_.value(), home = cell_rect(art_focus_);
        const float off = std::fabs(ring.x - home.x) + std::fabs(ring.y - home.y);
        const float settled = 1.0f - tween::clamp01(off / 24.0f);
        if (settled > 0.01f)
            list.glow(home, 16, 22, title.accent.with_alpha(0.45f * settled));
    }
    for (int i = 0; i < static_cast<int>(arts_.size()); ++i)
    {
        const Rect r = cell_rect(i);
        if (r.y > kGridBottom + 24 || r.y + r.h < kGridTop - 24)
            continue;
        const Art &art = *arts_[static_cast<std::size_t>(i)];
        const bool focused = i == art_focus_;
        // dimmed, not see-through: a dark card under each picture
        if (art.texture)
            list.rounded_rect(r, 14, logo ? gfx::mix(kNight, title.dark, 0.5f) : kNight);
        if (art.texture && logo)
            list.image(art.texture, contain(r.inset(18), art.tw, art.th), gfx::kFullUv,
                       kWhite.with_alpha(focused ? 1.0f : 0.82f), 0);
        else if (art.texture)
            list.image(art.texture, r, cover_uv(art.tw, art.th, r.w, r.h, 0.0f),
                       kWhite.with_alpha(focused ? 1.0f : 0.82f), 14);
        else
        {
            list.rounded_rect(r, 14, kWhite.with_alpha(0.06f + 0.04f * ui::breathe(clock_ + i * 0.2f)));
            if (art.failed)
                ui::text(list, fonts_.regular, "no picture", r.cx(), r.cy() + 8, 18, kWhite.with_alpha(0.4f),
                         gfx::Align::center);
        }
        // a background big enough for the console's 3840-wide one
        if (!icon && !logo && art.w >= 3840)
        {
            const Rect tag{r.x + r.w - 54, r.y + 10, 44, 26};
            list.rounded_rect(tag, 13, kNight.with_alpha(0.78f));
            ui::text(list, fonts_.semibold, "4K", tag.cx(), tag.y + 19, 15, kWhite, gfx::Align::center);
        }
        // the game's own, from PlayStation Store
        if (art.store)
        {
            const float tw = fonts_.semibold.measure("PLAYSTATION STORE", 14) + 24;
            const Rect tag{r.x + 10, r.y + 10, tw, 26};
            list.rounded_rect(tag, 13, Color::rgb(0x0070d1, 0.9f));
            ui::text(list, fonts_.semibold, "PLAYSTATION STORE", tag.cx(), tag.y + 18, 14, kWhite, gfx::Align::center,
                     1.0f);
        }
    }
    if (!arts_.empty())
        list.bordered_rect(cell_ring_.value().inset(-5), 18, Color::rgb(0x000000, 0.0f), 4, kWhite);
    list.pop_clip();

    // the grid's own messages
    if (arts_.empty())
    {
        if (listing_ || matching_)
        {
            draw_spinner(list, kGridX + 40, kGridTop + 60, 22);
            ui::text(list, fonts_.regular, "Asking SteamGridDB\xE2\x80\xA6", kGridX + 84, kGridTop + 70, 26,
                     kWhite.with_alpha(0.8f));
        }
        else if (!list_error_.empty())
        {
            ui::paragraph(list, fonts_.regular, list_error_, kGridX, kGridTop + 60, 27, 1050, 40, kWhite, 4);
            ui::draw_button(list, fonts_, ui::GlyphStyle::dark(), ui::Button::cross, kGridX, kGridTop + 222, 30);
            ui::text(list, fonts_.regular, "Try again", kGridX + 42, kGridTop + 230, 22, kWhite.with_alpha(0.7f));
        }
        else
        {
            const std::string what = icon ? "icons" : logo ? "logos" : "backgrounds";
            ui::paragraph(list, fonts_.regular,
                          "SteamGridDB has no " + what + " for this game yet. If it is listed under another "
                              "name, choose Another game.",
                          kGridX, kGridTop + 60, 27, 1050, 40, kWhite.with_alpha(0.85f), 4);
        }
    }
    else if (listing_)
        draw_spinner(list, kPanelX - 70, kGridBottom - 10, 16);

    // ---- the preview: what would be written, beside what is there ----
    const Art *art = focused_art();
    if (logo)
    {
        // as the game's page shows it, then what it has now
        const Rect page{kPanelX, kGridTop, kPanelW, kPanelW * 9 / 16};
        ui::text(list, fonts_.semibold, "NEW", page.x, page.y - 16, 17, kWhite.with_alpha(0.6f), gfx::Align::left, 3.0f);
        list.shadow({page.x, page.y + 18, page.w, page.h}, 18, 36, Color::rgb(0x000000, 0.55f));
        draw_logo_page(list, page, art ? art->texture : 0, art ? art->tw : 0, art ? art->th : 0, 14.0f);
        list.bordered_rect(page, 14, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.2f));
        float ly = page.y + page.h + 36;
        if (art)
        {
            char line[160];
            std::snprintf(line, sizeof(line), "%d\xC3\x97%d  \xC2\xB7  %s", art->w, art->h, art->style.c_str());
            ui::text(list, fonts_.regular, line, page.x, ly, 22, kWhite.with_alpha(0.8f));
            ui::text(list, fonts_.regular,
                     art->store ? std::string("PlayStation Store: the game's own")
                                : "by " + fonts_.regular.font->fit(art->author, 20, kPanelW - 40),
                     page.x, ly + 28, 20, kWhite.with_alpha(0.55f));
            ly += 64;
        }
        const Rect now{page.x, ly, 192, 108};
        draw_logo_page(list, now, current_art_, current_w_, current_h_, 8.0f);
        ui::text(list, fonts_.semibold, "NOW", now.x + now.w + 22, now.y + 28, 17, kWhite.with_alpha(0.6f),
                 gfx::Align::left, 3.0f);
        ui::text(list, fonts_.regular,
                 title.logo == "-" ? std::string("No logo: its name shows")
                                   : title.logo == "art" ? std::string("A logo from Art Swap") : std::string("Its own logo"),
                 now.x + now.w + 22, now.y + 58, 20, kWhite.with_alpha(0.7f));
        list.pop_opacity();
        return;
    }
    const Rect fresh = icon ? Rect{kPanelX + 88, kGridTop - 10, 380, 380} : Rect{kPanelX, kGridTop, kPanelW, kPanelW * 9 / 16};
    ui::text(list, fonts_.semibold, "NEW", fresh.x, fresh.y - 16, 17, kWhite.with_alpha(0.6f), gfx::Align::left, 3.0f);
    list.shadow({fresh.x, fresh.y + 18, fresh.w, fresh.h}, icon ? 32 : 18, 36, Color::rgb(0x000000, 0.55f));
    const float radius = icon ? 32.0f : 14.0f;
    draw_new_art(list, fresh, radius);
    list.bordered_rect(fresh, radius, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.2f));

    const float left = icon ? kPanelX : fresh.x;
    float y = fresh.y + fresh.h + 36;
    if (art)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "%d\xC3\x97%d  \xC2\xB7  %s", art->w, art->h, art->style.c_str());
        ui::text(list, fonts_.regular, line, left, y, 22, kWhite.with_alpha(0.8f));
        ui::text(list, fonts_.regular, "by " + fonts_.regular.font->fit(art->author, 20, kPanelW - 40), left, y + 28,
                 20, kWhite.with_alpha(0.55f));
        // the framing, changed on the frame page
        ui::draw_button(list, fonts_, dark, ui::Button::triangle, left, y + 62, 28);
        ui::text(list, fonts_.semibold, framing_label(), left + 40, y + 70, 21, kWhite.with_alpha(0.9f));
        y += 96;
    }
    // now
    const Rect now = icon ? Rect{left, y, 96, 96} : Rect{left, y, 192, 108};
    if (current_art_)
        list.image(current_art_, now, gfx::kFullUv, kWhite.with_alpha(0.9f), icon ? 16.0f : 8.0f);
    else
        list.rounded_rect(now, icon ? 16 : 8, kWhite.with_alpha(0.06f));
    ui::text(list, fonts_.semibold, "NOW", now.x + now.w + 22, now.y + 28, 17, kWhite.with_alpha(0.6f),
             gfx::Align::left, 3.0f);
    ui::text(list, fonts_.regular, describe(icon ? title.icon_desc : title.pic_desc), now.x + now.w + 22, now.y + 58,
             20, kWhite.with_alpha(0.7f));
    if (title.kept)
        ui::text(list, fonts_.regular, "original kept", now.x + now.w + 22, now.y + 86, 20,
                 title.accent.with_alpha(0.9f));
    list.pop_opacity();
}

// The game's page on the home screen, small: its background, darkened on the left as the console darkens it,
// and the logo where the console puts it, at the left, a little below the middle
void App::draw_logo_page(gfx::DrawList &list, const Rect &r, std::uint32_t logo, int lw, int lh, float radius) const
{
    const Title *title = picker_title_ >= 0 ? titles_[static_cast<std::size_t>(picker_title_)].get() : focused_title();
    list.rounded_rect(r, radius, kNight);
    if (title && title->background)
        list.image(title->background, r, cover_uv(title->background_w, title->background_h, r.w, r.h), kWhite, radius);
    list.gradient_rect_h(r, radius, kNight.with_alpha(0.75f), kNight.with_alpha(0.0f));
    const float s = r.w / 1920.0f;
    const Rect box{r.x + 172 * s, r.y + 590 * s, 620 * s, 200 * s};
    if (logo)
    {
        const Rect fitted = contain(box, lw, lh);
        list.image(logo, {box.x, fitted.y, fitted.w, fitted.h}, gfx::kFullUv, kWhite, 0);
    }
    else if (title && r.w > 300)
        ui::text(list, fonts_.semibold, fonts_.semibold.font->fit(title->name, 54 * s * 1.6f, box.w), box.x,
                 box.y + box.h * 0.6f, 54 * s * 1.6f, kWhite);
    // the console's Play button under it
    if (r.w > 300)
        list.rounded_rect({box.x, r.y + 860 * s, 360 * s, 84 * s}, 42 * s, kWhite.with_alpha(0.16f));
}

// What the chosen picture would make: the helper's preview of it, or, until that arrives, its
// thumbnail framed the same way. Never the preview of the picture chosen before.
void App::draw_new_art(gfx::DrawList &list, const Rect &r, float radius) const
{
    const Art *art = focused_art();
    const bool current = art && preview_ && preview_url_ == art->url && preview_framing_ == framing_;
    // the frame page's sharper copy of the picture, when it has it
    const bool sharp = art && frame_picture_ && frame_url_ == art->url;
    const std::uint32_t texture = sharp ? frame_picture_ : art ? art->texture : 0;
    const int tw = sharp ? frame_w_ : art ? art->tw : 0, th = sharp ? frame_h_ : art ? art->th : 0;
    if (current)
        list.image(preview_, r, gfx::kFullUv, kWhite, radius);
    else if (texture)
    {
        list.rounded_rect(r, radius, kNight);
        if (framing_.fit)
            list.image(texture, contain(r, tw, th), gfx::kFullUv, kWhite, radius * 0.5f);
        else
            list.image(texture, r, cover_uv(tw, th, r.w, r.h, framing_.pan_x, framing_.pan_y, framing_.zoom), kWhite,
                       radius);
    }
    else
        list.rounded_rect(r, radius, kWhite.with_alpha(0.06f));
    if (art && (preview_loading_ || preview_due_ >= 0.0f))
        draw_spinner(list, r.x + r.w - 30, r.y + 30, 12);
}

void App::draw_sheet(gfx::DrawList &list, std::uint32_t glass) const
{
    const float t = sheet_open_.value;
    const bool tall = sheet_ == Sheet::search || sheet_ == Sheet::log || sheet_ == Sheet::apply ||
                      (sheet_ == Sheet::refresh && (refresh_ == Refresh::ready || refresh_ == Refresh::starting));
    const float h = tall ? 760.0f : 470.0f;
    const Rect panel{360, (1080 - h) * 0.5f + 40.0f * (1.0f - t), 1200, h};
    list.push_opacity(tween::clamp01(t * 1.4f));
    list.shadow({panel.x, panel.y + 20, panel.w, panel.h}, 44, 60, Color::rgb(0x000000, 0.5f));
    list.glass(glass, panel, 28, kWhite);
    list.gradient_rect(panel, 28, Color::rgb(0x191a36, 0.9f), Color::rgb(0x0f1024, 0.94f));
    list.bordered_rect(panel, 28, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.1f));
    const float x = panel.x + 64;
    float y = panel.y + 92;
    const Title *title = (screen_ == Screen::picker || screen_ == Screen::frame) && picker_title_ >= 0
                             ? titles_[static_cast<std::size_t>(picker_title_)].get()
                             : focused_title();
    const std::string name = title ? title->name : "";
    const Color accent = title ? title->accent : Color::rgb(0x9fd8ff);

    auto heading = [&](const std::string &label, const std::string &text) {
        ui::text(list, fonts_.semibold, label, x, y, 19, accent, gfx::Align::left, 4.0f);
        ui::text(list, fonts_.display, fonts_.display.font->fit(text, 48, panel.w - 128), x - 2, y + 62, 48, kWhite);
        y += 120;
    };
    auto rows = [&](const std::vector<std::pair<std::string, std::string>> &items, int focus, float position) {
        const float row_h = 64, width = panel.w - 128;
        list.rounded_rect({x - 20, y + position * row_h, width + 40, row_h - 8}, 28, kWhite);
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
        {
            const bool f = i == focus;
            const float ry = y + i * row_h;
            ui::text(list, fonts_.semibold, fonts_.semibold.font->fit(items[i].first, 26, width * 0.62f), x + 8,
                     ry + 39, 26, f ? kInk : kWhite.with_alpha(0.9f));
            ui::text(list, fonts_.regular, fonts_.regular.font->fit(items[i].second, 22, width * 0.36f), x + width - 8,
                     ry + 39, 22, f ? kInk.with_alpha(0.7f) : kWhite.with_alpha(0.6f), gfx::Align::right);
        }
        y += row_h * items.size();
    };

    switch (sheet_)
    {
    case Sheet::confirm:
    case Sheet::working:
    {
        const bool icon = slot_ == "icon";
        if (slot_ == "logo" && (screen_ == Screen::picker || sheet_ == Sheet::confirm))
        {
            heading("NEW LOGO", "For " + name + "?");
            const Art *art = focused_art();
            const Rect pic{panel.x + panel.w - 64 - 400, panel.y + 170, 400, 225};
            draw_logo_page(list, pic, art ? art->texture : 0, art ? art->tw : 0, art ? art->th : 0, 10.0f);
            ui::paragraph(list, fonts_.regular,
                          "Art Swap puts it on the game's page on the home screen, where its name shows in plain "
                          "text now. It shows straight away, and stays when the game is renamed. Restore takes it "
                          "off again.",
                          x, y, 25, panel.w - 128 - pic.w - 40, 38, kWhite.with_alpha(0.85f), 7);
            if (sheet_ == Sheet::working)
            {
                draw_spinner(list, x + 22, panel.y + panel.h - 70, 20);
                ui::text(list, fonts_.semibold, working_text_ + "\xE2\x80\xA6", x + 64, panel.y + panel.h - 60, 26,
                         kWhite);
            }
            break;
        }
        heading(icon ? "NEW ICON" : "NEW BACKGROUND", "For " + name + "?");
        const Rect pic = icon ? Rect{panel.x + panel.w - 64 - 260, panel.y + 150, 260, 260}
                              : Rect{panel.x + panel.w - 64 - 400, panel.y + 170, 400, 225};
        draw_new_art(list, pic, icon ? 26.0f : 10.0f);
        const std::string file = icon ? "icon0" : "pic0 and pic1";
        const bool adding = !icon && title && !title->has_pic();
        ui::paragraph(list, fonts_.regular,
                      adding ? name + " has no background yet. Art Swap adds this one to the game's folder, and Apply "
                                      "changes installs the game again so the home screen takes it in. Restore "
                                      "deletes it."
                             : "Art Swap writes it over " + file +
                                   " in every copy of the game's art, in the same format and size. The original is "
                                   "kept, so Restore can always put it back.",
                      x, y, 25, panel.w - 128 - pic.w - 40, 38, kWhite.with_alpha(0.85f), 6);
        if (sheet_ == Sheet::working)
        {
            draw_spinner(list, x + 22, panel.y + panel.h - 70, 20);
            ui::text(list, fonts_.semibold, working_text_ + "\xE2\x80\xA6", x + 64, panel.y + panel.h - 60, 26, kWhite);
        }
        break;
    }
    case Sheet::restore:
    {
        heading("RESTORE", "The original art of " + name + "?");
        // what goes back: the art, the name, and the logo comes off
        std::string what;
        if (title && title->kept && !(title->added && !title->name_kept))
            what = title->name_kept ? "the icon, background and name" : "the icon and background";
        else if (title && title->name_kept)
            what = "the name";
        std::string text = what.empty() ? "" : "Art Swap puts back " + what + " this game came with.";
        if (title && title->added)
            text += std::string(text.empty() ? "Art Swap" : " It also") +
                    " deletes the background it added, which the game did not have.";
        if (title && title->logo == "art")
            text += std::string(text.empty() ? "Art Swap" : " It also") +
                    " takes its logo off its page, which shows its name again.";
        ui::paragraph(list, fonts_.regular, text, x, y, 26, panel.w - 128, 40, kWhite.with_alpha(0.85f), 4);
        break;
    }
    case Sheet::apply:
    {
        const int changed = static_cast<int>(pending_.size());
        heading("APPLY CHANGES", changed == 0   ? std::string("Show your changes")
                                 : changed == 1 ? std::string("1 game changed")
                                                : std::to_string(changed) + " games changed");
        const float width = panel.w - 128;
        const Color tint = Color::rgb(0x9fd8ff);
        y = ui::paragraph(list, fonts_.regular,
                          "The home screen keeps its own copy of each game's art, so it restarts to show new art: "
                          "every change shows at once, whatever installed the game.",
                          x, y, 25, width, 38, kWhite.with_alpha(0.85f), 3) +
            16;
        std::string renamed;
        for (const auto &id : renamed_)
            for (const auto &t : titles_)
                if (t && t->id == id)
                    renamed += (renamed.empty() ? "" : ", ") + t->name;
        const std::string steps[] = {
            "Art Swap closes.",
            renamed.empty() ? std::string("The home screen goes dark for a few seconds.")
                            : "ShadowMountPlus installs " + renamed + " again, for its new name or background.",
            renamed.empty() ? std::string("It comes back with your new art.")
                            : std::string("The home screen restarts with your new art and names.")};
        for (int i = 0; i < 3; ++i)
        {
            list.bordered_rect({x, y - 27, 36, 36}, 18, tint.with_alpha(0.16f), 1.5f, tint.with_alpha(0.8f));
            ui::text(list, fonts_.semibold, std::to_string(i + 1), x + 18, y - 2, 20, kWhite, gfx::Align::center);
            ui::text(list, fonts_.regular, fonts_.regular.font->fit(steps[i], 25, width - 56), x + 56, y, 25, kWhite);
            y += 52;
        }
        y += 14;
        const std::string kept =
            renamed_.empty()
                ? "Nothing else restarts: a game open in the background, your saved data and tools such as "
                  "ShadowMountPlus carry on."
                : "Nothing else restarts, and nothing is deleted: a renamed game's folder, with its saves, stays "
                  "where it is, and Art Swap checks every file of it afterwards.";
        const float kept_lines =
            static_cast<float>(std::min<std::size_t>(fonts_.regular.font->wrap(kept, 22, width - 40).size(), 3));
        const Rect safe{x - 4, y - 8, width + 8, 78 + 32 * (kept_lines - 1) + 38};
        list.bordered_rect(safe, 18, Color::rgb(0x3ddc84, 0.08f), 1.5f, Color::rgb(0x3ddc84, 0.45f));
        ui::text(list, fonts_.semibold, renamed_.empty() ? "ONLY THE HOME SCREEN RESTARTS" : "YOUR SAVED DATA STAYS",
                 safe.x + 24, safe.y + 40, 17,
                 Color::rgb(0x3ddc84), gfx::Align::left, 3.0f);
        ui::paragraph(list, fonts_.regular, kept, safe.x + 24, safe.y + 78, 22, safe.w - 48, 32,
                      kWhite.with_alpha(0.85f), 3);
        y = safe.y + safe.h + 48;
        if (applying_)
        {
            draw_spinner(list, x + 18, y - 9, 16);
            ui::text(list, fonts_.semibold, "Closing Art Swap\xE2\x80\xA6", x + 50, y, 25, kWhite);
        }
        break;
    }
    case Sheet::refresh:
    {
        heading(renaming_ ? "RENAME" : "REFRESH GAME", name);
        const float width = panel.w - 128;
        // a requirement and whether it is met
        auto need = [&](const std::string &label, bool met, const std::string &state) {
            list.circle(x + 8, y - 8, 7, met ? Color::rgb(0x3ddc84) : kRed);
            ui::text(list, fonts_.regular, label, x + 30, y, 23, kWhite.with_alpha(0.85f));
            ui::text(list, fonts_.regular, state, x + width, y, 23, kWhite.with_alpha(0.55f), gfx::Align::right);
            y += 40;
        };
        if (refresh_ == Refresh::asking)
        {
            draw_spinner(list, x + 22, y + 10, 18);
            ui::text(list, fonts_.regular, "Asking ShadowMountPlus about " + name + "\xE2\x80\xA6", x + 60, y + 20, 25,
                     kWhite.with_alpha(0.8f));
            break;
        }
        if (refresh_ == Refresh::unable)
        {
            const std::string why = renaming_ ? "Art Swap renames games that ShadowMountPlus installed from a folder, "
                                                "which it can then refresh. " + refresh_why_
                                              : refresh_why_;
            y = ui::paragraph(list, fonts_.regular, why, x, y, 27, width, 42, kWhite, 5) + 24;
            ui::text(list, fonts_.semibold, renaming_ ? "WHAT A RENAME NEEDS" : "WHAT A REFRESH NEEDS", x, y, 17,
                     kWhite.with_alpha(0.55f),
                     gfx::Align::left, 3.0f);
            y += 44;
            need("ShadowMountPlus 1.7 or later, running", !smp_version_.empty(),
                 smp_version_.empty() ? "not answering" : "ShadowMountPlus " + smp_version_);
            need("A game ShadowMountPlus installed from a folder", smp_folder_, smp_folder_ ? "yes" : "no");
            break;
        }
        y = ui::paragraph(list, fonts_.regular,
                          renamed_to_.empty()
                              ? "The home screen keeps its own copy of each game's art, so new art shows once the game "
                                "is installed again. Each game is refreshed on its own; Art Swap does it for you:"
                              : "Renamed. The home screen shows a game's name as it was when the game was installed, "
                                "so the new name shows once it is installed again. Art Swap does that for you:",
                          x, y, 25, width, 38, kWhite.with_alpha(0.85f), 3) +
            16;
        const std::string steps[] = {"Art Swap closes.", name + " leaves the home screen for a few seconds.",
                                     renamed_to_.empty() ? "ShadowMountPlus adds it straight back, with its new art."
                                                         : "ShadowMountPlus adds it straight back, with its new name."};
        for (int i = 0; i < 3; ++i)
        {
            list.bordered_rect({x, y - 27, 36, 36}, 18, accent.with_alpha(0.16f), 1.5f, accent.with_alpha(0.8f));
            ui::text(list, fonts_.semibold, std::to_string(i + 1), x + 18, y - 2, 20, kWhite, gfx::Align::center);
            ui::text(list, fonts_.regular, fonts_.regular.font->fit(steps[i], 25, width - 56), x + 56, y, 25, kWhite);
            y += 52;
        }
        y += 14;
        // saved data: what the refresh does to keep it, in a box as tall as its lines
        const std::string kept = "Nothing is deleted: the game's folder, with its saves and settings, stays where it "
                                 "is. Art Swap notes every file first and checks them all afterwards.";
        const float kept_lines =
            static_cast<float>(std::min<std::size_t>(fonts_.regular.font->wrap(kept, 22, width - 40).size(), 3));
        const Rect safe{x - 4, y - 8, width + 8, 78 + 32 * (kept_lines - 1) + 38};
        list.bordered_rect(safe, 18, Color::rgb(0x3ddc84, 0.08f), 1.5f, Color::rgb(0x3ddc84, 0.45f));
        ui::text(list, fonts_.semibold, "YOUR SAVED DATA STAYS", safe.x + 24, safe.y + 40, 17, Color::rgb(0x3ddc84),
                 gfx::Align::left, 3.0f);
        ui::paragraph(list, fonts_.regular, kept, safe.x + 24, safe.y + 78, 22, safe.w - 48, 32,
                      kWhite.with_alpha(0.85f), 3);
        y = safe.y + safe.h + 48;
        // what it needs; once it has started, what it is doing
        if (refresh_ == Refresh::starting)
        {
            draw_spinner(list, x + 18, y - 9, 16);
            ui::text(list, fonts_.semibold, refresh_step_ + "\xE2\x80\xA6", x + 50, y, 25, kWhite);
        }
        else
            need("ShadowMountPlus 1.7 or later, running", true, "ShadowMountPlus " + smp_version_);
        break;
    }
    case Sheet::search:
    {
        heading("WHICH GAME IS IT?", name);
        std::vector<std::pair<std::string, std::string>> items;
        items.emplace_back("Search for another name\xE2\x80\xA6", "keyboard");
        for (const auto &row : results_)
            items.emplace_back(row[2], row[3] != "0" ? row[3] : std::string());
        // a window of rows around the focus
        const int window = 8;
        const int first = std::clamp(sheet_focus_ - window / 2, 0, std::max(0, static_cast<int>(items.size()) - window));
        std::vector<std::pair<std::string, std::string>> shown(
            items.begin() + first, items.begin() + std::min(static_cast<int>(items.size()), first + window));
        rows(shown, sheet_focus_ - first, sheet_focus_position_.value - static_cast<float>(first));
        if (searching_)
        {
            draw_spinner(list, x + 22, y + 40, 18);
            ui::text(list, fonts_.regular, "Searching for \xE2\x80\x9C" + search_term_ + "\xE2\x80\x9D\xE2\x80\xA6",
                     x + 60, y + 50, 24, kWhite.with_alpha(0.8f));
        }
        else if (!search_error_.empty())
            ui::text(list, fonts_.regular, search_error_, x, y + 50, 24, kAmber);
        break;
    }
    case Sheet::log:
    {
        heading("HELPER LOG", "/data/artswap/helper.log");
        // the last lines that fit
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < log_text_.size())
        {
            std::size_t end = log_text_.find('\n', start);
            if (end == std::string::npos)
                end = log_text_.size();
            lines.push_back(log_text_.substr(start, end - start));
            start = end + 1;
        }
        const int fit = 15;
        const int first = std::max(0, static_cast<int>(lines.size()) - fit);
        for (int i = first; i < static_cast<int>(lines.size()); ++i)
        {
            ui::text(list, fonts_.mono, fonts_.mono.font->fit(lines[static_cast<std::size_t>(i)], 18, panel.w - 128),
                     x, y, 18, kWhite.with_alpha(0.85f));
            y += 28;
        }
        break;
    }
    default:
        break;
    }
    list.pop_opacity();
}

void App::draw_toasts(gfx::DrawList &list) const
{
    float y = 128;
    for (const Toast &t : toasts_)
    {
        const float in = tween::smoothstep(tween::clamp01(t.age * 4.0f));
        const float out = tween::clamp01((t.life - t.age) * 2.5f);
        const float a = in * out;
        if (a <= 0.01f)
            continue;
        const float w = 820.0f;
        const float h = t.detail.empty() ? 76.0f : 132.0f;
        const Rect r{1824 - w + 30.0f * (1.0f - in), y, w, h};
        list.push_opacity(a);
        list.shadow({r.x, r.y + 12, r.w, r.h}, 24, 30, Color::rgb(0x000000, 0.5f));
        list.rounded_rect(r, 24, gfx::mix(kInk, palette_[1].value(), 0.3f).with_alpha(0.96f));
        list.rounded_rect({r.x, r.y, 8, r.h}, 4, t.error ? kRed : accent());
        ui::text(list, fonts_.semibold, fonts_.semibold.font->fit(t.title, 26, w - 70), r.x + 36, r.y + 46, 26, kWhite);
        if (!t.detail.empty())
            ui::paragraph(list, fonts_.regular, t.detail, r.x + 36, r.y + 84, 20, w - 70, 28, kWhite.with_alpha(0.75f), 2);
        list.pop_opacity();
        y += h + 16;
    }
}

void App::draw_hints(gfx::DrawList &list) const
{
    const ui::GlyphStyle style = ui::GlyphStyle::dark();
    std::vector<ui::Hint> hints;
    if (sheet_ == Sheet::confirm)
        hints = {{ui::Button::cross, slot_ == "icon"   ? "Use this icon"
                                     : slot_ == "logo" ? "Use this logo"
                                                       : "Use this background"},
                 {ui::Button::circle, "Cancel"}};
    else if (sheet_ == Sheet::restore)
        hints = {{ui::Button::cross, "Restore"}, {ui::Button::circle, "Cancel"}};
    else if (sheet_ == Sheet::search)
        hints = {{ui::Button::cross, "Choose"}, {ui::Button::circle, "Back"}};
    else if (sheet_ == Sheet::log)
        hints = {{ui::Button::circle, "Back"}};
    else if (sheet_ == Sheet::apply)
    {
        if (!applying_)
            hints = {{ui::Button::cross, "Apply changes"}, {ui::Button::circle, "Cancel"}};
    }
    else if (sheet_ == Sheet::refresh)
    {
        if (refresh_ == Refresh::ready)
            hints = {{ui::Button::cross, "Refresh"}, {ui::Button::circle, "Cancel"}};
        else if (refresh_ == Refresh::unable || (refresh_ == Refresh::asking && !renaming_))
            hints = {{ui::Button::circle, "Back"}};
    }
    else if (sheet_ == Sheet::working)
        hints = {};
    else if (screen_ == Screen::starting)
    {
        if (helper_.state() == Helper::State::failed)
            hints = {{ui::Button::cross, "Try again"}};
    }
    else if (screen_ == Screen::settings)
    {
        if (settings_in_rows_)
            hints = {{ui::Button::dpad, "Choose"}, {ui::Button::cross, "Select"}, {ui::Button::circle, "Back"}};
        else
            hints = {{ui::Button::dpad, "Choose"}, {ui::Button::cross, "Open"}, {ui::Button::circle, "Close"}};
    }
    else if (screen_ == Screen::connect)
        hints = {{ui::Button::dpad, "Choose"}, {ui::Button::cross, "Select"},
                 {ui::Button::circle, connect_back_ == Screen::settings ? "Back" : "Not now"}};
    else if (screen_ == Screen::library)
    {
        const Title *title = focused_title();
        if (title)
        {
            hints.push_back({ui::Button::cross, "Icon"});
            if (title->has_pic() || title->refresh)
                hints.push_back({ui::Button::triangle, "Background"});
            if (title->listed)
                hints.push_back({ui::Button::r1, "Logo"});
            if (title->changed())
                hints.push_back({ui::Button::square, "Restore"});
            if (title->refresh)
                hints.push_back({ui::Button::l2, "Rename"});
        }
        hints.push_back({ui::Button::r2, "Apply changes"});
        hints.push_back({ui::Button::options, "Settings"});
    }
    else if (screen_ == Screen::frame)
        hints = {{ui::Button::left_stick, "Move"}, {ui::Button::r2, "Zoom in"}, {ui::Button::l2, "Zoom out"},
                 {ui::Button::cross, "Use this"}, {ui::Button::circle, "Back"}};
    else
    {
        if (focused_art())
        {
            hints.push_back({ui::Button::cross, "Use this"});
            if (slot_ != "logo")
                hints.push_back({ui::Button::triangle, "Frame"});
        }
        hints.push_back({ui::Button::square, "Another game"});
        hints.push_back({ui::Button::circle, "Back"});
    }
    if (hints.empty())
        return;
    list.push_opacity(tween::stagger(age_, 6, 0.06f, 0.4f));
    ui::draw_hints(list, fonts_, style, hints.data(), static_cast<int>(hints.size()), 1824, true);
    list.pop_opacity();
}

} // namespace artswap
