/*
 * Art Swap - a title on the console, as the screens hold it (app.cpp, music.cpp).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "app.hpp"

namespace artswap
{

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
    long recent = 0;        // its place in the home screen's order: higher was used more recently
    std::string music = "-"; // the music its tile plays ("at9 30s x2"), or -
    bool music_added = false; // Art Swap added it (it had none): Restore deletes it
    // a background to change, or one Art Swap can add (ShadowMountPlus installs the game again for it)
    bool has_pic() const
    {
        return !(pic_desc == "-" || pic_desc.empty());
    }
    long sgdb_id = 0;
    // changed by Art Swap: art (originals kept), a name or a logo, so Restore has something to put back
    bool changed() const
    {
        return kept || name_kept || logo == "art" || music_added;
    }
    bool has_music() const
    {
        return !(music == "-" || music.empty());
    }
    std::uint32_t icon = 0;
    bool icon_asked = false, icon_failed = false;
    std::uint32_t background = 0;
    int background_w = 0, background_h = 0;
    bool background_asked = false, background_failed = false;
    float background_used = 0.0f;
    hui::gfx::Color dark = hui::gfx::Color::rgb(0x0e121c), mid = hui::gfx::Color::rgb(0x27324a),
                    accent = hui::gfx::Color::rgb(0x9fd8ff);
};

} // namespace artswap
