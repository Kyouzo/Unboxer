#pragma once

#include <stdio.h>
#include <Windows.h>
#include <imgui.h>

#include "../../menu/fonts/Outfit_Regular_compressed.h"
#include "../../menu/fonts/Outfit_Bold_compressed.h"
#include "../../menu/fonts/IonIcons_compressed.h"
#include "../../menu/fonts/MaterialSymbolsRounded_compressed.h"

inline ImFont* g_FontRegular = nullptr;
inline ImFont* g_FontBold = nullptr;
inline ImFont* g_FontContentRegular = nullptr;
inline ImFont* g_FontContentBold = nullptr;
inline ImFont* g_FontIcons = nullptr;

// The style's look (menu/look.h) swaps the four text fonts above; icons are always drawn from this Outfit font,
// which has Material Symbols merged in. g_FontGeneration tells the look when LoadFonts rebuilt the atlas.
inline ImFont* g_FontIconFallback = nullptr;
inline int g_FontGeneration = 0;

inline const ImWchar* GetMaterialSymbolRanges()
{
#ifdef IMGUI_USE_WCHAR32
	static const ImWchar ranges[] = { 0xE000, 0xF8FF, 0xF0000, 0xFFFFD, 0 };
#else
	static const ImWchar ranges[] = { 0xE000, 0xF8FF, 0 };
#endif

	return ranges;
}

inline void MergeMaterialSymbols(ImGuiIO& io, ImFont* targetFont, float sizeIcons)
{
	if (!targetFont)
		return;

	ImFontConfig cfg{};
	cfg.MergeMode = true;
	cfg.DstFont = targetFont;
	cfg.GlyphMinAdvanceX = sizeIcons;
	cfg.GlyphMaxAdvanceX = sizeIcons;
	cfg.GlyphOffset = ImVec2(0.0f, 0.0f);

	snprintf(cfg.Name, sizeof(cfg.Name), "Material Symbols Rounded, %.0fpx", sizeIcons);

	io.Fonts->AddFontFromMemoryCompressedTTF(
		MaterialSymbolsRounded_compressed_data,
		MaterialSymbolsRounded_compressed_size,
		sizeIcons,
		&cfg,
		GetMaterialSymbolRanges()
	);
}

// U+E000..U+F8FF: where the Material Symbols icons live. Windows fonts merged into a text font must not answer for
// those codepoints (ImGui 1.92 loads glyphs on demand and asks the sources in order).
inline const ImWchar* PrivateUseRange()
{
	static const ImWchar range[] = { 0xE000, 0xF8FF, 0 };
	return range;
}

// Outfit has no Cyrillic glyphs. Merge Windows' Segoe UI in for the Cyrillic range only, so Russian text renders
// while Latin text keeps coming from Outfit (sources are searched in order). Skipped silently if the file is missing.
inline void MergeCyrillic(ImGuiIO& io, ImFont* targetFont, float size, bool bold)
{
	if (!targetFont)
		return;

	char windowsDir[MAX_PATH]{};
	if (!GetWindowsDirectoryA(windowsDir, MAX_PATH))
		return;

	char path[MAX_PATH]{};
	snprintf(path, sizeof(path), "%s\\Fonts\\%s", windowsDir, bold ? "segoeuib.ttf" : "segoeui.ttf");

	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		return;

	ImFontConfig cfg{};
	cfg.MergeMode = true;
	cfg.DstFont = targetFont;
	cfg.OversampleH = cfg.OversampleV = 2;
	snprintf(cfg.Name, sizeof(cfg.Name), "Segoe UI Cyrillic, %.0fpx", size);
	cfg.GlyphExcludeRanges = PrivateUseRange();   // Segoe UI has private-use glyphs: the icons must win

	static const ImWchar cyrillicRanges[] = { 0x0400, 0x052F, 0x2DE0, 0x2DFF, 0xA640, 0xA69F, 0 };
	io.Fonts->AddFontFromFileTTF(path, size, &cfg, cyrillicRanges);

	// ★ ™ (item names: "★ Karambit", "StatTrak™") and ● ○ (status dots): Segoe UI Symbol
	char symbolPath[MAX_PATH]{};
	snprintf(symbolPath, sizeof(symbolPath), "%s\\Fonts\\seguisym.ttf", windowsDir);

	if (GetFileAttributesA(symbolPath) == INVALID_FILE_ATTRIBUTES)
		return;

	ImFontConfig symbolCfg{};
	symbolCfg.MergeMode = true;
	symbolCfg.DstFont = targetFont;
	symbolCfg.OversampleH = symbolCfg.OversampleV = 2;
	symbolCfg.GlyphExcludeRanges = PrivateUseRange();
	snprintf(symbolCfg.Name, sizeof(symbolCfg.Name), "Segoe UI Symbol, %.0fpx", size);

	static const ImWchar symbolRanges[] = { 0x2122, 0x2122, 0x25CB, 0x25CF, 0x2605, 0x2606, 0 };
	io.Fonts->AddFontFromFileTTF(symbolPath, size, &symbolCfg, symbolRanges);
}

inline ImFont* AddOutfitRegular(ImGuiIO& io, float size, bool mergeIcons, float iconSize, const char* namePrefix)
{
	ImFontConfig cfg{};
	cfg.OversampleH = cfg.OversampleV = 2;

	snprintf(cfg.Name, sizeof(cfg.Name), "%s, %.0fpx", namePrefix, size);

	ImFont* font = io.Fonts->AddFontFromMemoryCompressedTTF(
		Outfit_Regular_compressed_data,
		Outfit_Regular_compressed_size,
		size,
		&cfg,
		io.Fonts->GetGlyphRangesDefault()
	);

	if (mergeIcons)
		MergeMaterialSymbols(io, font, iconSize);

	return font;
}

inline ImFont* AddOutfitBold(ImGuiIO& io, float size, bool mergeIcons, float iconSize, const char* namePrefix)
{
	ImFontConfig cfg{};
	cfg.OversampleH = cfg.OversampleV = 2;
	cfg.RasterizerMultiply = 1.10f;

	snprintf(cfg.Name, sizeof(cfg.Name), "%s, %.0fpx", namePrefix, size);

	ImFont* font = io.Fonts->AddFontFromMemoryCompressedTTF(
		Outfit_Bold_compressed_data,
		Outfit_Bold_compressed_size,
		size,
		&cfg,
		io.Fonts->GetGlyphRangesDefault()
	);

	if (mergeIcons)
		MergeMaterialSymbols(io, font, iconSize);

	return font;
}

inline void LoadFonts(bool mergeIconsIntoText, float sizeMain, float sizeIcons, float sizeContent = 18.0f, float sizeContentIcons = 18.0f)
{
	ImGuiIO& io = ImGui::GetIO();

	io.Fonts->Clear();
	g_FontRegular = nullptr;
	g_FontBold = nullptr;
	g_FontContentRegular = nullptr;
	g_FontContentBold = nullptr;
	g_FontIcons = nullptr;

	g_FontRegular = AddOutfitRegular(io, sizeMain, mergeIconsIntoText, sizeIcons, "Outfit Regular");
	MergeCyrillic(io, g_FontRegular, sizeMain, false);
	g_FontBold = AddOutfitBold(io, sizeMain, mergeIconsIntoText, sizeIcons, "Outfit Bold");
	MergeCyrillic(io, g_FontBold, sizeMain, true);

	g_FontContentRegular = AddOutfitRegular(io, sizeContent, mergeIconsIntoText, sizeContentIcons, "Outfit Content Regular");
	MergeCyrillic(io, g_FontContentRegular, sizeContent, false);
	g_FontContentBold = AddOutfitBold(io, sizeContent, mergeIconsIntoText, sizeContentIcons, "Outfit Content Bold");
	MergeCyrillic(io, g_FontContentBold, sizeContent, true);

	if (!mergeIconsIntoText)
	{
		ImFontConfig cfg{};
		cfg.GlyphMinAdvanceX = sizeIcons;
		cfg.GlyphMaxAdvanceX = sizeIcons;
		cfg.GlyphOffset = ImVec2(0.0f, 0.0f);

		snprintf(cfg.Name, sizeof(cfg.Name), "Material Symbols Rounded, %.0fpx", sizeIcons);

		g_FontIcons = io.Fonts->AddFontFromMemoryCompressedTTF(
			MaterialSymbolsRounded_compressed_data,
			MaterialSymbolsRounded_compressed_size,
			sizeIcons,
			&cfg,
			GetMaterialSymbolRanges()
		);
	}

	io.FontDefault = g_FontRegular ? g_FontRegular : g_FontBold;
	g_FontIconFallback = mergeIconsIntoText ? g_FontRegular : g_FontIcons;
	++g_FontGeneration;
}
