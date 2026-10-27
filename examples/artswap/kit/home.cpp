/*
 * Art Swap - the home screen page: which home each tile is on, Games or
 * Media, the tiles hidden from both, and the order of the Games row.
 *
 * The rail has the three places and the order; each place lists its tiles,
 * and ✕ on one moves it (the place sheet). □ picks up a game on the Games row
 * and the D-pad moves it along. The helper keeps what the person chose and
 * writes it into the home screen's database at Apply changes (layout.c),
 * just before the home screen restarts, and again at every Apply changes
 * after, so a firmware update or an install that put a tile back is put right
 * again. A locked order stays as it is when other games are played.
 *
 * The page follows the kit's Settings design (page.hpp).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app.hpp"
#include "page.hpp"
#include "title.hpp"

#include "audio/cues.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <cctype>
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
using namespace page;

constexpr int kCategories = 4;
constexpr const char *kCategoryNames[kCategories] = {"Games", "Media", "Hidden", "Order"};
constexpr const char *kPlaces[3] = {"games", "media", "hidden"};
constexpr float kListTop = kPanel.y + 100.0f;
constexpr float kListBottom = kPanel.y + kPanel.h - 100.0f;
constexpr const char *kArtSwap = "PPSA99318";

int place_index(const std::string &where)
{
    for (int i = 0; i < 3; ++i)
        if (where == kPlaces[i])
            return i;
    return 0;
}

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

// ================================================================ the list

const App::Tile *App::tile(const std::string &id) const
{
    for (const Tile &t : tiles_)
        if (t.id == id)
            return &t;
    return nullptr;
}

std::vector<int> App::home_rows(int category) const
{
    std::vector<int> rows;
    if (category < 0 || category > 2)
        return rows;
    for (int i = 0; i < static_cast<int>(tiles_.size()); ++i)
        if (tiles_[static_cast<std::size_t>(i)].where == kPlaces[category])
            rows.push_back(i);
    return rows;
}

int App::home_row_count(int category) const
{
    return category == 3 ? 2 : static_cast<int>(home_rows(category).size());
}

std::vector<std::string> App::games_order() const
{
    std::vector<std::string> ids;
    for (const Tile &t : tiles_)
        if (t.where == "games" && t.kind != "system")
            ids.push_back(t.id);
    return ids;
}

Rect App::home_rail_rect(int category) const
{
    return {kMargin, kRailY + static_cast<float>(category) * kRailPitch, kRailW, kRailH};
}

Rect App::home_row_rect(int category, int row) const
{
    if (category == 3)
    {
        // under the page's one row of tiles
        const float top = kPanel.y + 100.0f + 140.0f + 24.0f;
        return {kPanel.x + kPanelPad, top + static_cast<float>(row) * kRowPitch, kPanel.w - 2 * kPanelPad, kRowH};
    }
    return {kPanel.x + kPanelPad, kListTop + static_cast<float>(row) * kRowPitch - home_scroll_.offset(),
            kPanel.w - 2 * kPanelPad - 18.0f, kRowH};
}

void App::open_home()
{
    screen_ = Screen::home;
    home_in_rows_ = false;
    moving_ = false;
    page_age_ = 0.0f;
    age_ = 0.0f;
    // the game in focus on the shelf is found in the list once it has come
    const Title *title = focused_title();
    home_focus_id_ = title ? title->id : "";
    rail_marker_.snap(home_rail_rect(home_category_).y);
    page_focus_.snap(home_rail_rect(home_category_));
    home_scroll_.position.snap(0.0f);
    load_layout();
}

void App::load_layout()
{
    if (layout_loading_)
        return;
    layout_loading_ = true;
    helper_.ask({"LAYOUT"}, [this](Reply &reply) {
        layout_loading_ = false;
        if (!reply.ok)
        {
            layout_error_ = reply.error;
            return;
        }
        layout_error_.clear();
        std::vector<Tile> list;
        for (const auto &row : reply.rows())
        {
            if (row.size() >= 2 && row[0] == "order")
                layout_order_ = row[1];
            else if (row.size() >= 6 && row[0] == "T")
                list.push_back({row[1], row[2], row[3], row[4], row[5]});
        }
        tiles_ = std::move(list);
        // the shelf's game: its place opens with it in focus
        if (const Tile *t = tile(home_focus_id_); t && screen_ == Screen::home && !home_in_rows_)
        {
            home_category_ = place_index(t->where);
            const std::vector<int> rows = home_rows(home_category_);
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (tiles_[static_cast<std::size_t>(rows[static_cast<std::size_t>(i)])].id == home_focus_id_)
                    home_row_[home_category_] = i;
            home_in_rows_ = true;
            page_age_ = 0.0f;
            const Rect r = home_row_rect(home_category_, home_row_[home_category_]);
            const float start = r.y + home_scroll_.offset() - kListTop;
            home_scroll_.reveal(start, start + kRowH, kListBottom - kListTop, kRowPitch);
            home_scroll_.position.snap(home_scroll_.position.target);
            rail_marker_.snap(home_rail_rect(home_category_).y);
            page_focus_.snap(home_row_rect(home_category_, home_row_[home_category_]));
        }
        home_focus_id_.clear();
        for (int c = 0; c < 3; ++c)
            home_row_[c] = std::clamp(home_row_[c], 0, std::max(0, home_row_count(c) - 1));
    }, true);
}

// ================================================================ changes

void App::place_tile(const std::string &id, const std::string &where)
{
    const Tile *t = tile(id);
    if (t == nullptr || t->where == where)
        return;
    const std::string name = t->name;
    // the tile stays in focus, in its new place
    for (Tile &each : tiles_)
        if (each.id == id)
            each.where = where;
    helper_.ask({"PLACE", id, where}, [this, id, name, where](Reply &reply) {
        if (!reply.ok)
        {
            toast(name + " was not moved", reply.error, true);
            load_layout();
            return;
        }
        for (const auto &row : reply.rows())
            if (row.size() >= 6 && row[0] == "T")
                for (Tile &each : tiles_)
                    if (each.id == row[1])
                        each = {row[1], row[2], row[3], row[4], row[5]};
        const Tile *now = tile(id);
        const bool waits = now && now->where != now->now;
        toast(name + (where == "hidden" ? " is hidden" : where == "media" ? " goes on Media" : " goes on Games"),
              waits ? "Apply changes (R2) shows it on the home screen." : "The home screen has it there already.");
        load_pending();
    }, true);
    for (int c = 0; c < 3; ++c)
        home_row_[c] = std::clamp(home_row_[c], 0, std::max(0, home_row_count(c) - 1));
}

void App::send_order(const std::string &mode, const std::vector<std::string> &ids)
{
    std::vector<std::string> request = {"ORDER", mode};
    request.insert(request.end(), ids.begin(), ids.end());
    helper_.ask(request, [this, mode](Reply &reply) {
        if (!reply.ok)
        {
            toast("The order was not kept", reply.error, true);
            load_layout();
            return;
        }
        for (const auto &row : reply.rows())
            if (row.size() >= 2 && row[0] == "order")
                layout_order_ = row[1];
        toast(mode == "locked" ? "The order is locked"
              : mode == "free" ? "The new order is kept"
                               : "The order is free again",
              mode == "locked" ? "Apply changes (R2) puts your games in this order, and playing a game no longer "
                                 "moves it first."
              : mode == "free" ? "Apply changes (R2) puts your games in this order. Playing a game moves it first, "
                                 "as before."
                               : "Apply changes (R2), and playing a game moves it first again.");
        load_pending();
    }, true);
}

// ================================================================ input

void App::update_home(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    const int rows = home_row_count(home_category_);
    if (moving_)
    {
        // a game on the Games row, carried with the D-pad; ✕ or □ puts it down, ○ puts it back
        const std::vector<int> list = home_rows(0);
        int &row = home_row_[0];
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = row + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < static_cast<int>(list.size()) &&
                tiles_[static_cast<std::size_t>(list[static_cast<std::size_t>(next)])].kind != "system")
            {
                std::swap(tiles_[static_cast<std::size_t>(list[static_cast<std::size_t>(row)])],
                          tiles_[static_cast<std::size_t>(list[static_cast<std::size_t>(next)])]);
                row = next;
                feedback.play(Cue::focus, 1.0f, 0.3f);
            }
            else if (!input.nav_repeat)
                feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
        }
        else if (input.is_pressed(Action::confirm) || input.is_pressed(Action::west))
        {
            moving_ = false;
            feedback.play(Cue::select);
            send_order(layout_order_ == "locked" ? "locked" : "free", games_order());
        }
        else if (input.is_pressed(Action::back))
        {
            moving_ = false;
            feedback.play(Cue::back);
            tiles_ = moving_before_;
            const std::vector<int> again = home_rows(0);
            for (int i = 0; i < static_cast<int>(again.size()); ++i)
                if (tiles_[static_cast<std::size_t>(again[static_cast<std::size_t>(i)])].id == place_id_)
                    row = i;
        }
    }
    else
    {
        auto change_category = [&](int next) {
            next = std::clamp(next, 0, kCategories - 1);
            if (next == home_category_)
            {
                feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
                return;
            }
            home_category_ = next;
            page_age_ = 0.0f;
            home_scroll_.position.snap(0.0f);
            feedback.play(Cue::tab, 1.08f - 0.06f * static_cast<float>(next));
        };
        if (input.is_pressed(Action::jump_next))
        {
            feedback.play(Cue::modal_open);
            open_apply();
            return;
        }
        if (input.is_pressed(Action::page_prev))
            change_category(home_category_ - 1);
        else if (input.is_pressed(Action::page_next))
            change_category(home_category_ + 1);
        else if (!home_in_rows_)
        {
            if (input.nav == Direction::up || input.nav == Direction::down)
                change_category(home_category_ + (input.nav == Direction::down ? 1 : -1));
            else if ((input.nav == Direction::right || input.is_pressed(Action::confirm)) && rows > 0)
            {
                home_in_rows_ = true;
                home_row_[home_category_] = std::clamp(home_row_[home_category_], 0, rows - 1);
                feedback.play(Cue::focus, 1.0f, 0.3f);
            }
            else if (input.is_pressed(Action::confirm))
                feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
            else if (input.is_pressed(Action::back))
            {
                feedback.play(Cue::back);
                screen_ = Screen::library;
                age_ = 0.3f;
                return;
            }
        }
        else if (rows == 0)
            home_in_rows_ = false; // its last tile moved away
        else
        {
            int &row = home_row_[home_category_];
            if (input.nav == Direction::up || input.nav == Direction::down)
            {
                const int next = std::clamp(row + (input.nav == Direction::down ? 1 : -1), 0, std::max(0, rows - 1));
                if (next != row)
                {
                    row = next;
                    feedback.play(Cue::focus, 1.0f, 0.3f);
                }
                else if (!input.nav_repeat)
                    feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
            }
            else if (input.nav == Direction::left || input.is_pressed(Action::back))
            {
                home_in_rows_ = false;
                feedback.play(Cue::back);
            }
            else if (input.is_pressed(Action::confirm))
                home_select(feedback);
            else if (input.is_pressed(Action::west) && home_category_ == 0)
            {
                // a game, not one of the system's tiles, which keep their own places at the end of the row
                const std::vector<int> list = home_rows(0);
                const Tile &t = tiles_[static_cast<std::size_t>(list[static_cast<std::size_t>(row)])];
                if (t.kind == "system")
                {
                    feedback.play(Cue::error);
                    toast(t.name + " keeps its own place", "The system's tiles stay at the end of the Games row.",
                          true);
                }
                else
                {
                    moving_ = true;
                    moving_before_ = tiles_;
                    place_id_ = t.id;
                    feedback.play(Cue::select);
                }
            }
        }
    }

    // ---- motion ----
    const int count = home_row_count(home_category_);
    if (home_category_ < 3 && count > 0)
    {
        const float start = static_cast<float>(home_row_[home_category_]) * kRowPitch;
        home_scroll_.reveal(start, start + kRowH, kListBottom - kListTop, kRowPitch * 0.5f,
                            static_cast<float>(count) * kRowPitch);
    }
    home_scroll_.update(dt, 16.0f);
    rail_marker_.target = home_rail_rect(home_category_).y;
    rail_marker_.update(dt, 20.0f);
    page_focus_.target(home_in_rows_ && count > 0 ? home_row_rect(home_category_, home_row_[home_category_])
                                                  : home_rail_rect(home_category_));
    page_focus_.update(dt, 22.0f);
}

void App::home_select(ui::Feedback &feedback)
{
    const int row = home_row_[home_category_];
    if (home_category_ < 3)
    {
        const std::vector<int> list = home_rows(home_category_);
        if (row >= static_cast<int>(list.size()))
            return;
        const Tile &t = tiles_[static_cast<std::size_t>(list[static_cast<std::size_t>(row)])];
        place_id_ = t.id;
        sheet_ = Sheet::place;
        sheet_focus_ = place_index(t.where);
        sheet_focus_position_.snap(static_cast<float>(sheet_focus_));
        feedback.play(Cue::modal_open);
        return;
    }
    if (row == 0)
    {
        // the order locked as it is now, or free again
        feedback.play(Cue::toggle);
        if (layout_order_ == "locked")
            send_order("none", {});
        else
            send_order("locked", games_order());
        return;
    }
    // A to Z: the games on the row, in the places the games had, sorted by name
    feedback.play(Cue::select);
    std::vector<int> slots;
    std::vector<Tile> games;
    for (int i = 0; i < static_cast<int>(tiles_.size()); ++i)
        if (tiles_[static_cast<std::size_t>(i)].where == "games" && tiles_[static_cast<std::size_t>(i)].kind != "system")
        {
            slots.push_back(i);
            games.push_back(tiles_[static_cast<std::size_t>(i)]);
        }
    std::stable_sort(games.begin(), games.end(),
                     [](const Tile &a, const Tile &b) { return lower(a.name) < lower(b.name); });
    for (std::size_t i = 0; i < slots.size(); ++i)
        tiles_[static_cast<std::size_t>(slots[i])] = games[i];
    send_order(layout_order_ == "locked" ? "locked" : "free", games_order());
}

void App::update_place_sheet(const hui::InputFrame &input, ui::Feedback &feedback)
{
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = std::clamp(sheet_focus_ + (input.nav == Direction::down ? 1 : -1), 0, 2);
        if (next != sheet_focus_)
        {
            sheet_focus_ = next;
            feedback.play(Cue::focus, 1.0f, 0.2f);
        }
        else if (!input.nav_repeat)
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (input.is_pressed(Action::confirm))
    {
        const std::string where = kPlaces[sheet_focus_];
        // Art Swap hidden would leave no way back to it from the home screen
        if (place_id_ == kArtSwap && where == "hidden")
        {
            feedback.play(Cue::error);
            toast("Art Swap stays on the home screen", "Hidden, there would be no way back to it to show it again.",
                  true);
            return;
        }
        feedback.play(Cue::select);
        sheet_ = Sheet::none;
        place_tile(place_id_, where);
    }
    else if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::modal_close);
        sheet_ = Sheet::none;
    }
}

// ================================================================ drawing

void App::draw_home(gfx::DrawList &list) const
{
    const float in = tween::stagger(age_, 0, 0.06f, 0.5f);
    draw_header(list, fonts_, "Home screen", in);

    // the rail: the plate of the active category, the focus, then the names on top
    const float rail_in = tween::stagger(age_, 1, 0.05f, 0.45f);
    list.push_opacity(rail_in);
    list.rounded_rect({kMargin, rail_marker_.value, kRailW, kRailH}, kRadius, kText.with_alpha(0.06f));
    // what waits for Apply changes, under the rail
    const int waiting = static_cast<int>(arranged_.size());
    list.circle(kMargin + 6, 866, 4, waiting > 0 ? kWarn : kAccent);
    ui::paragraph(list, fonts_.regular,
                  waiting > 0 ? std::to_string(waiting) + (waiting == 1 ? " change waits" : " changes wait") +
                                    " for Apply changes."
                              : "Changes show at Apply changes (R2).",
                  kMargin + 24, 874, 22, kRailW - 40, 32, kText.with_alpha(0.55f), 2);
    list.pop_opacity();

    const float panel_in = tween::stagger(age_, 3, 0.06f, 0.55f);
    list.push_opacity(panel_in);
    draw_panel(list, kPanel);
    list.pop_opacity();

    static const Glyph glyphs[kCategories] = {Glyph::games, Glyph::media, Glyph::hidden, Glyph::order};
    for (int i = 0; i < kCategories; ++i)
    {
        const float item_in = tween::stagger(age_, 1 + i, 0.05f, 0.45f);
        const Rect r = home_rail_rect(i);
        const bool active = i == home_category_;
        const float x = r.x - 28.0f * (1.0f - item_in);
        list.push_opacity(item_in);
        draw_rail_icon(list, glyphs[i], x + 46, r.cy(), active ? kAccent : kText.with_alpha(0.6f));
        ui::text(list, active ? fonts_.semibold : fonts_.regular, kCategoryNames[i], x + 88, centred(r.cy(), 28), 28,
                 kText.with_alpha(active ? 1.0f : 0.68f));
        // how many tiles each place has; whether the order is locked
        const std::string count = i < 3 ? (tiles_.empty() ? "" : std::to_string(home_row_count(i)))
                                        : (layout_order_ == "locked" ? "Locked" : "");
        ui::text(list, fonts_.regular, count, r.x + r.w - 24, centred(r.cy(), 22), 22,
                 kText.with_alpha(active ? 0.75f : 0.45f), gfx::Align::right);
        list.pop_opacity();
    }
    list.rounded_rect({kMargin + 8, rail_marker_.value + 20, 4, kRailH - 40}, 2, kAccent.with_alpha(rail_in));

    // the page
    const int c = home_category_;
    const float rise = 18.0f;
    const float title_in = tween::stagger(page_age_, 0, 0.05f, 0.3f);
    list.push_opacity(title_in * panel_in);
    static const char *titles[kCategories] = {"On the Games home", "On the Media home", "Hidden from both",
                                              "The order of your games"};
    ui::text(list, fonts_.semibold, titles[c], kPanel.x + kPanelPad + kRowPad, kPanel.y + 66 + rise * (1.0f - title_in),
             36, kText);
    list.pop_opacity();
    auto staggered = [&](int index, const auto &draw) {
        const float t = tween::stagger(page_age_, index, 0.05f, 0.32f);
        list.push_opacity(t * panel_in);
        list.push_transform(1.0f, 0, 0, 0, rise * (1.0f - t));
        draw();
        list.pop_transform();
        list.pop_opacity();
    };

    std::string description;
    if (layout_loading_ && tiles_.empty())
    {
        draw_spinner(list, kPanel.x + kPanelPad + kRowPad + 18, kListTop + 40, 18);
        ui::text(list, fonts_.regular, "Reading the home screen\xE2\x80\xA6", kPanel.x + kPanelPad + kRowPad + 56,
                 kListTop + 50, 25, kText.with_alpha(0.8f));
        description = "Art Swap reads the home screen's own list of tiles.";
    }
    else if (!layout_error_.empty() && tiles_.empty())
    {
        ui::paragraph(list, fonts_.regular, layout_error_, kPanel.x + kPanelPad + kRowPad, kListTop + 40, 25,
                      kPanel.w - 2 * (kPanelPad + kRowPad), 38, kWarn, 4);
        description = "The home screen's list could not be read.";
    }
    else if (c < 3)
    {
        const std::vector<int> rows = home_rows(c);
        // the focus, under the rows it lights
        list.push_clip({kPanel.x, kListTop - 6, kPanel.w, kListBottom - kListTop + 12});
        list.push_opacity(tween::stagger(age_, 4, 0.06f, 0.4f));
        if (home_in_rows_ && !rows.empty())
            draw_focus(list, page_focus_.value(), ui::breathe(clock_));
        list.pop_opacity();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const Rect r = home_row_rect(c, i);
            if (r.y > kListBottom || r.y + r.h < kListTop - 6)
                continue;
            const Tile &t = tiles_[static_cast<std::size_t>(rows[static_cast<std::size_t>(i)])];
            const bool focused = home_in_rows_ && i == home_row_[c];
            staggered(std::min(1 + i, 8), [&] {
                if (!focused)
                    list.rounded_rect(r, kRadius, kText.with_alpha(0.035f));
                // its icon, when Art Swap has it; the system's tiles get their first letter
                const Rect icon{r.x + kRowPad - 8, r.cy() - 24, 48, 48};
                std::uint32_t texture = 0;
                for (const auto &title : titles_)
                    if (title->id == t.id)
                        texture = title->icon;
                if (texture)
                    list.image(texture, icon, gfx::kFullUv, kText.with_alpha(focused ? 1.0f : 0.85f), 10);
                else
                {
                    list.rounded_rect(icon, 10, kText.with_alpha(0.08f));
                    ui::text(list, fonts_.semibold, t.name.empty() ? std::string("?") : t.name.substr(0, 1), icon.cx(),
                             centred(icon.cy(), 24), 24, kText.with_alpha(0.7f), gfx::Align::center);
                }
                const bool carried = moving_ && focused;
                ui::text(list, focused ? fonts_.semibold : fonts_.regular,
                         fonts_.regular.font->fit(t.name, 27, r.w * 0.58f), icon.x + icon.w + 22,
                         centred(r.cy(), 27), 27, kText.with_alpha(focused ? 1.0f : 0.84f));
                // where it is now, when that is not where it goes
                std::string value;
                Color ink = kText.with_alpha(focused ? 0.75f : 0.5f);
                if (carried)
                    value = "Move it with the D-pad";
                else if (t.where != t.now)
                {
                    value = "Now on " + std::string(t.now == "hidden" ? "neither" : t.now == "media" ? "Media" : "Games") +
                            ": Apply changes";
                    ink = kWarn.with_alpha(focused ? 1.0f : 0.8f);
                }
                else if (t.kind == "system")
                    value = "The system's";
                else
                    for (const auto &title : titles_)
                        if (title->id == t.id)
                            value = title->kind == "homebrew" ? "Homebrew" : title->kind == "ps4" ? "PS4 game" : "PS5 game";
                ui::text(list, fonts_.regular, value, r.x + r.w - kRowPad - (carried ? 30 : 0), centred(r.cy(), 23), 23,
                         ink, gfx::Align::right);
                if (carried)
                {
                    // the arrows it moves by
                    const float ax = r.x + r.w - kRowPad - 8;
                    list.line(ax - 7, r.cy() - 8, ax, r.cy() - 15, 2.5f, kAccent);
                    list.line(ax + 7, r.cy() - 8, ax, r.cy() - 15, 2.5f, kAccent);
                    list.line(ax - 7, r.cy() + 8, ax, r.cy() + 15, 2.5f, kAccent);
                    list.line(ax + 7, r.cy() + 8, ax, r.cy() + 15, 2.5f, kAccent);
                }
            });
        }
        list.pop_clip();
        // more above or below: a thin bar beside the rows
        const float content = static_cast<float>(rows.size()) * kRowPitch, view = kListBottom - kListTop;
        if (content > view + 1.0f)
        {
            const float track_x = kPanel.x + kPanel.w - kPanelPad - 6;
            list.rounded_rect({track_x, kListTop, 4, view}, 2, kText.with_alpha(0.06f));
            const float bar_h = std::max(40.0f, view * view / content);
            const float bar_y = kListTop + (view - bar_h) * std::clamp(home_scroll_.offset() / (content - view), 0.0f, 1.0f);
            list.rounded_rect({track_x, bar_y, 4, bar_h}, 2, kAccent.with_alpha(0.7f));
        }
        if (rows.empty())
        {
            static const char *empty[3] = {
                "Nothing is on the Games home. Move a tile here from Media or Hidden.",
                "Nothing is on the Media home. Choose a tile on Games, then Media: tools and apps sit well there.",
                "Nothing is hidden. Choose a tile on Games or Media, then Hidden, to take it off the home screen."};
            ui::paragraph(list, fonts_.regular, empty[c], kPanel.x + kPanelPad + kRowPad, kListTop + 40, 25,
                          kPanel.w - 2 * (kPanelPad + kRowPad), 38, kText.with_alpha(0.7f), 3);
        }
        static const char *about[3] = {
            "The main row. Choose a tile to move it or hide it, or pick a game up to change its place.",
            "The Media home, beside Games. Tools, players and stores fit here, out of the way of your games.",
            "Tiles on neither home. They stay installed, and Art Swap can put them back."};
        description = moving_ ? "Up and down carry it along the row. Put it down where you want it, or go back to "
                                "leave it where it was."
                              : about[c];
    }
    else
    {
        const float tile_w = (kPanel.w - 2 * kPanelPad - 16.0f) * 0.5f;
        auto tile_rect = [&](int i) {
            return Rect{kPanel.x + kPanelPad + static_cast<float>(i) * (tile_w + 16.0f), kPanel.y + 100.0f, tile_w,
                        140.0f};
        };
        const bool locked = layout_order_ == "locked";
        const int games = static_cast<int>(games_order().size());
        staggered(1, [&] {
            draw_tile(list, fonts_, tile_rect(0), "THE ORDER", locked ? "Locked" : "As you play",
                      locked ? "Playing a game leaves it where it is" : "The game you played last comes first",
                      locked ? kGood : kClear);
        });
        staggered(2, [&] {
            draw_tile(list, fonts_, tile_rect(1), "ON THE GAMES ROW", std::to_string(games),
                      games == 1 ? "game, beside the system's tiles" : "games, beside the system's tiles");
        });
        list.push_opacity(tween::stagger(age_, 4, 0.06f, 0.4f) * panel_in);
        if (home_in_rows_)
            draw_focus(list, page_focus_.value(), ui::breathe(clock_));
        list.pop_opacity();
        const RowInfo rows[2] = {{"Lock the order", locked ? "On" : "Off"},
                                 {"Put the games in A to Z order", std::to_string(games) + " games"}};
        for (int i = 0; i < 2; ++i)
            staggered(3 + i, [&] {
                draw_row(list, fonts_, home_row_rect(3, i), rows[i], home_in_rows_ && i == home_row_[3]);
            });
        static const char *notes[2] = {
            "Keeps the Games row in the order it has now, or the one you set by picking games up on Games.",
            "Sorts the games by name. Locked, they stay that way; otherwise playing one moves it first again."};
        description = home_in_rows_ ? notes[home_row_[3]]
                                    : "The Games row puts the game played last first. Lock it to keep your own order.";
    }
    list.push_opacity(tween::stagger(page_age_, 6, 0.05f, 0.36f) * panel_in);
    draw_description(list, fonts_, kPanel, description);
    list.pop_opacity();
}

void App::draw_place_sheet(gfx::DrawList &list, const Rect &panel, float x, float y) const
{
    const Tile *t = tile(place_id_);
    const std::string name = t ? t->name : "";
    ui::text(list, fonts_.semibold, "MOVE", x, y, 19, kAccent, gfx::Align::left, 4.0f);
    ui::text(list, fonts_.display, fonts_.display.font->fit(name, 48, panel.w - 128), x - 2, y + 62, 48,
             Color::rgb(0xffffff));
    y += 120;
    const std::string where = t ? t->where : "";
    const struct
    {
        const char *label, *note, *place;
    } items[3] = {{"Games", "the main row of the home screen", "games"},
                  {"Media", "the Media home, beside Games", "media"},
                  {"Hidden", "on neither; it stays installed", "hidden"}};
    const float row_h = 64, width = panel.w - 128;
    list.rounded_rect({x - 20, y + sheet_focus_position_.value * row_h, width + 40, row_h - 8}, 28,
                      Color::rgb(0xffffff));
    for (int i = 0; i < 3; ++i)
    {
        const bool f = i == sheet_focus_;
        const float ry = y + i * row_h;
        const Color ink = f ? Color::rgb(0x0b0d16) : Color::rgb(0xffffff, 0.9f);
        const float lw = ui::text(list, fonts_.semibold, items[i].label, x + 8, ry + 39, 26, ink);
        // where it goes now
        if (where == items[i].place)
            ui::text(list, fonts_.semibold, "NOW", x + 8 + lw + 18, ry + 37, 15, ink.with_alpha(0.6f),
                     gfx::Align::left, 3.0f);
        ui::text(list, fonts_.regular, items[i].note, x + width - 8, ry + 39, 22, ink.with_alpha(0.6f),
                 gfx::Align::right);
    }
    y += row_h * 3 + 30;
    ui::paragraph(list, fonts_.regular,
                  "The home screen shows it there once it restarts: Apply changes (R2). Art Swap writes it again at "
                  "each Apply changes, so an update that puts it back is put right.",
                  x, y, 23, width, 34, Color::rgb(0xffffff, 0.7f), 3);
}

} // namespace artswap
