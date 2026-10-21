// Art Swap - the screens off the console, for checking them without one.
//
//   preview ASSETS_DIR OUTPUT_DIR PORT [SCRIPT]
//
// The real screens (examples/artswap/kit/app.cpp) and the real helper's host
// build (helper/, on a made-up console, talking to SteamGridDB), driven by a
// script of button presses in real time, drawn by the kit's Vulkan renderer
// on the PC's Vulkan driver (lavapipe), a PNG at each named step. The script
// is one step a line: "WAIT ACTION [NAME]" (seconds, then a press: cross,
// circle, triangle, square, l1, r1, left, right, up, down, options, rstick+,
// rstick-, none; NAME saves the picture after the wait as NAME.png).
//
// Copyright (C) 2026 Gary Riches
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vk_context.hpp"

#include "app.hpp"
#include "helper_client.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace
{

class Textures final : public artswap::TextureSink
{
  public:
    explicit Textures(hui::gfx::VkRenderer &renderer) : renderer_(renderer)
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

struct Step
{
    float wait = 0.5f;
    std::string action;
    std::string capture;
};

const char *kDefaultScript = R"(4 none library
0.5 right
0.5 right
1.5 right library-focus
6 cross picker-icon
3 r1 picker-background-tab
3 triangle picker-background-fill
1 circle
6 triangle picker-background
2 rstick+ picker-background-panned
1.5 cross confirm
5 cross after-apply
1 square restore
4 cross after-restore
1 options settings
0.5 down
0.5 down
0.5 down
2 cross log
1 circle
1 circle
6 cross picker-again
1.5 square search
1 circle
1 circle
)";

std::vector<Step> parse(const std::string &text)
{
    std::vector<Step> steps;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        Step step;
        if (words >> step.wait >> step.action)
        {
            words >> step.capture;
            steps.push_back(step);
        }
    }
    return steps;
}

std::uint32_t bit(hui::Action action)
{
    return hui::action_bit(action);
}

// The input of frame `frame` of a step whose action is `action`
hui::InputFrame input_for(const std::string &action, int frame)
{
    hui::InputFrame in;
    in.connected = true;
    if (action == "rstick+" || action == "rstick-")
    {
        in.stick2_x = action == "rstick+" ? 1.0f : -1.0f;
        return in;
    }
    if (action == "options")
    {
        if (frame == 0)
            in.pressed = in.held = bit(hui::Action::menu);
        else if (frame == 3)
            in.released = bit(hui::Action::menu);
        else if (frame < 3)
            in.held = bit(hui::Action::menu);
        return in;
    }
    if (frame != 0)
        return in;
    static const std::pair<const char *, hui::Action> buttons[] = {
        {"cross", hui::Action::confirm}, {"circle", hui::Action::back},     {"triangle", hui::Action::north},
        {"square", hui::Action::west},   {"l1", hui::Action::page_prev},    {"r1", hui::Action::page_next},
    };
    for (const auto &[name, which] : buttons)
        if (action == name)
            in.pressed = in.held = bit(which);
    if (action == "left")
        in.nav = hui::Direction::left;
    if (action == "right")
        in.nav = hui::Direction::right;
    if (action == "up")
        in.nav = hui::Direction::up;
    if (action == "down")
        in.nav = hui::Direction::down;
    return in;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s ASSETS_DIR OUTPUT_DIR PORT [SCRIPT]\n", argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const int port = std::atoi(argv[3]);
    std::string script = kDefaultScript;
    if (argc > 4)
    {
        std::ifstream file(argv[4]);
        script.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    const int width = 1920, height = 1080;

    Context context;
    if (!open_context(context))
    {
        std::fprintf(stderr, "no Vulkan device with a graphics queue\n");
        return 1;
    }
    std::fprintf(stderr, "Vulkan device: %s\n", context.name.c_str());
    Target target;
    if (!create_target(context, target, width, height))
        return 1;
    int status = 0;
    {
        hui::gfx::VkRenderer renderer;
        hui::gfx::VkRendererConfig config;
        config.physical_device = context.physical;
        config.device = context.device;
        config.queue = context.queue;
        config.queue_family = context.family;
        config.frames_in_flight = 1;
        config.render_pass = target.pass;
        hui::gfx::Font faces[6];
        hui::ui::Fonts fonts;
        if (!renderer.init(config) ||
            !load_font(renderer, assets + "/fonts/inter-regular.huifont", &faces[0], &fonts.regular) ||
            !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &faces[1], &fonts.semibold) ||
            !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", &faces[2], &fonts.display) ||
            !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &faces[3], &fonts.mono) ||
            !load_font(renderer, assets + "/fonts/press-start-2p.huifont", &faces[4], &fonts.pixel) ||
            !load_font(renderer, assets + "/fonts/patrick-hand.huifont", &faces[5], &fonts.hand))
        {
            std::fprintf(stderr, "renderer or fonts failed\n");
            return 1;
        }
        hui::save::ensure_directory(output);
        Textures textures(renderer);
        {
            artswap::Helper helper("/nonexistent/artswapd.elf", port, 1);
            artswap::App app(fonts, helper, textures, nullptr);
            hui::app::Frame frame;
            hui::ui::Feedback feedback;
            constexpr float kDt = 1.0f / 60.0f;
            bool ok = true;
            auto capture = [&](const std::string &name) {
                frame.reset();
                frame.glass_texture = renderer.glass_texture();
                app.draw(frame);
                renderer.begin();
                renderer.backdrop(frame.backdrop);
                renderer.draw(frame.scene);
                if (frame.glass)
                    renderer.glass();
                renderer.draw(frame.overlay);
                renderer.backdrop(frame.post);
                const bool drawn = present(context, target, renderer);
                std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
                std::memcpy(pixels.data(), target.pixels, pixels.size());
                for (std::size_t i = 3; i < pixels.size(); i += 4)
                    pixels[i] = 255;
                const std::string path = output + "/" + name + ".png";
                ok = drawn && stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
                std::fprintf(stderr, "wrote %s\n", path.c_str());
            };
            for (const Step &step : parse(script))
            {
                const int frames = std::max(1, static_cast<int>(step.wait * 60.0f));
                for (int f = 0; f < frames; ++f)
                {
                    const auto start = std::chrono::steady_clock::now();
                    app.pump();
                    app.update(input_for(step.action, f), kDt, feedback);
                    feedback.clear();
                    std::this_thread::sleep_until(start + std::chrono::microseconds(16667));
                }
                if (!step.capture.empty())
                    capture(step.capture);
            }
            status = ok ? 0 : 1;
            vkDeviceWaitIdle(context.device);
        }
        renderer.release();
    }
    destroy_target(context, target);
    vkDestroyDevice(context.device, nullptr);
    vkDestroyInstance(context.instance, nullptr);
    return status;
}
