/*
 * Art Swap - the screens: the titles on the console, the art SteamGridDB
 * has for one of them, the game it is matched with, and the settings.
 *
 * The screens see only the kit's drawing types and the helper; the program
 * (../artswap.cpp) gives them textures through TextureSink, and the text
 * entry through Keyboard, so they also draw off the console (tools/preview).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "helper_client.hpp"

#include "app/concept.hpp"
#include "core/input.hpp"
#include "core/tween.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace artswap
{

constexpr const char *kVersion = "0.1.0";

struct TextureSink
{
    virtual ~TextureSink() = default;
    // RGBA, top row first; 0 on failure
    virtual std::uint32_t create(int w, int h, const std::uint8_t *rgba) = 0;
    virtual void destroy(std::uint32_t texture) = 0;
};

struct Keyboard
{
    virtual ~Keyboard() = default;
    // Opens text entry; false when it cannot. done runs later, from App::pump.
    virtual bool open(const std::string &title, const std::string &initial, int max_length,
                      std::function<void(bool ok, std::string text)> done) = 0;
    virtual void pump() = 0;
};

// The console's keyboard dialog; null off the console (keyboard.cpp)
std::unique_ptr<Keyboard> make_keyboard();

class App
{
  public:
    App(const hui::ui::Fonts &fonts, Helper &helper, TextureSink &textures, Keyboard *keyboard);
    ~App();

    // Between frames: answers from the helper, then texture uploads
    void pump();
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::app::Frame &frame) const;
    // The accent of the screen, for the light bar
    hui::gfx::Color accent() const;

  private:
    struct Title;
    struct Art;
    struct Toast
    {
        std::string title, detail;
        bool error = false;
        float age = 0.0f;
        float life = 5.0f;
    };
    enum class Screen
    {
        starting,
        library,
        picker,
    };
    enum class Sheet
    {
        none,
        confirm,
        working,
        search,
        settings,
        log,
        restore,
        phone,
        key_needed,
    };

    // ---- the titles ----
    void load_titles(bool rescan);
    void request_icon(Title &title);
    void request_background(Title &title);
    Title *focused_title();
    const Title *focused_title() const;

    // ---- the picker ----
    void open_picker(int title, const std::string &slot);
    void close_picker();
    void match_and_list();
    void set_tab(int tab);
    void load_page();
    void request_thumbs();
    void request_preview();
    std::vector<const char *> tabs() const;
    Art *focused_art();
    const Art *focused_art() const;

    // ---- actions ----
    void apply();
    void restore();
    void open_search(const std::string &term);
    void choose_game(int row);
    void open_settings();
    void settings_action(hui::ui::Feedback &feedback);
    void open_phone();
    void close_phone();
    void key_added(const std::string &how);
    // ✕ or △ on a title: the picker, or first the explanation when there is no key
    void want_picker(const std::string &slot, hui::ui::Feedback &feedback);
    void type_key();
    void type_search();
    void toast(const std::string &title, const std::string &detail, bool error = false);

    // ---- input ----
    void update_starting(const hui::InputFrame &input, hui::ui::Feedback &feedback);
    void update_library(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void update_picker(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void update_sheet(const hui::InputFrame &input, hui::ui::Feedback &feedback);

    // ---- drawing ----
    void draw_backdrop(hui::app::Frame &frame) const;
    void draw_starting(hui::gfx::DrawList &list) const;
    void draw_top_bar(hui::gfx::DrawList &list) const;
    void draw_library(hui::gfx::DrawList &list) const;
    void draw_picker(hui::gfx::DrawList &list) const;
    void draw_sheet(hui::gfx::DrawList &list, std::uint32_t glass) const;
    void draw_toasts(hui::gfx::DrawList &list) const;
    void draw_hints(hui::gfx::DrawList &list) const;
    void draw_spinner(hui::gfx::DrawList &list, float cx, float cy, float radius) const;
    hui::gfx::Rect card_rect(int index, bool focused) const;
    hui::gfx::Rect cell_rect(int index) const;
    hui::gfx::Rect cell_size() const;
    int columns() const;

    // ---- textures ----
    struct Upload
    {
        Picture picture;
        std::function<void(std::uint32_t texture, const Picture &picture)> done;
    };
    void upload(Picture picture, std::function<void(std::uint32_t, const Picture &)> done);
    void forget(std::uint32_t &texture);

    const hui::ui::Fonts &fonts_;
    Helper &helper_;
    TextureSink &textures_;
    Keyboard *keyboard_;
    std::vector<Upload> uploads_;

    Screen screen_ = Screen::starting;
    Sheet sheet_ = Sheet::none;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float menu_held_ = -1.0f; // OPTIONS: a short press opens the settings, a hold leaves

    // the titles
    std::vector<std::unique_ptr<Title>> titles_;
    bool titles_loading_ = false;
    std::string titles_error_;
    int focus_ = 0;
    int shown_ = -1;      // the title whose background shows
    float focus_rest_ = 0.0f;
    hui::ui::Scroller shelf_scroll_;
    hui::ui::SpringRect ring_;
    hui::ui::SpringColor palette_[4];
    hui::ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    hui::tween::Timer hero_;
    std::uint32_t backdrop_ = 0, backdrop_old_ = 0;
    hui::tween::Timer backdrop_fade_;

    // the picker
    int picker_title_ = -1;
    std::string slot_; // icon or pic
    int tab_ = 0;
    int generation_ = 1;
    std::vector<std::unique_ptr<Art>> arts_;
    int art_focus_ = 0;
    int page_ = 0;
    int total_ = 0;
    int hidden_ = 0;
    bool listing_ = false;
    bool matching_ = false;
    std::string list_error_;
    hui::ui::Scroller grid_scroll_;
    hui::ui::SpringRect cell_ring_;
    bool fit_ = false;  // Fill (cover) or Fit
    float pan_ = 0.0f;  // -1..1 across, cover mode
    float preview_due_ = -1.0f;
    int preview_serial_ = 0;
    std::uint32_t preview_ = 0;
    int preview_w_ = 0, preview_h_ = 0;
    bool preview_loading_ = false;
    std::uint32_t current_art_ = 0; // the slot's art now, for "Now"
    int current_w_ = 0, current_h_ = 0;
    hui::tween::Spring tab_position_;

    // the sheets
    hui::tween::Spring sheet_open_;
    std::vector<std::vector<std::string>> results_; // search: G id name year verified
    std::string search_term_;
    bool searching_ = false;
    std::string search_error_;
    int sheet_focus_ = 0;
    hui::tween::Spring sheet_focus_position_;
    std::string working_text_;
    std::string log_text_;
    std::string pairing_code_; // the phone sheet's code, new each time it opens
    std::string phone_address_; // where the key page is, as the helper said
    std::string phone_error_;
    float phone_poll_ = 0.0f;
    Sheet phone_back_ = Sheet::settings; // where ○ goes from the phone sheet
    bool phone_asking_ = false;
    std::string key_state_; // user or none: the helper's answer, then each change
    std::string key_hint_;  // the key's last four characters
    bool key_explained_ = false; // the explanation has shown once this run
    std::string pending_slot_;  // the picker to open once a key is in
    std::vector<Toast> toasts_;
};

} // namespace artswap
