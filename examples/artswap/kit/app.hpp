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

// Plays a clip of music on the console's output (the kit's mixer, program.cpp); null off the console
struct Speaker
{
    virtual ~Speaker() = default;
    // Interleaved stereo at 48 kHz, -1..1; the samples must stay alive until stop() or the next play()
    virtual void play(const float *samples, std::size_t frames) = 0;
    virtual void stop() = 0;
};

class App
{
  public:
    App(const hui::ui::Fonts &fonts, Helper &helper, TextureSink &textures, Keyboard *keyboard,
        Speaker *speaker = nullptr);
    ~App();

    // Between frames: answers from the helper, then texture uploads
    void pump();
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::app::Frame &frame) const;
    // The accent of the screen, for the light bar
    hui::gfx::Color accent() const;
    // Art Swap has handed a home-screen refresh to the helper and closes, so the console is free for it
    bool wants_to_close() const
    {
        return close_;
    }

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
        frame,    // full page: the part of the chosen picture kept (the picker's triangle)
        settings, // full page: a rail of categories, a panel (settings.cpp)
        connect,  // full page: adding the SteamGridDB key, the QR code showing (settings.cpp)
        music,    // the music the tile plays: a clip of a song of the person's (music.cpp)
    };
    enum class Sheet
    {
        none,
        confirm,
        working,
        search,
        log,
        restore,
        refresh, // Rename: the new name, then installing the game again so the home screen shows it
        apply,   // Apply changes: the home screen restarts to show every change
        menu,    // OPTIONS: Music, Rename, Restore originals, Settings, Close Art Swap
        music_phone,   // sending music from a phone: the QR code
        music_confirm, // the clip as the game's music?
    };
    // The order of the games on the shelf (L2)
    enum class Sort
    {
        home,    // the home screen's own order: the most recently used first (the first choice)
        name,    // A to Z
        changed, // those Art Swap changed first, then A to Z
    };
    enum class Refresh
    {
        asking,   // what ShadowMountPlus can do for the title
        ready,    // it can: the steps, Refresh or Cancel
        unable,   // it cannot: why
        naming,   // Rename: the keyboard is up for the new name
        starting, // the helper notes the game's files; then Art Swap closes
    };

    // How the chosen picture fills the file: whole on a blurred fill (fit), or the part of it kept,
    // the largest of the file's shape made zoom times smaller and placed by pan -1..1 across and down
    struct Framing
    {
        bool fit = false;
        float pan_x = 0.0f, pan_y = 0.0f, zoom = 1.0f;
        bool operator==(const Framing &other) const
        {
            return fit == other.fit && pan_x == other.pan_x && pan_y == other.pan_y && zoom == other.zoom;
        }
    };

    // ---- the titles ----
    void load_titles(bool rescan);
    void order_titles();                      // titles_ in sort_'s order
    void set_sort(Sort sort, bool keep);      // the shelf in another order, from its first game; keep: remember it
    static const char *sort_label(Sort sort);
    void request_icon(Title &title);
    void request_background(Title &title);
    Title *focused_title();
    int kept_count() const; // games whose art Art Swap changed (originals kept)
    const Title *focused_title() const;

    // ---- the picker ----
    void open_picker(int title, const std::string &slot);
    void load_current_art();
    void close_picker();
    void match_and_list();
    void set_tab(int tab);
    void load_page();
    void request_thumbs();
    void request_preview();
    std::vector<int> tab_kinds() const; // the picker's tabs, as kinds of art: Icons, Backgrounds, Logos
    std::vector<const char *> tabs() const;
    void draw_new_art(hui::gfx::DrawList &list, const hui::gfx::Rect &r, float radius) const;
    Art *focused_art();
    const Art *focused_art() const;
    std::string framing_label() const;

    // ---- framing: the frame page, and the picker's right stick ----
    void open_frame();
    void update_frame(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw_frame(hui::gfx::DrawList &list) const;
    float file_aspect() const;                 // the file's shape: 1 for an icon, 16:9 for a background
    std::vector<std::string> presets() const;  // Fit, the three places along the cut (or Whole), Custom
    Framing preset(int index) const;
    int preset_index() const;                  // framing_ among presets()
    // Moves the part kept by (du, dv) of its own size and makes it zoom times closer, about its middle
    void move_frame(float du, float dv, float zoom);
    void reframed(float delay);                // framing_ changed: a new preview after delay
    void reset_framing();

    // a logo as the home screen's page shows it: over the game's background, where its name would be
    void draw_logo_page(hui::gfx::DrawList &list, const hui::gfx::Rect &r, std::uint32_t logo, int lw, int lh,
                        float radius) const;

    // ---- actions ----
    void apply();
    void restore();
    std::string shows_after(const std::string &before, const char *what) const;
    // Apply changes: the titles changed since the home screen last restarted, and the restart itself
    void load_pending();
    void note_pending(const std::string &id, bool name = false);
    bool is_pending(const std::string &id) const;
    void open_apply();
    void start_apply();
    void check_last_home();
    void open_search(const std::string &term);
    // OPTIONS on the games: what is not a picture of the focused game
    struct MenuItem
    {
        std::string label, note;
        bool enabled = true;
    };
    std::vector<MenuItem> menu_items() const;
    void open_menu();
    void menu_select(hui::ui::Feedback &feedback);
    void choose_game(int row);
    void open_refresh(bool rename = false);
    void type_name();
    bool open_keyboard(const std::string &title, const std::string &initial, int max_length,
                       std::function<void(bool, std::string)> done);
    void rename(const std::string &name);
    void start_refresh();
    void poll_refresh(float dt);
    void check_last_refresh();
    // ---- the music: a full page (music.cpp) ----
    void open_music();
    void leave_music();
    void load_songs();
    void load_song();
    void play_clip();
    void stop_clip();
    void apply_music();
    void open_music_phone();
    void update_music(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void update_music_sheet(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw_music(hui::gfx::DrawList &list) const;
    void draw_music_sheet(hui::gfx::DrawList &list, const hui::gfx::Rect &panel, float x, float y) const;
    bool can_have_music(const Title &title) const;

    // ---- settings and adding the key: full pages (settings.cpp) ----
    void open_settings();
    void update_settings(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void settings_select(hui::ui::Feedback &feedback);
    void draw_settings(hui::gfx::DrawList &list) const;
    hui::gfx::Rect settings_rail_rect(int category) const;
    hui::gfx::Rect settings_row_rect(int category, int row) const;
    int settings_rows(int category) const;
    void open_connect(const std::string &slot);
    void update_connect(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw_connect(hui::gfx::DrawList &list) const;
    void close_phone();
    void leave_connect();
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
    // L1, a row of pills with the active one lit (the highlight springs to position), R1
    void draw_pills(hui::gfx::DrawList &list, const std::vector<std::string> &names, float position) const;
    // A card on the shelf where it is now: every card keeps its place, the selected one grows in it
    hui::gfx::Rect card_rect(int index) const;
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
    Speaker *speaker_;
    std::vector<Upload> uploads_;

    Screen screen_ = Screen::starting;
    Sheet sheet_ = Sheet::none;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float dt_ = 0.0f; // this frame's step, for the sheets
    float menu_held_ = -1.0f; // OPTIONS: a short press opens the settings, a hold leaves

    // the titles
    std::vector<std::unique_ptr<Title>> titles_;
    bool titles_loading_ = false;
    Sort sort_ = Sort::home;
    bool sort_loaded_ = false; // the order chosen last time, asked of the helper once
    std::string titles_error_;
    int focus_ = 0;
    int shown_ = -1;      // the title whose background shows
    float focus_rest_ = 0.0f;
    hui::ui::Scroller shelf_scroll_;
    std::vector<hui::tween::Spring> grow_; // each card's growth, 0 at rest, 1 selected
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
    int store_arts_ = 0;        // the store's own logo, first among the logos (not SteamGridDB's)
    bool store_asked_ = false;  // asked for it since the tab opened
    bool store_unreachable_ = false; // PlayStation Store did not answer this run: not asked again
    int hidden_ = 0;
    bool listing_ = false;
    bool matching_ = false;
    std::string list_error_;
    hui::ui::Scroller grid_scroll_;
    hui::ui::SpringRect cell_ring_;
    Framing framing_;          // the focused picture's
    Framing custom_;           // the last framing made by hand, for its pill
    bool has_custom_ = false;
    float preview_due_ = -1.0f;
    int preview_serial_ = 0;
    std::uint32_t preview_ = 0;
    std::string preview_url_;  // the picture preview_ was made from, and in which framing:
    Framing preview_framing_;  // shown only for that picture and framing, never for the next one
    int preview_w_ = 0, preview_h_ = 0;
    bool preview_loading_ = false;
    std::uint32_t current_art_ = 0; // the slot's art now, for "Now"
    int current_w_ = 0, current_h_ = 0;
    hui::tween::Spring tab_position_;
    // the frame page: the whole picture, sharper than its thumbnail
    std::uint32_t frame_picture_ = 0;
    std::string frame_url_;
    int frame_w_ = 0, frame_h_ = 0;
    hui::tween::Spring preset_position_;

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
    std::string pairing_code_;  // the connect page's code, new each time it opens
    std::string phone_address_; // where the key page is, as the helper said
    std::string phone_error_;
    float phone_poll_ = 0.0f;
    bool phone_asking_ = false;
    Screen connect_back_ = Screen::library; // where the connect page returns to
    int connect_row_ = 0;
    float page_age_ = 0.0f; // seconds since a full page (or its category) came in

    // the settings page
    Screen settings_back_ = Screen::library;
    int settings_category_ = 0;
    bool settings_in_rows_ = false;
    int settings_row_[3] = {0, 0, 0};
    hui::ui::SpringRect page_focus_; // the one focus of a full page
    hui::tween::Spring rail_marker_; // the active category's plate on the rail
    float remove_armed_ = 0.0f;      // Remove the key: a second ✕ within this many seconds removes it
    std::string key_state_; // user or none: the helper's answer, then each change
    std::string key_hint_;  // the key's last four characters
    bool store_logos_ = false;   // PlayStation Store logos, for store games (the helper's STORE)
    bool key_explained_ = false; // the explanation has shown once this run
    std::string pending_slot_;  // the picker to open once a key is in
    std::vector<Toast> toasts_;

    // the home-screen refresh
    Refresh refresh_ = Refresh::asking;
    std::string refresh_id_;
    std::string refresh_why_;   // why it cannot, or why it stopped
    std::string refresh_step_;  // what the helper is doing
    std::string smp_version_;   // ShadowMountPlus's version, empty when it did not answer
    bool smp_folder_ = false;   // ShadowMountPlus installed the title from a folder
    float refresh_poll_ = 0.0f;
    bool refresh_polling_ = false;
    bool refresh_checked_ = false; // the last refresh's end, asked for once at the start
    bool close_ = false;
    std::vector<std::string> pending_; // ids changed since the home screen last restarted (the helper's PENDING)
    std::vector<std::string> renamed_; // of those, renamed: Apply changes installs them again first
    bool applying_ = false;            // Apply changes asked the helper; Art Swap closes on its answer
    bool home_checked_ = false;        // the last restart's end, asked for once at the start
    bool keyboard_up_ = false;         // the console's keyboard is open
    bool swallow_input_ = false;       // it closed: the pad is ignored until every button is let go
    // the music page
    struct Song
    {
        std::string name;
        long long bytes = 0;
    };
    std::vector<Song> songs_;
    bool songs_loading_ = false;
    std::string songs_error_, music_folder_;
    double music_max_ = 87.0;
    int song_focus_ = 0;
    hui::tween::Spring song_focus_position_;
    std::string song_name_;          // the song SONG has described
    double song_seconds_ = 0.0;
    std::vector<int> song_levels_;   // how loud it is along its length, 0-99
    bool song_loading_ = false;
    std::string song_error_;
    float song_rest_ = 0.0f;         // seconds the focus has rested on a song, before it is read
    double clip_start_ = 0.0, clip_seconds_ = 30.0;
    std::vector<float> clip_pcm_;    // the clip playing (the speaker reads it)
    std::vector<std::vector<float>> clip_retired_; // clips stopped lately, kept a moment for the audio thread
    float clip_retire_ = 0.0f;
    bool clip_loading_ = false, clip_playing_ = false;
    int clip_serial_ = 0;
    float clip_started_ = 0.0f;      // clock_ when it began
    double clip_length_ = 0.0;       // seconds of the clip playing
    double clip_lufs_ = 0.0;
    bool music_applying_ = false;
    float songs_poll_ = 0.0f;        // the phone sheet: the list asked for again
    std::vector<std::string> songs_seen_;    // names before the phone sheet opened
    std::vector<std::string> songs_arrived_; // what the phone has sent since

    bool renaming_ = false;     // the sheet was opened by Rename: whether the title can be renamed
    std::string renamed_to_;    // the refresh follows a rename to this name
};

} // namespace artswap
