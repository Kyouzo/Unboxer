#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cstdio>

#include "../../../external/imgui/imgui.h"
#include "../../../external/imgui/imgui_impl_dx11.h"
#include "../../../external/imgui/imgui_impl_win32.h"

#include "fonts/LoadFonts.hpp"
#include "ui_scale.h"
#include "localization.h"
#include "ui/ui.h"

using UiScale::S;

// Menu state shared between the WndProc (window thread), the Present hook and the Settings page.
namespace MenuState
{
	// Settings / Misc "Key" buttons: which key the next key press becomes (the WndProc sets it). Esc cancels, Page Down
	// is the unload key; Backspace / Delete clear a hotkey (not the menu key). Mouse 3 / 4 / 5 work for the hotkeys.
	enum KeyTarget : int { KeyNone = 0, KeyMenu, KeyBhop, KeyFov, KeyReapply };
	inline std::atomic_int capturingKey{ KeyNone };
	inline std::atomic_bool resetSizePending{ false };          // push Config::menu size into the window
	inline std::atomic_bool centerPending{ false };             // center the window on the display
	inline std::atomic<unsigned long long> dirtySinceMs{ 0 };   // menu settings changed → saved after a short delay
	inline std::atomic_bool unloadRequested{ false };           // Settings "Unload" button (same as Page Down)

	inline void MarkDirty() { dirtySinceMs.store(GetTickCount64()); }

	// The config field a KeyTarget sets (null for KeyNone). menu.cpp.
	int* KeyFor(int target);
	// "F6", "Mouse 4", "Home"... (render thread: one buffer)
	const char* KeyName(int vk);
	// "Key: F6" button: click it, then press the key.
	void KeyButton(const char* id, int target);
	// A toggle row with its key in it (the chip left of the switch). True = toggled.
	bool KeyToggle(const char* label, bool* value, int target);
	// Just the key chip ("F6"), `width` wide, on the current line (after SameLine).
	void KeyChip(const char* id, int target, float width);

	// The menu window's middle (render thread): dialogs open there.
	inline ImVec2 windowCenter{ 0.0f, 0.0f };

	// What the Present hook detected, shown in Settings → Display.
	inline std::atomic_int backbufferW{ 0 }, backbufferH{ 0 }, windowW{ 0 }, windowH{ 0 };
}

struct IconGlyph
{
	ImWchar codepoint;
	ImVec2 offset = ImVec2(0.0f, 0.0f);
};

// Material Symbols Rounded (merged into the text fonts)
inline constexpr ImWchar ICON_CASES = 0xE8B1;      // redeem
inline constexpr ImWchar ICON_INVENTORY = 0xE1A1;  // inventory_2
inline constexpr ImWchar ICON_UPGRADER = 0xE8E5;   // trending_up
inline constexpr ImWchar ICON_SETTINGS = 0xE8B8;   // settings
inline constexpr ImWchar ICON_MISC = 0xE429;       // tune
inline constexpr ImWchar ICON_ADD_ITEMS = 0xE146;  // add_box
inline constexpr ImWchar ICON_CLOSE = 0xE5CD;      // close
inline constexpr ImWchar ICON_DELETE = 0xE872;     // delete
inline constexpr ImWchar ICON_KEY = 0xE73C;        // key
inline constexpr ImWchar ICON_CASINO = 0xEB40;     // casino (random)
inline constexpr ImWchar ICON_TARGET = 0xE55C;     // my_location (picked)

class Menu
{
public:
	Menu();

	void init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView);
	void render();
	void toggleMenu();

	bool showMenu;

private:
	void RenderHeader(float height);
	void CenterCursor();

	int activeTab;

	HWND gameWindow = nullptr;
	bool centerCursorPending = false;

	// menu size bookkeeping (see Menu::render)
	float appliedWidth = -1.0f, appliedHeight = -1.0f, appliedScale = -1.0f;
	ImVec2 lastWindowSize = ImVec2(0.0f, 0.0f);
	double openedAt = -1.0;
	bool firstShow = true;
};

inline void DrawCenteredIcon(ImDrawList* drawList, ImFont* font, float fontSize, const IconGlyph& icon, const ImVec2& min, const ImVec2& max, ImU32 color)
{
	// icons are merged into the text font (Material Symbols)
	if (g_FontIconFallback)
		font = g_FontIconFallback;

	if (!drawList || !font)
		return;

	ImFontBaked* baked = font->GetFontBaked(fontSize);

	if (!baked)
		return;

	ImFontGlyph* glyph = baked->FindGlyphNoFallback(icon.codepoint);

	if (!glyph)
		return;

	const float glyphWidth = glyph->X1 - glyph->X0;
	const float glyphHeight = glyph->Y1 - glyph->Y0;
	const float boxWidth = max.x - min.x;
	const float boxHeight = max.y - min.y;

	ImVec2 pos(
		min.x + (boxWidth - glyphWidth) * 0.5f - glyph->X0 + icon.offset.x,
		min.y + (boxHeight - glyphHeight) * 0.5f - glyph->Y0 + icon.offset.y
	);

	font->RenderChar(drawList, fontSize, pos, color, icon.codepoint);
}
