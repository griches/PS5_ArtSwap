/*
 * Art Swap - what the full pages share (settings.cpp, home.cpp): the kit's
 * Settings design (PS5_VKHomebrewUI's src/concepts/settings.cpp,
 * BlackBearReloaded): a header, a rail of categories with a plate that glides
 * to the active one, a panel of tiles and rows with one focus that is lit,
 * filled and ringed, and a hairline over the focused row's description. Its
 * measurements are kept; the colours are Art Swap's own indigo and violet,
 * from its icon, and stay the same whatever game is in focus.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

#include <string>

namespace artswap::page
{

namespace gfx = hui::gfx;
namespace ui = hui::ui;
using gfx::Color;
using gfx::Rect;

const Color kText = Color::rgb(0xeceaff);
const Color kAccent = Color::rgb(0xa594ff);
const Color kGood = Color::rgb(0x46d39a);
const Color kWarn = Color::rgb(0xf5b544);
const Color kDanger = Color::rgb(0xff8f8f);
const Color kDeep = Color::rgb(0x0a0b1c);    // text on bright surfaces, scrims
const Color kSurface = Color::rgb(0x17183a); // the panel
const Color kClear = Color::rgb(0x000000, 0.0f);

constexpr float kMargin = 96.0f;
constexpr float kRadius = 18.0f;
constexpr float kPanelRadius = 28.0f;
constexpr float kRailW = 372.0f;
constexpr float kRailY = 236.0f;
constexpr float kRailH = 72.0f;
constexpr float kRailPitch = 84.0f;
constexpr Rect kPanel{504.0f, 236.0f, 1320.0f, 692.0f};
constexpr float kPanelPad = 24.0f;
constexpr float kRowPad = 28.0f;
constexpr float kRowH = 72.0f;
constexpr float kRowPitch = 80.0f;

inline float centred(float cy, float size)
{
    return cy + size * 0.36f;
}

enum class Glyph
{
    key,
    games,
    about,
    media,  // the Media home: a play triangle in a screen
    hidden, // hidden tiles: an eye, struck through
    order,  // the order: three bars, shortening
};

inline void draw_rail_icon(gfx::DrawList &list, Glyph glyph, float cx, float cy, Color ink)
{
    switch (glyph)
    {
    case Glyph::key: // a key: bow, shaft, two bits
        list.ring(cx - 7, cy, 8, 2.5f, ink);
        list.line(cx + 1, cy, cx + 16, cy, 2.5f, ink);
        list.line(cx + 11, cy, cx + 11, cy + 6, 2.5f, ink);
        list.line(cx + 16, cy, cx + 16, cy + 5, 2.5f, ink);
        break;
    case Glyph::games: // a controller: body, d-pad, one button
        list.bordered_rect({cx - 17, cy - 10, 34, 20}, 10, kClear, 2.5f, ink);
        list.line(cx - 10, cy, cx - 4, cy, 2.0f, ink);
        list.line(cx - 7, cy - 3, cx - 7, cy + 3, 2.0f, ink);
        list.circle(cx + 7, cy, 2.5f, ink);
        break;
    case Glyph::about: // "i" in a circle
        list.ring(cx, cy, 16, 2.5f, ink);
        list.circle(cx, cy - 7, 2.2f, ink);
        list.line(cx, cy - 1, cx, cy + 8, 3.0f, ink);
        break;
    case Glyph::media: // a screen with a play triangle
        list.bordered_rect({cx - 17, cy - 12, 34, 24}, 5, kClear, 2.5f, ink);
        list.line(cx - 4, cy - 6, cx - 4, cy + 6, 2.5f, ink);
        list.line(cx - 4, cy - 6, cx + 6, cy, 2.5f, ink);
        list.line(cx + 6, cy, cx - 4, cy + 6, 2.5f, ink);
        break;
    case Glyph::hidden: // an eye, struck through
        list.ring(cx, cy, 7, 2.5f, ink);
        list.line(cx - 17, cy, cx - 9, cy - 7, 2.5f, ink);
        list.line(cx + 17, cy, cx + 9, cy - 7, 2.5f, ink);
        list.line(cx - 17, cy, cx - 9, cy + 7, 2.5f, ink);
        list.line(cx + 17, cy, cx + 9, cy + 7, 2.5f, ink);
        list.line(cx - 14, cy + 13, cx + 14, cy - 13, 2.5f, ink);
        break;
    case Glyph::order: // three bars, each shorter
        list.line(cx - 15, cy - 9, cx + 15, cy - 9, 2.5f, ink);
        list.line(cx - 15, cy, cx + 7, cy, 2.5f, ink);
        list.line(cx - 15, cy + 9, cx - 1, cy + 9, 2.5f, ink);
        break;
    }
}

inline void draw_chevron(gfx::DrawList &list, float x, float cy, Color ink)
{
    list.line(x - 5, cy - 8, x + 3, cy, 2.5f, ink);
    list.line(x + 3, cy, x - 5, cy + 8, 2.5f, ink);
}

// A fact: a label in small capitals, a value, a note
inline void draw_tile(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &tile, const std::string &label,
               const std::string &value, const std::string &note, Color dot = kClear)
{
    list.bordered_rect(tile, kRadius, kDeep.with_alpha(0.32f), 1.5f, kText.with_alpha(0.06f));
    const float small = tile.h < 135.0f ? 1.0f : 0.0f;
    ui::text(list, fonts.semibold, label, tile.x + kRowPad, tile.y + 40 - 4 * small, 15, kText.with_alpha(0.5f),
             gfx::Align::left, 3.0f);
    float vx = tile.x + kRowPad;
    if (dot.a > 0.0f)
    {
        list.circle(vx + 7, tile.y + 76 - 6 * small, 7, dot);
        list.glow({vx, tile.y + 69 - 6 * small, 14, 14}, 7, 10, dot.with_alpha(0.35f));
        vx += 26;
    }
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(value, 30 - 3 * small, tile.w - 2 * kRowPad), vx,
             tile.y + 86 - 7 * small, 30 - 3 * small, kText);
    ui::text(list, fonts.regular, fonts.regular.font->fit(note, 20, tile.w - 2 * kRowPad), tile.x + kRowPad,
             tile.y + 118 - 10 * small, 20, kText.with_alpha(0.55f));
}

// The one focus of a page: lit, lifted, filled and ringed
inline void draw_focus(gfx::DrawList &list, const Rect &r, float breath)
{
    list.shadow({r.x, r.y + 10, r.w, r.h}, kRadius, 24, Color::rgb(0x000000, 0.4f));
    list.glow(r, kRadius, 18, kAccent.with_alpha(0.14f + 0.1f * breath));
    list.rounded_rect(r, kRadius, gfx::mix(kSurface, kAccent, 0.17f));
    list.bordered_rect(r, kRadius, kClear, 2.0f, kAccent);
}

inline void draw_panel(gfx::DrawList &list, const Rect &panel)
{
    list.shadow({panel.x, panel.y + 18, panel.w, panel.h}, kPanelRadius, 44, Color::rgb(0x000000, 0.4f));
    list.gradient_rect(panel, kPanelRadius, kSurface.with_alpha(0.94f), gfx::mix(kSurface, kDeep, 0.35f).with_alpha(0.94f));
    list.bordered_rect(panel, kPanelRadius, kClear, 1.5f, kText.with_alpha(0.09f));
    list.rounded_rect({panel.x + kPanelPad, panel.y + panel.h - 84.0f, panel.w - 2 * kPanelPad, 1.5f}, 0,
                      kText.with_alpha(0.1f));
}

// The line under the hairline: an "i" and what the focus does
inline void draw_description(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &panel, const std::string &text)
{
    const float cy = panel.y + panel.h - 42.0f;
    const float x = panel.x + kPanelPad + kRowPad;
    list.ring(x + 10, cy, 10, 1.8f, kText.with_alpha(0.55f));
    list.circle(x + 10, cy - 4.5f, 1.4f, kText.with_alpha(0.55f));
    list.line(x + 10, cy - 1, x + 10, cy + 5, 2.0f, kText.with_alpha(0.55f));
    ui::text(list, fonts.regular, fonts.regular.font->fit(text, 23, panel.w - 2 * kPanelPad - 2 * kRowPad - 40), x + 34,
             centred(cy, 23), 23, kText.with_alpha(0.7f));
}

inline void draw_header(gfx::DrawList &list, const ui::Fonts &fonts, const std::string &title, float in)
{
    const float lift = 14.0f * (1.0f - in);
    list.push_opacity(in);
    ui::text(list, fonts.semibold, "ART SWAP", kMargin + 2, 118 - lift, 18, kAccent, gfx::Align::left, 4.0f);
    ui::text(list, fonts.display, title, kMargin - 3, 186 - lift, 64, kText);
    list.pop_opacity();
}

struct RowInfo
{
    std::string label, value;
    bool opens = false;
    bool enabled = true;
    bool danger = false;
};

inline void draw_row(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &r, const RowInfo &row, bool focused)
{
    if (!focused)
        list.rounded_rect(r, kRadius, kText.with_alpha(0.035f));
    list.push_opacity(row.enabled ? 1.0f : 0.4f);
    const Color ink = row.danger && focused ? kDanger : kText.with_alpha(focused ? 1.0f : 0.84f);
    ui::text(list, focused ? fonts.semibold : fonts.regular, row.label, r.x + kRowPad, centred(r.cy(), 27), 27, ink);
    float right = r.x + r.w - kRowPad;
    if (row.opens)
    {
        draw_chevron(list, right - 4, r.cy(), kText.with_alpha(focused ? 0.9f : 0.45f));
        right -= 34;
    }
    if (!row.value.empty())
        ui::text(list, fonts.regular, row.value, right, centred(r.cy(), 23), 23,
                 row.danger && focused ? kDanger.with_alpha(0.9f) : kText.with_alpha(focused ? 0.75f : 0.5f),
                 gfx::Align::right);
    list.pop_opacity();
}

} // namespace artswap::page
