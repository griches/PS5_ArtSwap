/*
 * Art Swap - the home screen page: which tab each tile is on, Games or Media,
 * the tiles hidden, a row's order and the tiles pinned in it, for one profile
 * at a time.
 *
 * The rail has the two tabs, the tiles hidden, and the page's options; each
 * tab lists its tiles as the home screen orders them, and ✕ on one opens what
 * it can do (the place sheet): move to the other tab (on every profile), pin
 * it at its place, hide it on this profile, or, for one of the system's own
 * tiles the system hides, show it on every profile. □ picks up a tile of the
 * row and the D-pad carries it along. The helper keeps what the person chose
 * and writes it into the home screen's databases at Apply changes (layout.c,
 * tiles.c), just before the home screen restarts; from then on, triggers in
 * the databases keep it through restarts, rest mode and installs.
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__PROSPERO__)
extern "C" int sceUserServiceGetInitialUser(int32_t *user);
#endif

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
constexpr const char *kCategoryNames[kCategories] = {"Games", "Media", "Hidden", "Options"};
constexpr int kOptions = 5;
constexpr float kListTop = kPanel.y + 100.0f;
constexpr float kListBottom = kPanel.y + kPanel.h - 100.0f;

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The profile that started Art Swap, as the home screen's tables name it (ten digits); "" off the console
std::string console_user()
{
#if defined(__PROSPERO__)
    int32_t user = 0;
    if (sceUserServiceGetInitialUser(&user) == 0 && user > 0)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%010u", static_cast<unsigned>(user));
        return text;
    }
#endif
    return "";
}

const char *tab_label(const std::string &tab)
{
    return tab == "media" ? "Media" : tab == "games" ? "Games" : "neither";
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
    {
        const Tile &t = tiles_[static_cast<std::size_t>(i)];
        const bool hidden = t.hidden || t.tab == "none";
        if (category == 2 ? hidden : !hidden && t.tab == (category == 0 ? "games" : "media"))
            rows.push_back(i);
    }
    return rows;
}

int App::home_row_count(int category) const
{
    return category == 3 ? kOptions : static_cast<int>(home_rows(category).size());
}

Rect App::home_rail_rect(int category) const
{
    return {kMargin, kRailY + static_cast<float>(category) * kRailPitch, kRailW, kRailH};
}

Rect App::home_row_rect(int category, int row) const
{
    if (category == 3)
        return {kPanel.x + kPanelPad, kListTop + static_cast<float>(row) * kRowPitch, kPanel.w - 2 * kPanelPad, kRowH};
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
    if (profile_.empty())
        profile_ = console_user();
    helper_.ask({"PROFILES"}, [this](Reply &reply) {
        if (!reply.ok)
            return;
        profiles_.clear();
        for (const auto &row : reply.rows())
            if (row.size() >= 3 && row[0] == "U")
                profiles_.push_back({row[1], row[2]});
    }, true);
    load_layout();
}

void App::load_layout()
{
    if (layout_loading_)
        return;
    layout_loading_ = true;
    helper_.ask({"LAYOUT", profile_}, [this](Reply &reply) {
        layout_loading_ = false;
        if (!reply.ok)
        {
            // a profile the database does not know (one made since): the first one
            if (!profile_.empty() && reply.error.find("no such profile") != std::string::npos)
            {
                profile_.clear();
                load_layout();
                return;
            }
            layout_error_ = reply.error;
            return;
        }
        layout_error_.clear();
        layout_conflict_ = false;
        std::vector<Tile> list;
        for (const auto &row : reply.rows())
        {
            if (row.size() >= 2 && row[0] == "profile")
                profile_ = row[1];
            else if (row.size() >= 2 && row[0] == "conflict")
                layout_conflict_ = true;
            else if (row.size() >= 2 && row[0] == "tabshifter")
                tabshifter_ = row[1];
            else if (row.size() >= 2 && row[0] == "reset")
                reset_queued_ = row[1] == "queued";
            else if (row.size() >= 10 && row[0] == "T")
            {
                Tile t;
                t.id = row[1];
                t.tab = row[2];
                t.now = row[3];
                t.hidden = row[4] == "1";
                t.now_hidden = row[5] == "1";
                t.kind = row[6];
                t.caps = row[7];
                t.pin = row[8] == "-" ? -1 : std::atoi(row[8].c_str());
                t.name = row[9];
                list.push_back(std::move(t));
            }
        }
        tiles_ = std::move(list);
        // the shelf's game: its place opens with it in focus
        if (const Tile *t = tile(home_focus_id_); t && screen_ == Screen::home && !home_in_rows_)
        {
            home_category_ = t->hidden || t->tab == "none" ? 2 : t->tab == "media" ? 1 : 0;
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

void App::change_tile(const std::string &id, const std::string &action)
{
    const Tile *t = tile(id);
    if (t == nullptr)
        return;
    const std::string name = t->name, tab = t->tab;
    if (action == "pin" || action == "unpin")
    {
        // the row as it is, this tile pinned at its place, or not
        for (Tile &each : tiles_)
            if (each.id == id)
                each.pin = action == "pin" ? 0 : -1;
        send_row(tab, action == "pin" ? name + " is pinned at its place" : name + " is no longer pinned");
        return;
    }
    std::vector<std::string> request;
    std::string done;
    if (action == "games" || action == "media")
    {
        request = {"TAB", profile_, id, action};
        done = name + " goes on " + tab_label(action);
    }
    else if (action == "hide" || action == "unhide")
    {
        request = {"HIDE", profile_, id, action == "hide" ? "1" : "0"};
        done = name + (action == "hide" ? " is hidden on this profile" : " comes back on this profile");
    }
    else
    {
        request = {"SHOW", profile_, id, action == "show" ? "1" : "0"};
        done = name + (action == "show" ? " is shown, on every profile" : " is hidden again, on every profile");
    }
    helper_.ask(request, [this, name, done](Reply &reply) {
        if (!reply.ok)
        {
            toast(name + " was not changed", reply.error, true);
            load_layout();
            return;
        }
        toast(done, "Apply changes (R2) shows it on the home screen.");
        load_layout();
        load_pending();
    }, true);
}

void App::send_row(const std::string &tab, const std::string &done)
{
    // the tiles of the row in their order, a pinned one marked
    std::vector<std::string> request = {"ROW", profile_, tab};
    for (const Tile &t : tiles_)
        if (t.tab == tab && !t.hidden && t.can('o'))
            request.push_back(t.id + (t.pin >= 0 ? "*" : ""));
    helper_.ask(request, [this, done](Reply &reply) {
        if (!reply.ok)
        {
            toast("The order was not kept", reply.error, true);
            load_layout();
            return;
        }
        toast(done, "Apply changes (R2) puts the row in this order. A pinned tile keeps its place when you play "
                    "another.");
        load_layout();
        load_pending();
    }, true);
}

void App::send_queue(const std::string &what, bool on)
{
    helper_.ask({what == "tabshifter" ? "TABSHIFTER" : "RESET", on ? "1" : "0"}, [this, what, on](Reply &reply) {
        if (!reply.ok)
        {
            toast("That was not kept", reply.error, true);
            return;
        }
        if (what == "tabshifter")
            toast(on ? "TabShifter's tile goes" : "TabShifter's tile stays",
                  on ? "Apply changes (R2) takes it out of the home screen's databases and deletes its folders."
                     : "Nothing of it is changed.");
        else
            toast(on ? "Everything Art Swap changed goes" : "Art Swap's changes stay",
                  on ? "Apply changes (R2) brings back what it hid, hides again what it showed, and removes its "
                       "tables from the home screen's databases. Moved tiles stay where they are."
                     : "Nothing is removed.");
        load_layout();
        load_pending();
    }, true);
}

void App::sort_row(const std::string &tab)
{
    // A to Z: the row's tiles in the places they had, sorted by name; a pinned tile keeps its place
    std::vector<int> slots;
    std::vector<Tile> loose;
    for (int i = 0; i < static_cast<int>(tiles_.size()); ++i)
    {
        const Tile &t = tiles_[static_cast<std::size_t>(i)];
        if (t.tab == tab && !t.hidden && t.can('o') && t.pin < 0)
        {
            slots.push_back(i);
            loose.push_back(t);
        }
    }
    if (loose.size() < 2)
    {
        toast("Nothing to sort", "The row has no tiles to put in order.", true);
        return;
    }
    std::stable_sort(loose.begin(), loose.end(),
                     [](const Tile &a, const Tile &b) { return lower(a.name) < lower(b.name); });
    for (std::size_t i = 0; i < slots.size(); ++i)
        tiles_[static_cast<std::size_t>(slots[i])] = loose[i];
    send_row(tab, std::string("The ") + tab_label(tab) + " row goes in A to Z order");
}

// ================================================================ input

void App::update_home(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    const int rows = home_row_count(home_category_);
    if (moving_)
    {
        // a tile of the row, carried with the D-pad; ✕ or □ puts it down, ○ puts it back
        const std::vector<int> list = home_rows(home_category_);
        int &row = home_row_[home_category_];
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = row + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < static_cast<int>(list.size()) &&
                tiles_[static_cast<std::size_t>(list[static_cast<std::size_t>(next)])].can('o'))
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
            const Tile *t = tile(place_id_);
            send_row(home_category_ == 1 ? "media" : "games", (t ? t->name : std::string("It")) + " takes its new place");
        }
        else if (input.is_pressed(Action::back))
        {
            moving_ = false;
            feedback.play(Cue::back);
            tiles_ = moving_before_;
            const std::vector<int> again = home_rows(home_category_);
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
            else if (input.is_pressed(Action::west) && home_category_ < 2)
            {
                // a tile of the row, not one of the hubs or libraries, which keep their own places
                const std::vector<int> list = home_rows(home_category_);
                const Tile &t = tiles_[static_cast<std::size_t>(list[static_cast<std::size_t>(row)])];
                if (!t.can('o'))
                {
                    feedback.play(Cue::error);
                    toast(t.name + " keeps its own place",
                          "The home screen puts the system's hubs at the front of the row and the library at its end.",
                          true);
                }
                else if (layout_conflict_)
                {
                    feedback.play(Cue::error);
                    toast("Lineup's changes are in the way", "Remove them in Lineup first: see Options.", true);
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
        if (layout_conflict_)
        {
            feedback.play(Cue::error);
            toast("Lineup's changes are in the way", "Remove them in Lineup first: see Options.", true);
            return;
        }
        // what it can do
        sheet_actions_.clear();
        const bool hidden = t.hidden || t.tab == "none";
        if (!hidden)
        {
            if (t.can('m'))
            {
                const std::string other = t.tab == "games" ? "media" : "games";
                sheet_actions_.push_back({std::string("Move to ") + tab_label(other), "on every profile", other});
            }
            if (t.can('o'))
                sheet_actions_.push_back(t.pin >= 0 ? TileAction{"Unpin", "playing other apps can move it", "unpin"}
                                                    : TileAction{"Pin at this place", "it keeps it as you play", "pin"});
            if (t.can('h'))
                sheet_actions_.push_back({"Hide", "on this profile only", "hide"});
            if (t.can('S'))
                sheet_actions_.push_back({"Hide again", "as the system had it, on every profile", "unshow"});
        }
        else
        {
            if (t.can('h'))
                sheet_actions_.push_back({"Bring back", "on this profile", "unhide"});
            if (t.can('s') && !t.can('S'))
                sheet_actions_.push_back({"Show it (a test)", "the system hides it; on every profile", "show"});
            if (t.can('m'))
            {
                const std::string other = t.tab == "games" ? "media" : "games";
                sheet_actions_.push_back({std::string("Move to ") + tab_label(other), "on every profile", other});
            }
        }
        if (sheet_actions_.empty())
        {
            feedback.play(Cue::error);
            toast(t.name + " stays as it is",
                  t.kind == "system" ? "The system keeps this tile where it is." : "Art Swap cannot change this tile.",
                  true);
            return;
        }
        place_id_ = t.id;
        sheet_ = Sheet::place;
        sheet_focus_ = 0;
        sheet_focus_position_.snap(0.0f);
        feedback.play(Cue::modal_open);
        return;
    }
    // the options
    if (row == 0)
    {
        // the next profile
        feedback.play(Cue::toggle);
        if (profiles_.size() < 2)
        {
            toast("One profile", "This console has one profile with a home screen.", true);
            return;
        }
        std::size_t at = 0;
        for (std::size_t i = 0; i < profiles_.size(); ++i)
            if (profiles_[i].user == profile_)
                at = i;
        profile_ = profiles_[(at + 1) % profiles_.size()].user;
        home_row_[0] = home_row_[1] = home_row_[2] = 0;
        load_layout();
    }
    else if (row == 1 || row == 2)
    {
        feedback.play(Cue::select);
        if (layout_conflict_)
            toast("Lineup's changes are in the way", "Remove them in Lineup first.", true);
        else
            sort_row(row == 1 ? "games" : "media");
    }
    else if (row == 3)
    {
        if (tabshifter_ == "none")
        {
            feedback.play(Cue::error);
            toast("No TabShifter tile", "The home screen's databases have none.", true);
            return;
        }
        feedback.play(Cue::toggle);
        send_queue("tabshifter", tabshifter_ != "queued");
    }
    else
    {
        feedback.play(Cue::toggle);
        send_queue("reset", !reset_queued_);
    }
}

void App::update_place_sheet(const hui::InputFrame &input, ui::Feedback &feedback)
{
    const int n = static_cast<int>(sheet_actions_.size());
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = std::clamp(sheet_focus_ + (input.nav == Direction::down ? 1 : -1), 0, std::max(0, n - 1));
        if (next != sheet_focus_)
        {
            sheet_focus_ = next;
            feedback.play(Cue::focus, 1.0f, 0.2f);
        }
        else if (!input.nav_repeat)
            feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
    }
    if (input.is_pressed(Action::confirm) && sheet_focus_ < n)
    {
        feedback.play(Cue::select);
        sheet_ = Sheet::none;
        change_tile(place_id_, sheet_actions_[static_cast<std::size_t>(sheet_focus_)].action);
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
        // how many tiles each place has
        const std::string count = i < 3 ? (tiles_.empty() ? "" : std::to_string(home_row_count(i))) : "";
        ui::text(list, fonts_.regular, count, r.x + r.w - 24, centred(r.cy(), 22), 22,
                 kText.with_alpha(active ? 0.75f : 0.45f), gfx::Align::right);
        list.pop_opacity();
    }
    list.rounded_rect({kMargin + 8, rail_marker_.value + 20, 4, kRailH - 40}, 2, kAccent.with_alpha(rail_in));

    // the page
    const int c = home_category_;
    const float rise = 18.0f;
    const float title_in = tween::stagger(page_age_, 0, 0.05f, 0.3f);
    std::string profile_name = profile_;
    for (const Profile &p : profiles_)
        if (p.user == profile_)
            profile_name = p.name;
    list.push_opacity(title_in * panel_in);
    static const char *titles[kCategories] = {"On the Games home", "On the Media home", "Hidden", "Options"};
    const float tw = ui::text(list, fonts_.semibold, titles[c], kPanel.x + kPanelPad + kRowPad,
                              kPanel.y + 66 + rise * (1.0f - title_in), 36, kText);
    if (!profile_name.empty())
        ui::text(list, fonts_.regular, profile_name, kPanel.x + kPanelPad + kRowPad + tw + 20,
                 kPanel.y + 66 + rise * (1.0f - title_in), 26, kText.with_alpha(0.55f));
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
                         fonts_.regular.font->fit(t.name, 27, r.w * 0.56f), icon.x + icon.w + 22,
                         centred(r.cy(), 27), 27, kText.with_alpha(focused ? 1.0f : 0.84f));
                // where it is now, when that is not where it goes; else what it is
                std::string value;
                Color ink = kText.with_alpha(focused ? 0.75f : 0.5f);
                if (carried)
                    value = "Move it with the D-pad";
                else if (t.tab != t.now || t.hidden != t.now_hidden)
                {
                    value = t.hidden != t.now_hidden ? (t.now_hidden ? "Hidden now: Apply changes"
                                                                     : "Shown now: Apply changes")
                                                     : std::string("Now on ") + tab_label(t.now) + ": Apply changes";
                    ink = kWarn.with_alpha(focused ? 1.0f : 0.8f);
                }
                else if (t.pin >= 0)
                {
                    value = "Pinned";
                    ink = kAccent.with_alpha(focused ? 1.0f : 0.85f);
                }
                else if (c == 2)
                    value = t.can('h') ? "Hidden by Art Swap" : "Hidden by the system";
                else if (t.can('S'))
                    value = "Shown by Art Swap";
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
                "Nothing is on the Games home. Move a tile here from Media.",
                "Nothing is on the Media home. Choose a tile on Games, then Move to Media: tools and apps sit well "
                "there.",
                "Nothing is hidden. Choose a tile on Games or Media, then Hide, to take it off this profile's home "
                "screen."};
            ui::paragraph(list, fonts_.regular, empty[c], kPanel.x + kPanelPad + kRowPad, kListTop + 40, 25,
                          kPanel.w - 2 * (kPanelPad + kRowPad), 38, kText.with_alpha(0.7f), 3);
        }
        static const char *about[3] = {
            "The main row, as the home screen orders it. Choose a tile to move, pin or hide it, or pick it up to "
            "change its place.",
            "The Media home, beside Games. Tools, players and stores fit here, out of the way of your games.",
            "Tiles off this profile's home screen. They stay installed: bring back what Art Swap hid, or show one the "
            "system hides."};
        description = layout_conflict_ ? "Lineup's changes are in the home screen's database, and Art Swap's would "
                                         "fight them. Open Lineup, then Options, More options, Remove pins from every "
                                         "profile, and come back."
                      : moving_        ? "Up and down carry it along the row. Put it down where you want it, or go "
                                         "back to leave it where it was."
                                       : about[c];
    }
    else
    {
        list.push_opacity(tween::stagger(age_, 4, 0.06f, 0.4f) * panel_in);
        if (home_in_rows_)
            draw_focus(list, page_focus_.value(), ui::breathe(clock_));
        list.pop_opacity();
        RowInfo rows[kOptions] = {
            {"Profile", profile_name.empty() ? std::string("The first") : profile_name},
            {"Put the Games row in A to Z order", ""},
            {"Put the Media row in A to Z order", ""},
            {"Remove TabShifter's tile", tabshifter_ == "queued"  ? "At Apply changes"
                                         : tabshifter_ == "found" ? "Found"
                                                                  : "None"},
            {"Remove everything Art Swap changed", reset_queued_ ? "At Apply changes" : ""},
        };
        rows[3].enabled = tabshifter_ != "none";
        rows[4].danger = true;
        for (int i = 0; i < kOptions; ++i)
            staggered(1 + i, [&] {
                draw_row(list, fonts_, home_row_rect(3, i), rows[i], home_in_rows_ && i == home_row_[3]);
            });
        static const char *notes[kOptions] = {
            "Each profile has its own home screen: what is hidden and the order are its own. Moving between the "
            "tabs is on every profile.",
            "Sorts the Games row by name. A pinned tile keeps its place.",
            "Sorts the Media row by name. A pinned tile keeps its place.",
            "TabShifter leaves a \"TS\" tile in the home screen's databases. Apply changes takes it out and deletes its "
            "folders, after both databases are copied aside. Do this after deleting TabShifter.",
            "Brings back what Art Swap hid, hides again what it showed, and takes its tables and triggers out of the "
            "home screen's databases at Apply changes. Moved tiles stay where they are."};
        description = layout_conflict_ ? "Lineup's changes are in the home screen's database: open Lineup, then "
                                         "Options, More options, Remove pins from every profile, and come back."
                      : home_in_rows_  ? notes[home_row_[3]]
                                       : "The profile shown, a row in A to Z order, and taking out what other tools "
                                         "or Art Swap left in the home screen's databases.";
    }
    list.push_opacity(tween::stagger(page_age_, 6, 0.05f, 0.36f) * panel_in);
    draw_description(list, fonts_, kPanel, description);
    list.pop_opacity();
}

void App::draw_place_sheet(gfx::DrawList &list, const Rect &panel, float x, float y) const
{
    const Tile *t = tile(place_id_);
    const std::string name = t ? t->name : "";
    ui::text(list, fonts_.semibold, "CHANGE", x, y, 19, kAccent, gfx::Align::left, 4.0f);
    ui::text(list, fonts_.display, fonts_.display.font->fit(name, 48, panel.w - 128), x - 2, y + 62, 48,
             Color::rgb(0xffffff));
    y += 120;
    const float row_h = 64, width = panel.w - 128;
    const int n = static_cast<int>(sheet_actions_.size());
    list.rounded_rect({x - 20, y + sheet_focus_position_.value * row_h, width + 40, row_h - 8}, 28,
                      Color::rgb(0xffffff));
    for (int i = 0; i < n; ++i)
    {
        const bool f = i == sheet_focus_;
        const float ry = y + static_cast<float>(i) * row_h;
        const Color ink = f ? Color::rgb(0x0b0d16) : Color::rgb(0xffffff, 0.9f);
        const TileAction &a = sheet_actions_[static_cast<std::size_t>(i)];
        ui::text(list, fonts_.semibold, a.label, x + 8, ry + 39, 26, ink);
        ui::text(list, fonts_.regular, a.note, x + width - 8, ry + 39, 22, ink.with_alpha(0.6f), gfx::Align::right);
    }
    y += row_h * static_cast<float>(std::max(n, 3)) + 30;
    const bool test = t && t->can('s') && !t->can('S') && (t->hidden || t->tab == "none");
    ui::paragraph(list, fonts_.regular,
                  test ? "The system hides this tile, and it may open nothing. Hide it again here if it does. The home "
                         "screen shows the change once it restarts: Apply changes (R2)."
                       : "The home screen shows it once it restarts: Apply changes (R2). From then on Art Swap keeps it "
                         "that way through restarts, rest mode and installs.",
                  x, y, 23, width, 34, Color::rgb(0xffffff, 0.7f), 3);
}

} // namespace artswap
