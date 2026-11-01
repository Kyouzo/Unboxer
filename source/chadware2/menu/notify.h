#pragma once

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>

#include "../../../external/imgui/imgui.h"
#include "../config/config.h"
#include "ui_scale.h"
#include "ui/ui.h"

// Small pop-ups at the right side of the screen ("Added: Kilowatt Case x1", ...).
// Push() is safe from any thread; Render() runs on the render thread every frame, menu open or not.
namespace Notify
{
    struct Entry
    {
        std::string text;
        ImU32 accent = 0;
        double startedAt = -1.0;   // set on the first rendered frame
    };

    inline std::mutex g_mutex;
    inline std::deque<Entry> g_entries;

    inline constexpr double LIFETIME = 3.2;   // seconds on screen
    inline constexpr double FADE = 0.25;      // fade in / out
    inline constexpr std::size_t MAX_VISIBLE = 5;

    // accent 0 = the gold accent
    inline void Push(std::string text, ImU32 accent = 0)
    {
        if (!Config::menu.notifications || text.empty())
            return;

        std::lock_guard lock(g_mutex);

        // the same message twice in a row just restarts its timer
        if (!g_entries.empty() && g_entries.back().text == text)
        {
            g_entries.back().startedAt = -1.0;
            return;
        }

        g_entries.push_back({ std::move(text), accent, -1.0 });

        while (g_entries.size() > MAX_VISIBLE)
            g_entries.pop_front();
    }

    inline void Render()
    {
        std::lock_guard lock(g_mutex);

        if (g_entries.empty())
            return;

        const double now = ImGui::GetTime();
        const ImGuiIO& io = ImGui::GetIO();
        ImDrawList* draw = ImGui::GetForegroundDrawList();

        const float gap = UiScale::S(6.0f);
        const float maxTextWidth = UiScale::S(320.0f);
        const float right = io.DisplaySize.x - UiScale::S(18.0f);
        float y = io.DisplaySize.y * 0.18f;

        for (auto it = g_entries.begin(); it != g_entries.end();)
        {
            if (it->startedAt < 0.0)
                it->startedAt = now;

            const double age = now - it->startedAt;

            if (age > LIFETIME)
            {
                it = g_entries.erase(it);
                continue;
            }

            const float alpha = static_cast<float>((std::min)({ 1.0, age / FADE, (LIFETIME - age) / FADE }));
            const float slide = (1.0f - (std::min)(1.0f, static_cast<float>(age / FADE))) * UiScale::S(24.0f);

            const ImVec2 size = UI::ToastSize(it->text.c_str(), maxTextWidth);
            const ImVec2 min(right - size.x + slide, y);
            const ImVec2 max(right + slide, y + size.y);

            UI::Toast(draw, min, max, it->text.c_str(), maxTextWidth, it->accent, alpha);

            y += size.y + gap;
            ++it;
        }
    }
}
