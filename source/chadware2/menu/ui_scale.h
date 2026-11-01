#pragma once

#include <cstring>

#include "../../../external/imgui/imgui.h"

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// UI scale. Sizes are baked into ImGui::GetStyle() with ScaleAllSizes(); fonts scale through
// style.FontScaleMain (ImGui 1.92 re-rasterizes glyphs at the new size, so text stays sharp).
//
// The theme owns the unscaled sizes (ThemePreset::ApplyToImGui writes rounding / padding), so we keep a copy of
// the style exactly as the theme left it and always scale FROM that copy. Scaling the live style again would
// compound (0.8 * 0.8 * ...) every time the theme is re-applied.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace UiScale
{
    inline float      g_Scale = 1.0f;        // current UI scale (sizes + fonts)
    inline float      g_Applied = 1.0f;      // scale currently baked into ImGui::GetStyle()
    inline ImGuiStyle g_Unscaled{};          // style as the theme left it, before the UI scale
    inline bool       g_HasUnscaled = false;

    // Scale a hard-coded pixel size.
    inline float  S(float v) { return v * g_Scale; }
    inline ImVec2 S(float x, float y) { return ImVec2(x * g_Scale, y * g_Scale); }

    // Put the theme's unscaled sizes back into the live style. Colors, font sizing and alpha keep their live values.
    inline void RestoreUnscaled(ImGuiStyle& s)
    {
        if (!g_HasUnscaled)
            return;

        ImVec4 colors[ImGuiCol_COUNT];
        std::memcpy(colors, s.Colors, sizeof(colors));
        const float fontSizeBase = s.FontSizeBase;
        const float fontScaleMain = s.FontScaleMain;
        const float fontScaleDpi = s.FontScaleDpi;
        const float alpha = s.Alpha;

        s = g_Unscaled;

        std::memcpy(s.Colors, colors, sizeof(colors));
        s.FontSizeBase = fontSizeBase;
        s.FontScaleMain = fontScaleMain;
        s.FontScaleDpi = fontScaleDpi;
        s.Alpha = alpha;
    }

    // Remember the live style as the new unscaled reference, then bake the current UI scale into it.
    inline void CaptureAndApply(ImGuiStyle& s)
    {
        g_Unscaled = s;
        g_HasUnscaled = true;
        s.ScaleAllSizes(g_Scale);
        g_Applied = g_Scale;
    }

    // The style's unscaled values, for code that reads the theme back (e.g. saving a theme preset).
    inline const ImGuiStyle& Unscaled()
    {
        return g_HasUnscaled ? g_Unscaled : ImGui::GetStyle();
    }

    // Once per frame: re-bake the style if the scale changed.
    inline void Update(float scale)
    {
        g_Scale = scale;

        if (g_Applied == scale)
            return;

        ImGuiStyle& s = ImGui::GetStyle();
        RestoreUnscaled(s);
        CaptureAndApply(s);
    }
}
