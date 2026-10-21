/*
 * Art Swap - the program: the kit on the base class, the helper, the screens.
 *
 * Grown from the template's UI starter (examples/uiscreen): the kit draws the
 * whole screen and owns the pad; holding OPTIONS for a second leaves, a short
 * press opens the settings (app.cpp). Textures the screens ask for are made
 * here, between frames, as the kit's renderer requires.
 *
 * It lives in kit/, compiled outside the namespace the program's main file
 * is wrapped in (ps5/ui/ui.cmake), and ../artswap.cpp hands it over through
 * artswap_make_program().
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kit.hpp"

#include "app.hpp"
#include "helper_client.hpp"

#include <algorithm>
#include <memory>

#ifndef PS5_APP_ROOT
#define PS5_APP_ROOT "/app0"
#endif

namespace
{

class KitTextures final : public artswap::TextureSink
{
  public:
    explicit KitTextures(hui::gfx::VkRenderer &renderer) : renderer_(renderer)
    {
    }
    std::uint32_t create(int w, int h, const std::uint8_t *rgba) override
    {
        return renderer_.create_texture(w, h, rgba);
    }
    void destroy(std::uint32_t texture) override
    {
        renderer_.destroy_texture(texture);
    }

  private:
    hui::gfx::VkRenderer &renderer_;
};

class ArtSwapProgram : public ps5ui::KitExample
{
  public:
    std::unique_ptr<artswap::Helper> helper;
    std::unique_ptr<KitTextures> textures;
    std::unique_ptr<artswap::Keyboard> keyboard;
    std::unique_ptr<artswap::App> app;
    hui::app::Frame frame;
    hui::ui::Feedback feedback;
    hui::Settings choices;
    ps5ui::HoldToLeave leaving;
    hui::gfx::DrawList leavingList;

    ArtSwapProgram() : KitExample()
    {
        title = "Art Swap";
        name = "artswap";
        // the kit draws the whole screen and owns the pad
        settings.overlay = false;
        ps5.ownOverlay = true;
        defaultClearColor = {{0.0f, 0.0f, 0.0f, 1.0f}};
    }

    void prepare() override
    {
        VulkanExampleBase::prepare();
        prepareKit({.sound = true, .music = false, .covers = false});
        kit.apply(choices);
        helper = std::make_unique<artswap::Helper>(PS5_APP_ROOT "/artswapd.elf");
        textures = std::make_unique<KitTextures>(kit.renderer);
        keyboard = artswap::make_keyboard();
        app = std::make_unique<artswap::App>(kit.fonts, *helper, *textures, keyboard.get());
        feedback.play(hui::audio::Cue::welcome);
        prepared = true;
    }

    void render() override
    {
        if (!prepared)
            return;
        // between frames: the helper's answers, and the textures they bring
        app->pump();
        prepareFrame();
        const float dt = std::min(frameTimer, 0.05f);
        hui::InputFrame input = kit.input();
        if (ps5.frameBudget)
        {
            // a test run ignores the pad
            input = hui::InputFrame{};
            input.connected = true;
        }
        leaving.label = "Close Art Swap";
        if (!ps5.frameBudget && leaving.update(dt, (ps5_pad().held & PAD_OPTIONS) != 0))
            quit = true;
        app->update(input, dt, feedback);
        kit.play(feedback, hui::audio::SoundSet::glass, choices.haptics);
        feedback.clear();
        if (choices.light_bar)
            kit.light_bar(app->accent());
        kit.tick(dt);

        // backdrop, scene, [the glass copy], overlay: the order the kit draws in
        frame.reset();
        frame.glass_texture = kit.renderer.glass_texture();
        app->draw(frame);
        kit.renderer.begin();
        kit.renderer.backdrop(frame.backdrop);
        kit.renderer.draw(frame.scene);
        if (frame.glass)
            kit.renderer.glass();
        kit.renderer.draw(frame.overlay);
        kit.renderer.backdrop(frame.post);
        leavingList.clear();
        leaving.draw(leavingList, kit.fonts, ps5ui::active_theme());
        kit.renderer.draw(leavingList);
        buildKitCommandBuffer();
        submitFrame();
    }

    ~ArtSwapProgram() override
    {
        if (device)
            vkDeviceWaitIdle(device);
        // the screens free their textures through the renderer, which the kit still holds
        app.reset();
        helper.reset();
    }
};

} // namespace

extern "C" VulkanExampleBase *artswap_make_program()
{
    return new ArtSwapProgram();
}
