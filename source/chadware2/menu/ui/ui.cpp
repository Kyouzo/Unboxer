#include "ui.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../menu.h"
#include "../../../../external/imgui/imgui_stdlib.h"

using UiScale::S;

namespace
{
    // "Add to Inventory##add" -> the translated "Add to Inventory"
    std::string Visible(const char* label)
    {
        if (!label)
            return {};

        const char* end = std::strstr(label, "##");
        const std::string text = end ? std::string(label, end) : std::string(label);
        return tr(text.c_str());   // copied before `text` goes away
    }

    ImVec2 TextSize(ImFont* font, float size, const char* text)
    {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    }

    ImVec2 Add(const ImVec2& a, const ImVec2& b)
    {
        return ImVec2(a.x + b.x, a.y + b.y);
    }

    float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    ImU32 Mix(ImU32 a, ImU32 b, float t)
    {
        const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
        const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(Lerp(x.x, y.x, t), Lerp(x.y, y.y, t), Lerp(x.z, y.z, t), Lerp(x.w, y.w, t)));
    }

    // Smoothly moves a stored value toward `target` (hover / toggle animations).
    float Animate(ImGuiID id, float target, float speed = 14.0f)
    {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        float value = storage->GetFloat(id, target);
        value = Lerp(value, target, std::min(1.0f, ImGui::GetIO().DeltaTime * speed));

        if (std::fabs(value - target) < 0.002f)
            value = target;

        storage->SetFloat(id, value);
        return value;
    }

    void Star(ImDrawList* draw, const ImVec2& center, float radius, ImU32 color)
    {
        ImVec2 points[10];

        for (int i = 0; i < 10; ++i)
        {
            constexpr float PI = 3.14159265358979f;
            const float angle = -PI * 0.5f + i * PI / 5.0f;
            const float r = (i % 2 == 0) ? radius : radius * 0.43f;
            points[i] = ImVec2(center.x + std::cos(angle) * r, center.y + std::sin(angle) * r);
        }

        draw->AddConcavePolyFilled(points, 10, color);
    }

    std::string FontPath(const char* file)
    {
        char windows[MAX_PATH]{};

        if (!GetWindowsDirectoryA(windows, MAX_PATH))
            return {};

        std::string path = std::string(windows) + "\\Fonts\\" + file;
        return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : std::string();
    }

    // Latin-1, Cyrillic, dashes / quotes, ™, arrows, ● ○, ★
    const ImWchar* TextRanges()
    {
        static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x2027, 0x2122, 0x2122, 0x2190, 0x2193,
            0x25CB, 0x25CF, 0x2605, 0x2606, 0 };
        return ranges;
    }

    ImFont* AddTextFont(ImGuiIO& io, const char* file, bool bold, float size, bool icons)
    {
        ImFont* font = nullptr;
        const std::string path = FontPath(file);

        if (!path.empty())
        {
            ImFontConfig cfg{};
            cfg.OversampleH = cfg.OversampleV = 2;
            snprintf(cfg.Name, sizeof(cfg.Name), "%s, %.0fpx", file, size);
            cfg.GlyphExcludeRanges = PrivateUseRange();   // the icons (merged below) own that range
            font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg, TextRanges());
        }

        if (!font)   // no Windows font: the built-in Outfit
            font = bold ? AddOutfitBold(io, size, false, size, "Outfit Bold") : AddOutfitRegular(io, size, false, size, "Outfit Regular");

        MergeCyrillic(io, font, size, bold);   // Cyrillic for Outfit; ★ ™ ● ○ from Segoe UI Symbol for all

        if (icons)
            MergeMaterialSymbols(io, font, size);

        return font;
    }
}

ImU32 UI::WithAlpha(ImU32 color, float alpha)
{
    const ImU32 a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * ((color >> IM_COL32_A_SHIFT) & 0xFF));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// fonts / style
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void UI::LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    constexpr float TEXT = 17.0f;
    FontText = AddTextFont(io, "segoeui.ttf", false, TEXT, true);
    FontSemibold = AddTextFont(io, "seguisb.ttf", true, TEXT, true);
    FontHeading = AddTextFont(io, "bahnschrift.ttf", true, 22.0f, false);

    if (!FontHeading)
        FontHeading = FontSemibold;

    // what the rest of the code (icons, image helpers) knows them as
    g_FontRegular = FontText;
    g_FontBold = FontSemibold;
    g_FontContentRegular = FontText;
    g_FontContentBold = FontSemibold;
    g_FontIcons = nullptr;
    g_FontIconFallback = FontText;
    ++g_FontGeneration;

    io.FontDefault = FontText;
}

void UI::ApplyStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.WindowRounding = 14.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ScrollbarSize = 9.0f;
    style.WindowMinSize = ImVec2(200.0f, 150.0f);

    ImVec4* c = style.Colors;
    const auto v = [](ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); };

    c[ImGuiCol_Text] = v(Col::Text);
    c[ImGuiCol_TextDisabled] = v(Col::TextDim);
    c[ImGuiCol_WindowBg] = v(Col::Background);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v(Col::Raised);
    c[ImGuiCol_Border] = v(Col::Border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v(Col::Raised);
    c[ImGuiCol_FrameBgHovered] = v(Col::Hover);
    c[ImGuiCol_FrameBgActive] = v(Col::Hover);
    c[ImGuiCol_TitleBg] = v(Col::Header);
    c[ImGuiCol_TitleBgActive] = v(Col::Header);
    c[ImGuiCol_TitleBgCollapsed] = v(Col::Header);
    c[ImGuiCol_MenuBarBg] = v(Col::Header);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v(Col::Border);
    c[ImGuiCol_ScrollbarGrabHovered] = v(Col::Hover);
    c[ImGuiCol_ScrollbarGrabActive] = v(Col::Accent);
    c[ImGuiCol_CheckMark] = v(Col::Accent);
    c[ImGuiCol_SliderGrab] = v(Col::Accent);
    c[ImGuiCol_SliderGrabActive] = v(Col::AccentHot);
    c[ImGuiCol_Button] = v(Col::Raised);
    c[ImGuiCol_ButtonHovered] = v(Col::Hover);
    c[ImGuiCol_ButtonActive] = v(Col::Border);
    c[ImGuiCol_Header] = v(WithAlpha(Col::Accent, 0.22f));
    c[ImGuiCol_HeaderHovered] = v(Col::Hover);
    c[ImGuiCol_HeaderActive] = v(WithAlpha(Col::Accent, 0.35f));
    c[ImGuiCol_Separator] = v(Col::Border);
    c[ImGuiCol_SeparatorHovered] = v(Col::Accent);
    c[ImGuiCol_SeparatorActive] = v(Col::Accent);
    c[ImGuiCol_ResizeGrip] = v(WithAlpha(Col::Accent, 0.25f));
    c[ImGuiCol_ResizeGripHovered] = v(WithAlpha(Col::Accent, 0.6f));
    c[ImGuiCol_ResizeGripActive] = v(Col::Accent);
    c[ImGuiCol_TextSelectedBg] = v(WithAlpha(Col::Accent, 0.35f));
    c[ImGuiCol_NavHighlight] = v(Col::Accent);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}

void UI::SetAccent(ImU32 color)
{
    color |= IM_COL32_A_MASK;

    if (color == Col::Accent)
        return;

    Col::Accent = color;
    Col::AccentHot = Mix(color, IM_COL32(255, 255, 255, 255), 0.22f);

    // text on it: dark on a light accent (the gold), light on a dark one
    const ImVec4 rgb = ImGui::ColorConvertU32ToFloat4(color);
    const float luminance = 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
    Col::OnAccent = luminance > 0.5f ? IM_COL32(28, 20, 6, 255) : IM_COL32(250, 250, 252, 255);

    // the stock widgets' accent colours (ApplyStyle's), without touching the rest of the style (it's scaled)
    if (!ImGui::GetCurrentContext())
        return;

    ImVec4* c = ImGui::GetStyle().Colors;
    const auto v = [](ImU32 value) { return ImGui::ColorConvertU32ToFloat4(value); };
    c[ImGuiCol_ScrollbarGrabActive] = v(Col::Accent);
    c[ImGuiCol_CheckMark] = v(Col::Accent);
    c[ImGuiCol_SliderGrab] = v(Col::Accent);
    c[ImGuiCol_SliderGrabActive] = v(Col::AccentHot);
    c[ImGuiCol_Header] = v(WithAlpha(Col::Accent, 0.22f));
    c[ImGuiCol_HeaderActive] = v(WithAlpha(Col::Accent, 0.35f));
    c[ImGuiCol_SeparatorHovered] = v(Col::Accent);
    c[ImGuiCol_SeparatorActive] = v(Col::Accent);
    c[ImGuiCol_ResizeGrip] = v(WithAlpha(Col::Accent, 0.25f));
    c[ImGuiCol_ResizeGripHovered] = v(WithAlpha(Col::Accent, 0.6f));
    c[ImGuiCol_ResizeGripActive] = v(Col::Accent);
    c[ImGuiCol_TextSelectedBg] = v(WithAlpha(Col::Accent, 0.35f));
    c[ImGuiCol_NavHighlight] = v(Col::Accent);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// layout
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

bool UI::BeginPanel(const char* id, const ImVec2& size, const char* title, bool scroll)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, S(8.0f, 9.0f));

    ImGuiChildFlags flags = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (size.y == 0.0f)
        flags |= ImGuiChildFlags_AutoResizeY;

    const ImGuiWindowFlags windowFlags = scroll ? ImGuiWindowFlags_None : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const bool open = ImGui::BeginChild(id, size, flags, windowFlags);
    ImGui::PopStyleVar(2);

    if (open && title && *title)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float size12 = ImGui::GetFontSize() * 0.78f;
        const std::string text = Visible(title);

        draw->AddRectFilled(ImVec2(pos.x, pos.y + S(2.0f)), ImVec2(pos.x + S(3.0f), pos.y + size12 + S(2.0f)), Col::Accent, S(2.0f));
        draw->AddText(FontSemibold, size12, ImVec2(pos.x + S(10.0f), pos.y + S(1.0f)), Col::TextDim, text.c_str());
        ImGui::Dummy(ImVec2(1.0f, size12 + S(8.0f)));
    }

    return open;
}

void UI::EndPanel()
{
    ImGui::EndChild();

    // drawn into the parent: the child window renders on top of it
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, Col::Panel, S(12.0f));
    draw->AddRect(min, max, Col::Border, S(12.0f));
}

void UI::SectionTitle(const char* text)
{
    Gap(4.0f);
    const std::string label = Visible(text);
    const float size = ImGui::GetFontSize() * 0.78f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImVec2 textSize = TextSize(FontSemibold, size, label.c_str());
    draw->AddText(FontSemibold, size, pos, Col::TextDim, label.c_str());
    const float lineY = std::floor(pos.y + textSize.y * 0.55f);
    draw->AddLine(ImVec2(pos.x + textSize.x + S(10.0f), lineY), ImVec2(pos.x + width, lineY), Col::Border);
    ImGui::Dummy(ImVec2(width, textSize.y + S(4.0f)));
}

void UI::Hint(const char* text)
{
    if (ShowTips)
        Note(text);
}

void UI::Note(const char* text)
{
    const std::string label = Visible(text);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::TextDim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void UI::Gap(float pixels)
{
    ImGui::Dummy(ImVec2(1.0f, S(pixels)));
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// controls
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

bool UI::Button(const char* label, const ImVec2& size, Kind kind, bool enabled)
{
    const std::string text = Visible(label);
    ImFont* font = kind == Kind::Primary ? FontSemibold : FontText;
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 textSize = TextSize(font, fontSize, text.c_str());

    ImVec2 box = size;
    if (box.x == 0.0f)
        box.x = textSize.x + S(30.0f);
    else if (box.x < 0.0f)
        box.x = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    if (box.y == 0.0f)
        box.y = S(36.0f);

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, box);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float hover = Animate(ImGui::GetItemID(), hovered && enabled ? 1.0f : 0.0f);
    const float rounding = S(8.0f);
    const float alpha = enabled ? 1.0f : 0.4f;

    ImU32 fill = 0, border = 0, textColor = Col::Text;

    switch (kind)
    {
    case Kind::Primary:
        fill = Mix(Col::Accent, Col::AccentHot, hover);
        textColor = Col::OnAccent;
        break;
    case Kind::Secondary:
        fill = Mix(Col::Raised, Col::Hover, hover);
        border = Col::Border;
        break;
    case Kind::Danger:
        fill = WithAlpha(Col::Danger, 0.12f + 0.16f * hover);
        border = WithAlpha(Col::Danger, 0.8f);
        textColor = Mix(Col::Danger, Col::Text, hover * 0.5f);
        break;
    case Kind::Ghost:
        fill = WithAlpha(Col::Hover, hover);
        textColor = Mix(Col::TextDim, Col::Text, hover);
        break;
    }

    const ImVec2 offset(0.0f, held ? S(1.0f) : 0.0f);
    if (fill)
        draw->AddRectFilled(Add(min, offset), Add(max, offset), WithAlpha(fill, alpha), rounding);
    if (border)
        draw->AddRect(Add(min, offset), Add(max, offset), WithAlpha(border, alpha), rounding);

    const ImVec2 textPos(std::floor(min.x + (box.x - textSize.x) * 0.5f), std::floor(min.y + (box.y - textSize.y) * 0.5f));
    draw->AddText(font, fontSize, Add(textPos, offset), WithAlpha(textColor, alpha), text.c_str());

    return clicked && enabled;
}

bool UI::IconButton(const char* id, ImWchar icon, float size, const char* tooltip)
{
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(min, max, WithAlpha(Col::Hover, hover), S(8.0f));
    DrawCenteredIcon(draw, FontText, ImGui::GetFontSize() * 1.15f, { icon }, min, max, Mix(Col::TextDim, Col::Text, hover));

    if (hovered && tooltip)
        ImGui::SetTooltip("%s", Visible(tooltip).c_str());

    return clicked;
}

bool UI::Toggle(const char* label, bool* value, bool enabled)
{
    const std::string text = Visible(label);
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = S(30.0f);

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();

    if (clicked && enabled)
        *value = !*value;

    const ImVec2 min = ImGui::GetItemRectMin();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float on = Animate(ImGui::GetItemID(), *value ? 1.0f : 0.0f);
    const float alpha = enabled ? 1.0f : 0.4f;

    const float textY = min.y + (height - ImGui::GetFontSize()) * 0.5f;
    draw->AddText(ImVec2(min.x, textY), WithAlpha(hovered ? Col::Text : Mix(Col::TextDim, Col::Text, 0.75f), alpha), text.c_str());

    const float trackW = S(40.0f), trackH = S(22.0f);
    const ImVec2 trackMin(min.x + width - trackW, min.y + (height - trackH) * 0.5f);
    const ImVec2 trackMax(trackMin.x + trackW, trackMin.y + trackH);
    draw->AddRectFilled(trackMin, trackMax, WithAlpha(Mix(Col::Border, Col::Accent, on), alpha), trackH * 0.5f);

    const float knobR = trackH * 0.5f - S(3.0f);
    const float knobX = Lerp(trackMin.x + trackH * 0.5f, trackMax.x - trackH * 0.5f, on);
    draw->AddCircleFilled(ImVec2(knobX, trackMin.y + trackH * 0.5f), knobR, WithAlpha(Mix(Col::Text, Col::OnAccent, on * 0.15f), alpha));

    return clicked && enabled;
}

bool UI::ToggleKey(const char* label, bool* value, const char* keyText, bool keyActive, bool* keyClicked)
{
    ImGui::PushID(label);
    const std::string text = Visible(label);
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = S(30.0f);
    const float trackW = S(40.0f), trackH = S(22.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    // the key button: left of the switch, as wide as its text
    ImGui::PushFont(FontSemibold, 0.0f);
    const float keySize = ImGui::GetFontSize() * 0.85f;
    const ImVec2 keyTextSize = TextSize(FontSemibold, keySize, keyText ? keyText : "");
    ImGui::PopFont();
    const float keyW = std::max(S(46.0f), keyTextSize.x + S(20.0f));
    const float keyH = S(24.0f);
    const float keyX = pos.x + width - trackW - S(10.0f) - keyW;

    // the label part and the switch toggle; the key part is its own button
    ImGui::SetCursorScreenPos(pos);
    const bool clickedLabel = ImGui::InvisibleButton("##toggle_label", ImVec2(std::max(1.0f, keyX - S(6.0f) - pos.x), height));
    const bool hoveredLabel = ImGui::IsItemHovered();

    ImGui::SetCursorScreenPos(ImVec2(pos.x + width - trackW - S(4.0f), pos.y));
    const bool clickedSwitch = ImGui::InvisibleButton("##toggle_switch", ImVec2(trackW + S(4.0f), height));
    const bool hoveredSwitch = ImGui::IsItemHovered();
    const ImGuiID switchId = ImGui::GetItemID();

    ImGui::SetCursorScreenPos(ImVec2(keyX, pos.y + (height - keyH) * 0.5f));
    const bool clickedKey = ImGui::InvisibleButton("##toggle_key", ImVec2(keyW, keyH));
    const bool hoveredKey = ImGui::IsItemHovered();

    if (keyClicked)
        *keyClicked = clickedKey;

    const bool clicked = clickedLabel || clickedSwitch;
    if (clicked)
        *value = !*value;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float on = Animate(switchId, *value ? 1.0f : 0.0f);
    const bool hovered = hoveredLabel || hoveredSwitch;

    draw->AddText(ImVec2(pos.x, pos.y + (height - ImGui::GetFontSize()) * 0.5f), hovered ? Col::Text : Mix(Col::TextDim, Col::Text, 0.75f), text.c_str());

    // the key: a small chip (gold while it waits for a key)
    const ImVec2 keyMin(keyX, pos.y + (height - keyH) * 0.5f);
    const ImVec2 keyMax(keyMin.x + keyW, keyMin.y + keyH);
    draw->AddRectFilled(keyMin, keyMax, keyActive ? Col::Accent : (hoveredKey ? Col::Hover : Col::Raised), S(6.0f));
    draw->AddText(FontSemibold, keySize, ImVec2(keyMin.x + (keyW - keyTextSize.x) * 0.5f, keyMin.y + (keyH - keyTextSize.y) * 0.5f),
        keyActive ? Col::OnAccent : Col::TextDim, keyText ? keyText : "");

    const ImVec2 trackMin(pos.x + width - trackW, pos.y + (height - trackH) * 0.5f);
    const ImVec2 trackMax(trackMin.x + trackW, trackMin.y + trackH);
    draw->AddRectFilled(trackMin, trackMax, Mix(Col::Border, Col::Accent, on), trackH * 0.5f);

    const float knobR = trackH * 0.5f - S(3.0f);
    const float knobX = Lerp(trackMin.x + trackH * 0.5f, trackMax.x - trackH * 0.5f, on);
    draw->AddCircleFilled(ImVec2(knobX, trackMin.y + trackH * 0.5f), knobR, Mix(Col::Text, Col::OnAccent, on * 0.15f));

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
    return clicked;
}

bool UI::Segmented(const char* id, std::span<const char* const> options, int* selected)
{
    if (options.empty())
        return false;

    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = S(34.0f);
    const int count = static_cast<int>(options.size());
    *selected = std::clamp(*selected, 0, count - 1);

    ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const float segment = width / count;
    const int hoveredIndex = hovered ? std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - min.x) / segment), 0, count - 1) : -1;
    bool changed = false;

    if (ImGui::IsItemClicked() && hoveredIndex >= 0 && hoveredIndex != *selected)
    {
        *selected = hoveredIndex;
        changed = true;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + width, min.y + height);
    draw->AddRectFilled(min, max, Col::Raised, height * 0.5f);

    // the gold pill slides to the selected segment
    const float pillX = Animate(ImGui::GetID(id), static_cast<float>(*selected), 16.0f);
    const ImVec2 pillMin(min.x + pillX * segment + S(3.0f), min.y + S(3.0f));
    const ImVec2 pillMax(pillMin.x + segment - S(6.0f), max.y - S(3.0f));
    draw->AddRectFilled(pillMin, pillMax, Col::Accent, (height - S(6.0f)) * 0.5f);

    for (int i = 0; i < count; ++i)
    {
        const std::string text = Visible(options[i]);
        const float fontSize = ImGui::GetFontSize() * 0.94f;
        const std::string fitted = Fit(text.c_str(), FontSemibold, fontSize, segment - S(12.0f));
        const ImVec2 textSize = TextSize(FontSemibold, fontSize, fitted.c_str());
        const ImVec2 pos(std::floor(min.x + i * segment + (segment - textSize.x) * 0.5f), std::floor(min.y + (height - textSize.y) * 0.5f));
        const ImU32 color = i == *selected ? Col::OnAccent : (i == hoveredIndex ? Col::Text : Col::TextDim);
        draw->AddText(FontSemibold, fontSize, pos, color, fitted.c_str());
    }

    return changed;
}

bool UI::Slider(const char* id, const char* label, float* value, float minValue, float maxValue, const char* valueText)
{
    const std::string text = Visible(label);
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // label ................ value
    const ImVec2 row = ImGui::GetCursorScreenPos();
    draw->AddText(row, Mix(Col::TextDim, Col::Text, 0.75f), text.c_str());
    if (valueText)
    {
        const ImVec2 valueSize = TextSize(FontSemibold, ImGui::GetFontSize(), valueText);
        draw->AddText(FontSemibold, ImGui::GetFontSize(), ImVec2(row.x + width - valueSize.x, row.y), Col::Text, valueText);
    }
    ImGui::Dummy(ImVec2(width, ImGui::GetFontSize() + S(2.0f)));

    const float height = S(20.0f);
    ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    bool changed = false;

    if (active && maxValue > minValue)
    {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - min.x) / width, 0.0f, 1.0f);
        const float next = minValue + t * (maxValue - minValue);

        if (next != *value)
        {
            *value = next;
            changed = true;
        }
    }

    const float t = maxValue > minValue ? std::clamp((*value - minValue) / (maxValue - minValue), 0.0f, 1.0f) : 0.0f;
    const float trackY = min.y + height * 0.5f;
    const float trackH = S(4.0f);
    draw->AddRectFilled(ImVec2(min.x, trackY - trackH * 0.5f), ImVec2(min.x + width, trackY + trackH * 0.5f), Col::Border, trackH);
    draw->AddRectFilled(ImVec2(min.x, trackY - trackH * 0.5f), ImVec2(min.x + width * t, trackY + trackH * 0.5f), Col::Accent, trackH);

    const float grow = Animate(ImGui::GetItemID(), (hovered || active) ? 1.0f : 0.0f);
    const float knob = S(7.0f) + S(2.0f) * grow;
    draw->AddCircleFilled(ImVec2(min.x + width * t, trackY), knob + S(3.0f), WithAlpha(Col::Accent, 0.18f * grow));
    draw->AddCircleFilled(ImVec2(min.x + width * t, trackY), knob, Col::Text);
    draw->AddCircle(ImVec2(min.x + width * t, trackY), knob, Col::Accent, 0, S(2.0f));

    return changed;
}

namespace
{
    // How much one repeat of a held +/- adds: faster the longer it's held (1, 5, 25, 100).
    int HoldStep()
    {
        const float held = ImGui::GetIO().MouseDownDuration[ImGuiMouseButton_Left];
        return held < 1.2f ? 1 : held < 2.4f ? 5 : held < 3.6f ? 25 : 100;
    }
}

bool UI::Stepper(const char* id, const char* label, int* value, int minValue, int maxValue)
{
    ImGui::PushID(id);
    const std::string text = Visible(label);
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = S(34.0f);
    const float box = S(150.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    bool changed = false;

    draw->AddText(ImVec2(pos.x, pos.y + (height - ImGui::GetFontSize()) * 0.5f), Mix(Col::TextDim, Col::Text, 0.75f), text.c_str());

    const ImVec2 boxMin(pos.x + width - box, pos.y);
    const ImVec2 boxMax(boxMin.x + box, pos.y + height);
    draw->AddRectFilled(boxMin, boxMax, Col::Raised, S(8.0f));

    // - and +: once on press, then again and again while held
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);

    ImGui::SetCursorScreenPos(boxMin);
    if (IconButton("##minus", 0xE15B, height))   // remove (Material Symbols)
    {
        *value = std::max(minValue, *value - HoldStep());
        changed = true;
    }

    ImGui::SetCursorScreenPos(ImVec2(boxMax.x - height, boxMin.y));
    if (IconButton("##plus", 0xE145, height))   // add
    {
        *value = std::min(maxValue, *value + HoldStep());
        changed = true;
    }

    ImGui::PopItemFlag();

    // the number in the middle: click it and type
    const float fieldWidth = box - height * 2.0f;
    char number[16];
    snprintf(number, sizeof(number), "%d", *value);

    ImGui::PushFont(FontSemibold, 0.0f);
    const float frameHeight = ImGui::GetFrameHeight();
    const float textWidth = ImGui::CalcTextSize(number).x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::max(0.0f, std::floor((fieldWidth - textWidth) * 0.5f)), ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, WithAlpha(Col::Hover, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, WithAlpha(Col::Hover, 0.6f));

    ImGui::SetCursorScreenPos(ImVec2(boxMin.x + height, boxMin.y + (height - frameHeight) * 0.5f));
    ImGui::SetNextItemWidth(fieldWidth);
    int typed = *value;

    if (ImGui::InputScalar("##value", ImGuiDataType_S32, &typed, nullptr, nullptr, "%d",
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll))
    {
        typed = std::clamp(typed, minValue, maxValue);

        if (typed != *value)
        {
            *value = typed;
            changed = true;
        }
    }

    const bool typing = ImGui::IsItemActive();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopFont();

    if (typing)
        draw->AddRect(boxMin, boxMax, WithAlpha(Col::Accent, 0.7f), S(8.0f));

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
    return changed;
}

bool UI::FloatField(const char* id, const char* label, float* value, float minValue, float maxValue, float step, const char* format)
{
    ImGui::PushID(id);
    const std::string text = Visible(label);
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = S(34.0f);
    const float box = S(190.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    bool changed = false;

    draw->AddText(ImVec2(pos.x, pos.y + (height - ImGui::GetFontSize()) * 0.5f), Mix(Col::TextDim, Col::Text, 0.75f), text.c_str());

    const ImVec2 boxMin(pos.x + width - box, pos.y);
    const ImVec2 boxMax(boxMin.x + box, pos.y + height);
    draw->AddRectFilled(boxMin, boxMax, Col::Raised, S(8.0f));

    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);

    ImGui::SetCursorScreenPos(boxMin);
    if (IconButton("##minus", 0xE15B, height))
    {
        *value = std::max(minValue, *value - step * static_cast<float>(HoldStep()));
        changed = true;
    }

    ImGui::SetCursorScreenPos(ImVec2(boxMax.x - height, boxMin.y));
    if (IconButton("##plus", 0xE145, height))
    {
        *value = std::min(maxValue, *value + step * static_cast<float>(HoldStep()));
        changed = true;
    }

    ImGui::PopItemFlag();

    // the number in the middle: click it and type (any precision)
    const float fieldWidth = box - height * 2.0f;
    char number[32];
    snprintf(number, sizeof(number), format, *value);

    ImGui::PushFont(FontSemibold, 0.0f);
    const float frameHeight = ImGui::GetFrameHeight();
    const float textWidth = ImGui::CalcTextSize(number).x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::max(0.0f, std::floor((fieldWidth - textWidth) * 0.5f)), ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, WithAlpha(Col::Hover, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, WithAlpha(Col::Hover, 0.6f));

    ImGui::SetCursorScreenPos(ImVec2(boxMin.x + height, boxMin.y + (height - frameHeight) * 0.5f));
    ImGui::SetNextItemWidth(fieldWidth);
    float typed = *value;

    if (ImGui::InputFloat("##value", &typed, 0.0f, 0.0f, format, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll))
    {
        typed = std::clamp(typed, minValue, maxValue);

        if (typed != *value)
        {
            *value = typed;
            changed = true;
        }
    }

    const bool typing = ImGui::IsItemActive();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopFont();

    if (typing)
        draw->AddRect(boxMin, boxMax, WithAlpha(Col::Accent, 0.7f), S(8.0f));

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
    return changed;
}

bool UI::Search(const char* id, const char* hint, std::string* text)
{
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = S(36.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), Col::Raised, S(9.0f));
    DrawCenteredIcon(draw, FontText, ImGui::GetFontSize() * 1.1f, { 0xE8B6 }, pos, ImVec2(pos.x + height, pos.y + height), Col::TextDim);   // search

    const float frameHeight = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(pos.x + height - S(6.0f), pos.y + (height - frameHeight) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
    const bool hasText = !text->empty();
    ImGui::SetNextItemWidth(width - height - (hasText ? height - S(6.0f) : 0.0f));
    const std::string hintText = Visible(hint);
    bool changed = ImGui::InputTextWithHint(id, hintText.c_str(), text);
    const bool focused = ImGui::IsItemActive();
    ImGui::PopStyleColor(3);

    if (focused)
        draw->AddRect(pos, ImVec2(pos.x + width, pos.y + height), WithAlpha(Col::Accent, 0.7f), S(9.0f));

    // something typed: an x at the end clears it
    if (hasText)
    {
        const ImVec2 clearMin(pos.x + width - height, pos.y);
        ImGui::SetCursorScreenPos(clearMin);
        ImGui::PushID(id);
        const bool clear = ImGui::InvisibleButton("##clear", ImVec2(height, height));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        DrawCenteredIcon(draw, FontText, ImGui::GetFontSize(), { 0xE5CD }, clearMin, ImVec2(clearMin.x + height, clearMin.y + height),
            hovered ? Col::Text : Col::TextDim);   // close

        if (clear)
        {
            text->clear();
            changed = true;
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    return changed;
}

void UI::Status(bool ok, const char* text, const char* tooltip)
{
    const std::string label = Visible(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ok ? Col::Success : Col::Warning;
    const ImVec2 dot(pos.x + S(5.0f), pos.y + lineHeight * 0.5f);

    if (!ok)   // a slow pulse while waiting
    {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
        draw->AddCircleFilled(dot, S(4.0f) + S(3.0f) * pulse, WithAlpha(color, 0.25f * (1.0f - pulse)));
    }

    draw->AddCircleFilled(dot, S(4.0f), color);
    draw->AddText(ImVec2(pos.x + S(16.0f), pos.y), Col::TextDim, label.c_str());
    ImGui::Dummy(ImVec2(S(16.0f) + ImGui::CalcTextSize(label.c_str()).x, lineHeight));

    if (tooltip && *tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
}

void UI::Chip(const char* text, ImU32 color)
{
    const std::string label = Visible(text);
    const float fontSize = ImGui::GetFontSize() * 0.86f;
    const ImVec2 textSize = TextSize(FontSemibold, fontSize, label.c_str());
    const ImVec2 size(textSize.x + S(18.0f), textSize.y + S(8.0f));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(pos, Add(pos, size), WithAlpha(color, 0.14f), size.y * 0.5f);
    draw->AddText(FontSemibold, fontSize, ImVec2(pos.x + S(9.0f), pos.y + S(4.0f)), color, label.c_str());
    ImGui::Dummy(size);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// cards
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

UI::Grid UI::MakeGrid(float minWidth)
{
    minWidth *= CardPictureScale;   // the tab's picture size
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    Grid grid;
    grid.columns = std::max(1, static_cast<int>((available + gap) / (minWidth + gap)));
    grid.width = std::floor((available - gap * (grid.columns - 1)) / grid.columns);
    return grid;
}

float UI::CardHeight(float width)
{
    const float pad = S(8.0f);
    return std::floor(width * 0.64f) + pad + ImGui::GetFontSize() * (0.95f + 0.82f) * CardTextScale + S(3.0f) + pad;
}

UI::Grid UI::MakeGridFitting(int count, float minWidth, float maxWidth)
{
    Grid best = MakeGrid(minWidth);   // (it scales minWidth itself)
    maxWidth *= CardPictureScale;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float gapX = ImGui::GetStyle().ItemSpacing.x;
    const float gapY = ImGui::GetStyle().ItemSpacing.y;

    if (count <= 0 || available.y <= 0.0f)
        return best;

    // fewer columns = wider cards; as long as everything still fits below
    for (int columns = best.columns - 1; columns >= 1; --columns)
    {
        const float width = std::floor((std::max(1.0f, available.x) - gapX * static_cast<float>(columns - 1)) / static_cast<float>(columns));

        if (width > maxWidth)
            break;

        const int rows = (count + columns - 1) / columns;

        if (static_cast<float>(rows) * (CardHeight(width) + gapY) - gapY > available.y)
            break;

        best.columns = columns;
        best.width = width;
    }

    return best;
}

void UI::NextCell(Grid& grid)
{
    if (grid.index % grid.columns != 0)
        ImGui::SameLine();

    ++grid.index;
}

std::string UI::Fit(const char* text, ImFont* font, float size, float width)
{
    if (!text)
        return {};

    if (TextSize(font, size, text).x <= width)
        return text;

    std::string cut = text;

    while (!cut.empty())
    {
        // whole UTF-8 characters: pop until the byte taken off was the character's first (not a 10xxxxxx continuation)
        unsigned char removed = 0;
        do
        {
            removed = static_cast<unsigned char>(cut.back());
            cut.pop_back();
        } while (!cut.empty() && (removed & 0xC0) == 0x80);

        if (TextSize(font, size, (cut + "...").c_str()).x <= width)
            return cut + "...";
    }

    return "...";
}

void UI::DrawImage(ImDrawList* draw, const CardImage& image, const ImVec2& min, const ImVec2& max)
{
    if (!image.texture || image.width <= 0.0f || image.height <= 0.0f)
        return;

    const float areaW = max.x - min.x;
    const float areaH = max.y - min.y;
    const float scale = std::min(areaW / image.width, areaH / image.height);
    const ImVec2 size(image.width * scale, image.height * scale);
    const ImVec2 pos(min.x + (areaW - size.x) * 0.5f, min.y + (areaH - size.y) * 0.5f);
    draw->AddImage(image.texture, pos, Add(pos, size), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), image.tint);
}

void UI::RarityBackdrop(ImDrawList* draw, const ImVec2& min, const ImVec2& max, ImU32 rarity, ImU32 base, float strength, float rounding,
    ImDrawFlags corners)
{
    const float width = max.x - min.x;
    const float height = max.y - min.y;

    if (width < 1.0f || height < 1.0f)
        return;

    strength = std::clamp(strength, 0.0f, 1.0f);
    const ImU32 left = Mix(base, rarity, 0.80f * strength);
    const ImU32 right = Mix(base, rarity, 0.10f * strength);

    // one rounded fill, coloured per vertex (keeps each vertex's alpha: the anti-aliased edge)
    const int first = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, IM_COL32_WHITE, rounding, corners);

    for (int i = first; i < draw->VtxBuffer.Size; ++i)
    {
        ImDrawVert& vertex = draw->VtxBuffer[i];
        const float x = std::clamp((vertex.pos.x - min.x) / width, 0.0f, 1.0f);
        const float y = std::clamp((vertex.pos.y - min.y) / height, 0.0f, 1.0f);
        const float light = 0.10f * strength * std::max(0.0f, 1.0f - 0.6f * x - 0.8f * y);
        const ImU32 color = Mix(Mix(left, right, x), IM_COL32_WHITE, light);
        vertex.col = (color & ~IM_COL32_A_MASK) | (vertex.col & IM_COL32_A_MASK);
    }

    // the mesh: thin dark diagonals both ways, clipped to the backdrop
    const float step = std::max(4.0f, S(5.0f));
    const ImU32 line = IM_COL32(0, 0, 0, static_cast<int>(26.0f * strength));
    draw->PushClipRect(min, max, true);

    for (float offset = -height; offset < width; offset += step)
    {
        draw->AddLine(ImVec2(min.x + offset, max.y), ImVec2(min.x + offset + height, min.y), line);
        draw->AddLine(ImVec2(min.x + offset, min.y), ImVec2(min.x + offset + height, max.y), line);
    }

    draw->PopClipRect();
}

bool UI::Card(const char* id, float width, const std::string& title, const std::string& subtitle, ImU32 rarity, bool selected,
    const std::function<CardImage()>& image, const char* badge, ImU32 badgeColor)
{
    const float pad = S(8.0f);
    const float imageHeight = std::floor(width * 0.64f);
    const float titleSize = ImGui::GetFontSize() * 0.95f * CardTextScale;   // the tab's text size
    const float subSize = ImGui::GetFontSize() * 0.82f * CardTextScale;
    const float height = imageHeight + pad + titleSize + S(3.0f) + subSize + pad;

    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f);
    ImGui::PopID();

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();

    if (!ImGui::IsRectVisible(min, max))
        return clicked;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = S(10.0f);
    const ImVec2 lift(0.0f, -S(2.0f) * hover);
    const ImVec2 a = Add(min, lift), b = Add(max, lift);

    const ImU32 fill = Mix(Col::Raised, Col::Hover, hover * 0.8f);
    draw->AddRectFilled(a, b, fill, rounding);

    // picture well: the rarity's backdrop (CS2's loadout card look); no rarity: a faint glow from the bottom
    const ImVec2 wellMin(a.x + S(1.0f), a.y + S(1.0f));
    const ImVec2 wellMax(b.x - S(1.0f), a.y + imageHeight);

    if (rarity)
        RarityBackdrop(draw, wellMin, wellMax, rarity, fill, 0.8f + 0.2f * hover, rounding - S(1.0f), ImDrawFlags_RoundCornersTop);
    else
    {
        const ImU32 glow = WithAlpha(Col::Text, 0.03f);
        draw->AddRectFilledMultiColor(wellMin, wellMax, WithAlpha(glow, 0.0f), WithAlpha(glow, 0.0f), glow, glow);
    }

    DrawImage(draw, image(), ImVec2(wellMin.x + S(10.0f), wellMin.y + S(8.0f)), ImVec2(wellMax.x - S(10.0f), wellMax.y - S(6.0f)));

    if (rarity)
        draw->AddRectFilled(ImVec2(a.x + pad, wellMax.y - S(3.0f)), ImVec2(b.x - pad, wellMax.y), rarity, S(2.0f));

    if (badge && *badge)
    {
        const float badgeSize = ImGui::GetFontSize() * 0.72f;
        const ImVec2 badgeText = TextSize(FontSemibold, badgeSize, badge);
        const ImVec2 badgeMin(a.x + S(6.0f), a.y + S(6.0f));
        const ImVec2 badgeMax(badgeMin.x + badgeText.x + S(10.0f), badgeMin.y + badgeText.y + S(4.0f));
        draw->AddRectFilled(badgeMin, badgeMax, WithAlpha(badgeColor, 0.9f), (badgeMax.y - badgeMin.y) * 0.5f);
        draw->AddText(FontSemibold, badgeSize, ImVec2(badgeMin.x + S(5.0f), badgeMin.y + S(2.0f)), Col::OnAccent, badge);
    }

    const float textWidth = width - pad * 2.0f;
    const std::string line1 = Fit(title.c_str(), FontSemibold, titleSize, textWidth);
    const std::string line2 = Fit(subtitle.c_str(), FontText, subSize, textWidth);
    draw->AddText(FontSemibold, titleSize, ImVec2(a.x + pad, wellMax.y + pad * 0.75f), Col::Text, line1.c_str());
    draw->AddText(FontText, subSize, ImVec2(a.x + pad, wellMax.y + pad * 0.75f + titleSize + S(3.0f)), Col::TextDim, line2.c_str());

    if (selected)
        draw->AddRect(a, b, Col::Accent, rounding, 0, S(2.0f));
    else
        draw->AddRect(a, b, Mix(Col::Border, WithAlpha(Col::Accent, 0.6f), hover * 0.6f), rounding);

    if (hovered && TextSize(FontSemibold, titleSize, title.c_str()).x > textWidth)
        ImGui::SetTooltip("%s", title.c_str());

    return clicked;
}

namespace
{
    // A small slider on one line: "label ──●── 120%" (5 % steps). True = changed. (CardSizeRow)
    bool MiniSlider(const char* id, const char* label, float* value, float minValue, float maxValue, float trackWidth, float height)
    {
        const std::string text = Visible(label);
        const float fontSize = ImGui::GetFontSize() * 0.86f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 labelSize = TextSize(UI::FontText, fontSize, text.c_str());
        draw->AddText(UI::FontText, fontSize, ImVec2(pos.x, pos.y + std::floor((height - labelSize.y) * 0.5f)), UI::Col::TextDim, text.c_str());

        ImGui::SetCursorScreenPos(ImVec2(pos.x + labelSize.x + S(8.0f), pos.y));
        ImGui::InvisibleButton(id, ImVec2(trackWidth, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImVec2 min = ImGui::GetItemRectMin();
        bool changed = false;

        if (active && maxValue > minValue)
        {
            const float t = std::clamp((ImGui::GetIO().MousePos.x - min.x) / trackWidth, 0.0f, 1.0f);
            const float next = std::clamp(std::round((minValue + t * (maxValue - minValue)) * 20.0f) / 20.0f, minValue, maxValue);

            if (next != *value)
            {
                *value = next;
                changed = true;
            }
        }

        const float t = maxValue > minValue ? std::clamp((*value - minValue) / (maxValue - minValue), 0.0f, 1.0f) : 0.0f;
        const float trackY = min.y + height * 0.5f;
        const float trackH = S(3.0f);
        draw->AddRectFilled(ImVec2(min.x, trackY - trackH * 0.5f), ImVec2(min.x + trackWidth, trackY + trackH * 0.5f), UI::Col::Border, trackH);
        draw->AddRectFilled(ImVec2(min.x, trackY - trackH * 0.5f), ImVec2(min.x + trackWidth * t, trackY + trackH * 0.5f), UI::Col::Accent, trackH);

        const float grow = Animate(ImGui::GetItemID(), (hovered || active) ? 1.0f : 0.0f);
        const float knob = S(5.5f) + S(1.5f) * grow;
        draw->AddCircleFilled(ImVec2(min.x + trackWidth * t, trackY), knob, UI::Col::Text);
        draw->AddCircle(ImVec2(min.x + trackWidth * t, trackY), knob, UI::Col::Accent, 0, S(1.5f));

        char number[16];
        snprintf(number, sizeof(number), "%d%%", static_cast<int>(std::lround(*value * 100.0f)));
        draw->AddText(UI::FontSemibold, fontSize, ImVec2(min.x + trackWidth + S(8.0f), pos.y + std::floor((height - fontSize) * 0.5f)), UI::Col::Text, number);

        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy(ImVec2(labelSize.x + S(8.0f) + trackWidth + S(8.0f) + TextSize(UI::FontSemibold, fontSize, "200%").x, height));
        return changed;
    }
}

bool UI::CardSizeRow(const char* id, float* picture, float* text)
{
    ImGui::PushID(id);
    const float height = S(24.0f);
    const float track = S(110.0f);
    const float fontSize = ImGui::GetFontSize() * 0.86f;
    const float number = TextSize(FontSemibold, fontSize, "200%").x;
    const auto width = [&](const char* label) { return TextSize(FontText, fontSize, Visible(label).c_str()).x + S(8.0f) + track + S(8.0f) + number; };
    const float total = width("Pictures") + S(18.0f) + width("Text size") + S(10.0f) + height;

    // at the right of the line
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - total));
    bool changed = MiniSlider("##card_picture", "Pictures", picture, 0.6f, 2.0f, track, height);
    ImGui::SameLine(0.0f, S(18.0f));
    changed |= MiniSlider("##card_text", "Text size", text, 0.6f, 2.0f, track, height);
    ImGui::SameLine(0.0f, S(10.0f));

    if (IconButton("##card_size_reset", 0xE5D5, height, "Back to the usual size") && (*picture != 1.0f || *text != 1.0f))
    {
        *picture = 1.0f;
        *text = 1.0f;
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// top bar
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void UI::Logo(ImDrawList* draw, const ImVec2& min, float size)
{
    const ImVec2 max(min.x + size, min.y + size);
    const float rounding = size * 0.26f;
    draw->AddRectFilled(min, max, Col::Accent, rounding);
    draw->AddRectFilled(min, ImVec2(max.x, min.y + size * 0.5f), WithAlpha(Col::AccentHot, 0.6f), rounding, ImDrawFlags_RoundCornersTop);

    // a case lid and a star
    const float lid = min.y + size * 0.36f;
    draw->AddLine(ImVec2(min.x + size * 0.18f, lid), ImVec2(max.x - size * 0.18f, lid), Col::OnAccent, std::max(1.0f, size * 0.07f));
    Star(draw, ImVec2(min.x + size * 0.5f, min.y + size * 0.64f), size * 0.22f, Col::OnAccent);
}

bool UI::TopTab(const char* label, ImWchar icon, bool selected, float height)
{
    const std::string text = Visible(label);
    const float fontSize = ImGui::GetFontSize();
    const float iconSize = fontSize * 1.2f;
    const ImVec2 textSize = TextSize(FontSemibold, fontSize, text.c_str());
    const float width = iconSize + S(8.0f) + textSize.x + S(24.0f);

    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const float hover = Animate(ImGui::GetItemID(), hovered || selected ? 1.0f : 0.0f);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImU32 color = selected ? Col::Text : Mix(Col::TextDim, Col::Text, hover);
    const float x = min.x + S(12.0f);
    const float midY = min.y + height * 0.5f;

    DrawCenteredIcon(draw, FontText, iconSize, { icon }, ImVec2(x, midY - iconSize * 0.5f), ImVec2(x + iconSize, midY + iconSize * 0.5f),
        selected ? Col::Accent : color);
    draw->AddText(FontSemibold, fontSize, ImVec2(x + iconSize + S(8.0f), std::floor(midY - textSize.y * 0.5f)), color, text.c_str());

    if (selected)
        draw->AddRectFilled(ImVec2(min.x + S(10.0f), max.y - S(3.0f)), ImVec2(max.x - S(10.0f), max.y), Col::Accent, S(2.0f));

    return clicked;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// pop-ups
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

ImVec2 UI::ToastSize(const char* text, float wrapWidth)
{
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 textSize = FontSemibold->CalcTextSizeA(fontSize, FLT_MAX, wrapWidth, text);
    return ImVec2(textSize.x + S(34.0f), textSize.y + S(22.0f));
}

void UI::Toast(ImDrawList* draw, const ImVec2& min, const ImVec2& max, const char* text, float wrapWidth, ImU32 accent, float alpha)
{
    const ImU32 color = accent ? accent : Col::Accent;
    draw->AddRectFilled(Add(min, S(0.0f, 3.0f)), Add(max, S(0.0f, 3.0f)), WithAlpha(IM_COL32(0, 0, 0, 255), 0.35f * alpha), S(10.0f));
    draw->AddRectFilled(min, max, WithAlpha(Col::Raised, 0.97f * alpha), S(10.0f));
    draw->AddRect(min, max, WithAlpha(Col::Border, alpha), S(10.0f));
    draw->AddRectFilled(ImVec2(min.x + S(8.0f), min.y + S(9.0f)), ImVec2(min.x + S(11.0f), max.y - S(9.0f)), WithAlpha(color, alpha), S(2.0f));
    draw->AddText(FontSemibold, ImGui::GetFontSize(), ImVec2(min.x + S(22.0f), min.y + S(11.0f)), WithAlpha(Col::Text, alpha), text, nullptr, wrapWidth);
}
