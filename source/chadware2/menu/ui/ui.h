#pragma once

#include <functional>
#include <span>
#include <string>

#include "../../../../external/imgui/imgui.h"

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Unboxer's own UI: one fixed look (dark, gold accent, rounded), no themes. Everything is drawn here; the pages only
// lay things out. Render thread only.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace UI
{
    namespace Col
    {
        inline constexpr ImU32 Background = IM_COL32(13, 14, 18, 255);
        inline constexpr ImU32 Header = IM_COL32(18, 19, 25, 255);
        inline constexpr ImU32 Panel = IM_COL32(21, 23, 29, 255);
        inline constexpr ImU32 Raised = IM_COL32(29, 32, 40, 255);
        inline constexpr ImU32 Hover = IM_COL32(38, 42, 52, 255);
        inline constexpr ImU32 Border = IM_COL32(43, 47, 58, 255);
        inline constexpr ImU32 Text = IM_COL32(236, 237, 241, 255);
        inline constexpr ImU32 TextDim = IM_COL32(136, 142, 156, 255);
        inline constexpr ImU32 TextFaint = IM_COL32(92, 97, 110, 255);
        // the accent (selected tab, primary buttons, switches, sliders...): gold, or the user's own - Settings -> Menu ->
        // "Menu colour" sets it every frame through SetAccent (render thread)
        inline constexpr ImU32 DefaultAccent = IM_COL32(236, 180, 60, 255);
        inline ImU32 Accent = DefaultAccent;
        inline ImU32 AccentHot = IM_COL32(250, 202, 96, 255);   // a lighter accent (hover)
        inline ImU32 OnAccent = IM_COL32(28, 20, 6, 255);       // text on the accent: dark on a light one, light on a dark one
        inline constexpr ImU32 Danger = IM_COL32(230, 76, 80, 255);
        inline constexpr ImU32 Success = IM_COL32(70, 206, 128, 255);
        inline constexpr ImU32 Warning = IM_COL32(242, 152, 62, 255);
    }

    ImU32 WithAlpha(ImU32 color, float alpha);

    // fonts (Segoe UI text + Material Symbols icons merged in, Bahnschrift headings; Outfit if those are missing)
    inline ImFont* FontText = nullptr;
    inline ImFont* FontSemibold = nullptr;
    inline ImFont* FontHeading = nullptr;

    void LoadFonts();
    void ApplyStyle();    // colours / rounding of the few stock ImGui widgets still used (text inputs, combos, popups)
    // The accent colour (opaque): Col::Accent / AccentHot / OnAccent and the stock widgets' accent colours. Same colour = nothing.
    void SetAccent(ImU32 color);

    // ── layout ──
    // A rounded panel. height 0 = as tall as its content. title: small caps line at the top (already translated).
    bool BeginPanel(const char* id, const ImVec2& size = ImVec2(0.0f, 0.0f), const char* title = nullptr, bool scroll = false);
    void EndPanel();
    void SectionTitle(const char* text);              // caps, dim, with a line — inside a panel
    // A tip (grey, wrapped): only while Settings -> Menu -> "Show tips" is on (ShowTips).
    void Hint(const char* text);
    // The same look, always shown: what tells you the state of things (loading, nothing found...).
    void Note(const char* text);
    inline bool ShowTips = false;   // Config::menu.showTips, set by Menu::render every frame
    void Gap(float pixels = 8.0f);

    // ── controls (labels are translated here; "##id" suffixes are hidden) ──
    enum class Kind { Primary, Secondary, Danger, Ghost };
    bool Button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f), Kind kind = Kind::Secondary, bool enabled = true);
    bool IconButton(const char* id, ImWchar icon, float size, const char* tooltip = nullptr);
    bool Toggle(const char* label, bool* value, bool enabled = true);
    bool Segmented(const char* id, std::span<const char* const> options, int* selected);
    bool Slider(const char* id, const char* label, float* value, float minValue, float maxValue, const char* valueText);
    bool Stepper(const char* id, const char* label, int* value, int minValue, int maxValue);
    // The same for a float (a skin's float): label left, [- value +] right; click the number and type any value.
    bool FloatField(const char* id, const char* label, float* value, float minValue, float maxValue, float step, const char* format);
    // A toggle row with a small key button left of its switch ("F6"): returns the toggle's click, keyClicked the key's.
    bool ToggleKey(const char* label, bool* value, const char* keyText, bool keyActive, bool* keyClicked);
    bool Search(const char* id, const char* hint, std::string* text);
    void Status(bool ok, const char* text, const char* tooltip = nullptr);
    void Chip(const char* text, ImU32 color);

    // ── item cards ──
    struct CardImage
    {
        ImTextureID texture = 0;
        float width = 0.0f;
        float height = 0.0f;
        ImU32 tint = IM_COL32_WHITE;   // multiplied in (a coloured graffiti's white picture)
    };

    struct Grid
    {
        int columns = 1;
        float width = 0.0f;
        int index = 0;
    };

    // The cards' size on the tab being drawn (each tab has its own - CardSizeRow, the sliders above its cards): the
    // pictures (how wide a card is: MakeGrid / MakeGridFitting) and the text on the cards. 1 = the usual. The menu sets
    // them before each page; render thread.
    inline float CardPictureScale = 1.0f;
    inline float CardTextScale = 1.0f;
    // Those two sliders on one slim line at the right ("Pictures 100%  Text size 100%  reset"). True = changed.
    bool CardSizeRow(const char* id, float* picture, float* text);

    Grid MakeGrid(float minWidth);
    // A card's height for a width (Card's layout).
    [[nodiscard]] float CardHeight(float width);
    // A grid for `count` cards: as big as they can get (up to maxWidth) while all of them still fit in the space left
    // below; a long list keeps minWidth cards.
    Grid MakeGridFitting(int count, float minWidth, float maxWidth);
    void NextCell(Grid& grid);

    // The picture is asked for only when the card is on screen. rarity 0 = no colour strip. badge: a small tag in the
    // picture's corner (already translated), e.g. "IN CASE".
    bool Card(const char* id, float width, const std::string& title, const std::string& subtitle, ImU32 rarity, bool selected,
        const std::function<CardImage()>& image, const char* badge = nullptr, ImU32 badgeColor = Col::Success);

    void DrawImage(ImDrawList* draw, const CardImage& image, const ImVec2& min, const ImVec2& max);
    // The backdrop behind an item, like CS2's loadout card: the rarity colour strong on the left fading into `base` (the
    // colour under it) on the right, a soft light from the top left and a faint diagonal mesh. strength 0..1.
    void RarityBackdrop(ImDrawList* draw, const ImVec2& min, const ImVec2& max, ImU32 rarity, ImU32 base, float strength, float rounding,
        ImDrawFlags corners = ImDrawFlags_RoundCornersAll);
    std::string Fit(const char* text, ImFont* font, float size, float width);   // "Long name..." within width

    // ── top bar ──
    void Logo(ImDrawList* draw, const ImVec2& min, float size);
    bool TopTab(const char* label, ImWchar icon, bool selected, float height);

    // ── pop-ups (Notify) ──
    ImVec2 ToastSize(const char* text, float wrapWidth);
    void Toast(ImDrawList* draw, const ImVec2& min, const ImVec2& max, const char* text, float wrapWidth, ImU32 accent, float alpha);
}
