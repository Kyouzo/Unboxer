#include "unboxer_menu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <unordered_map>
#include <string>
#include <vector>

#include "../menu.h"
#include "../notify.h"
#include "../../../../external/imgui/imgui_stdlib.h"
#include "../skinchanger/images/skin_images.h"
#include "../../features/skinchanger/item_schema.h"
#include "../../features/unboxer/cases.h"
#include "../../features/unboxer/prices.h"
#include "../../features/unboxer/unboxer.h"
#include "../../features/model_preview/model_preview.h"
#include "../../features/model_preview/model_preview_widget.h"

namespace
{
    // ── render thread state ──
    std::uint16_t g_selectedCase = 0;
    std::string g_caseSearch;
    int g_caseKind = 0;               // Cases::Category shown in the list
    int g_addCount = 1;
    bool g_addWithKeys = true;
    std::uint16_t g_goldItem = 0;     // ★ picker: the knife / gloves whose finishes are shown
    std::uint16_t g_caseGoldItem = 0; // the same, limited to the case's own ★ types
    std::uint16_t g_gunItem = 0;      // All skins: the gun whose finishes are shown
    std::string g_skinSearch;
    int g_pickCategory = 0;           // 0 this case, 1 knives & gloves, 2 all skins
    std::string g_itemsSearch;
    int g_itemsFilter = 0;            // 0 all, 1 cases, 2 keys, 3 opened, 4 equipped, 5 storage units (and what's in them)
    // Inventory multi-select: Ctrl+click adds / drops a card, Shift+click everything from the last clicked card to
    // this one. Only cards on screen stay selected (a filter / search change drops the hidden ones).
    std::vector<std::uint64_t> g_itemsSelected;
    std::uint64_t g_itemsAnchor = 0;

    // CS2's rarity colours
    ImU32 TierColor(int tier)
    {
        switch (tier)
        {
        case Cases::TierConsumer: return IM_COL32(176, 195, 217, 255);
        case Cases::TierIndustrial: return IM_COL32(94, 152, 217, 255);
        case Cases::TierMilSpec: return IM_COL32(75, 105, 255, 255);
        case Cases::TierRestricted: return IM_COL32(136, 71, 255, 255);
        case Cases::TierClassified: return IM_COL32(211, 44, 230, 255);
        case Cases::TierCovert: return IM_COL32(235, 75, 75, 255);
        case Cases::TierGold: return IM_COL32(228, 174, 57, 255);
        default: return IM_COL32(176, 195, 217, 255);
        }
    }

    // "Mil-Spec" for skins; stickers, patches, graffiti, music kits and pins say "High Grade" ... "Extraordinary".
    const char* TierLabel(int tier, bool skins)
    {
        if (skins || tier == Cases::TierGold)
            return Cases::TierName(static_cast<Cases::Tier>(tier));

        switch (tier)
        {
        case Cases::TierConsumer: return "Base Grade";
        case Cases::TierIndustrial: return "Industrial Grade";
        case Cases::TierMilSpec: return "High Grade";
        case Cases::TierRestricted: return "Remarkable";
        case Cases::TierClassified: return "Exotic";
        default: return "Extraordinary";
        }
    }

    bool HoldsSkins(const Cases::Case& item)
    {
        return item.category == Cases::Category::Weapon || item.category == Cases::Category::Souvenir || item.category == Cases::Category::Other;
    }

    ImU32 ItemColor(const Unboxer::Item& item)
    {
        if (item.kind != Unboxer::Kind::Drop)
            return IM_COL32(176, 195, 217, 255);   // Base Grade

        if (item.quality == 3)
            return TierColor(Cases::TierGold);

        return TierColor(std::clamp(item.rarity - 1, 0, static_cast<int>(Cases::TierCovert)));
    }

    std::string Lower(std::string text)
    {
        for (char& c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    bool Matches(const std::string& haystackLower, const std::string& search)
    {
        return search.empty() || haystackLower.find(Lower(search)) != std::string::npos;
    }

    // ── pictures ──
    UI::CardImage ToCard(const SkinImages::Image* image)
    {
        if (!image || !image->texture)
            return {};

        return { reinterpret_cast<ImTextureID>(image->texture), static_cast<float>(image->width), static_cast<float>(image->height) };
    }

    // A game picture, or (CS2 doesn't ship it: the (Redacted) Map Coin, the Budapest 2025 souvenir charm...) Lootline's
    // own copy from data\extra_images.bin.
    UI::CardImage GameOrExtraImage(const std::string& path)
    {
        if (path.empty())
            return {};

        if (const UI::CardImage image = ToCard(SkinImages::GetInventoryImage(path)); image.texture)
            return image;

        const Cases::EmbeddedImage embedded = Cases::ExtraImage(path);
        return embedded.data ? ToCard(SkinImages::GetEncodedImage(path, embedded.data, embedded.size)) : UI::CardImage{};
    }

    UI::CardImage DropImage(std::uint16_t definition, int paintKit)
    {
        if (definition == Unboxer::DEF_STICKER_SLAB)   // the slab tool: the slab charm's picture
            return DropImage(Unboxer::DEF_CHARM, Unboxer::KEYCHAIN_STICKER_SLAB);

        if (definition == Unboxer::DEF_CHARM)
        {
            const KeychainInfo* charm = Skins::g_ItemSchema.GetKeychain(paintKit);
            return charm ? GameOrExtraImage(charm->image) : UI::CardImage{};
        }

        if (definition == Unboxer::DEF_PET)
            return ToCard(SkinImages::GetUiIcon("pet_adult"));

        if (const Cases::DropKind kind = Cases::KindOf(definition); kind != Cases::DropKind::Skin)
        {
            const Cases::Kit* kit = Cases::FindKit(kind, kind == Cases::DropKind::Pin ? definition : paintKit);
            return kit ? GameOrExtraImage(kit->image) : UI::CardImage{};
        }

        const ItemDefinitionInfo* item = Skins::g_ItemSchema.GetItem(definition);

        if (!item)
            return ToCard(SkinImages::GetInventoryImage(Cases::ItemImage(definition)));

        if (paintKit > 0)
        {
            if (const PaintKitInfo* kit = Skins::g_ItemSchema.GetPaintKit(paintKit))
            {
                if (const SkinImages::Image* image = SkinImages::GetPaintKitImage(*item, *kit))
                    return ToCard(image);
            }
        }
        else if (item->isKnife || definition == Unboxer::DEF_C4)
        {
            // a plain ("Vanilla") knife / the C4: its own picture (econ/weapons/base_weapons/...), like a gun - not the white icon
            if (const UI::CardImage image = ToCard(SkinImages::GetInventoryImage(Cases::ItemImage(definition))); image.texture)
                return image;
        }

        UI::CardImage image = ToCard((item->isKnife || item->isGlove) ? SkinImages::GetItemIcon(*item) : SkinImages::GetModelPreview(*item));

        if (!image.texture)
            image = ToCard(SkinImages::GetInventoryImage(Cases::ItemImage(definition)));   // egg, feed, anything else

        return image;
    }

    UI::CardImage CaseImage(const Cases::Case& item)
    {
        if (const UI::CardImage image = ToCard(SkinImages::GetInventoryImage(item.image)); image.texture)
            return image;

        // a picture CS2 doesn't ship (the Budapest 2025 Overpass Souvenir Package): Lootline's own copy
        const Cases::EmbeddedImage embedded = Cases::ExtraImage(item.image);
        return ToCard(SkinImages::GetEncodedImage(item.image, embedded.data, embedded.size));
    }

    UI::CardImage ItemImage(const Unboxer::Item& item)
    {
        if (item.kind == Unboxer::Kind::Case)
        {
            const Cases::Case* info = Cases::Find(item.definition);
            return info ? CaseImage(*info) : UI::CardImage{};
        }

        if (item.kind == Unboxer::Kind::Key)
        {
            const Cases::Key* key = Cases::FindKey(item.definition);
            return key ? ToCard(SkinImages::GetInventoryImage(key->image)) : UI::CardImage{};
        }

        if (item.definition == Unboxer::DEF_PET)
            return ToCard(SkinImages::GetUiIcon(item.petLevel <= 1 ? "pet_chick" : "pet_adult"));

        // a coloured graffiti: its (white) picture in the colour
        if (item.tint > 0 && Cases::KindOf(item.definition) == Cases::DropKind::Graffiti)
        {
            UI::CardImage image = DropImage(item.definition, item.paintKit);
            const std::uint32_t rgb = Cases::TintRgb(item.tint);
            image.tint = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
            return image;
        }

        // a Sticker Slab with a sticker sealed in: the sticker shows what it is
        if (item.definition == Unboxer::DEF_CHARM && item.charmSticker > 0)
            if (const UI::CardImage image = DropImage(Cases::DEF_STICKER, item.charmSticker); image.texture)
                return image;

        return DropImage(item.definition, item.paintKit);
    }

    // "AK-47 | Redline" (no wear) for lists
    // Doppler / Gamma Doppler: the game names every phase just "Doppler" - the phase from the finish's internal name.
    std::string PhaseOf(int paintKit)
    {
        const PaintKitInfo* kit = paintKit > 0 ? Skins::g_ItemSchema.GetPaintKit(paintKit) : nullptr;

        if (!kit)
            return {};

        const std::string& name = kit->name;

        for (const char phase : { '1', '2', '3', '4' })
            if (name.find(std::string("phase") + phase) != std::string::npos)
                return std::string("Phase ") + phase;

        if (name.find("ruby") != std::string::npos)
            return "Ruby";
        if (name.find("sapphire") != std::string::npos)
            return "Sapphire";
        if (name.find("blackpearl") != std::string::npos)
            return "Black Pearl";
        if (name.find("emerald") != std::string::npos)
            return "Emerald";

        return {};
    }

    std::string DropTitle(std::uint16_t weapon, int paintKit)
    {
        Unboxer::Item item;
        item.kind = Unboxer::Kind::Drop;
        item.definition = weapon;
        item.paintKit = paintKit;

        std::string name = Unboxer::DisplayName(item);
        const std::size_t wearStart = name.rfind(" (");

        if (wearStart != std::string::npos)
            name.resize(wearStart);

        if (const std::string phase = PhaseOf(paintKit); !phase.empty())
            name += " (" + phase + ")";

        return name;
    }

    // A big picture on its rarity's backdrop (the case / the picked drop).
    void Showcase(const UI::CardImage& image, float width, float height, ImU32 glow)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        ImDrawList* draw = ImGui::GetWindowDrawList();

        draw->AddRectFilled(min, max, UI::Col::Raised, S(10.0f));
        UI::RarityBackdrop(draw, min, max, glow, UI::Col::Raised, 1.0f, S(10.0f));

        // a soft dark shadow under the item (thin layers: no visible rings)
        const ImVec2 shadow(min.x + width * 0.5f, max.y - height * 0.14f);
        for (int layer = 0; layer < 10; ++layer)
        {
            const float t = static_cast<float>(layer) / 9.0f;
            draw->AddEllipseFilled(shadow, ImVec2(width * (0.42f - 0.30f * t), height * (0.09f - 0.065f * t)), IM_COL32(0, 0, 0, 9));
        }

        UI::DrawImage(draw, image, ImVec2(min.x + S(12.0f), min.y + S(10.0f)), ImVec2(max.x - S(12.0f), max.y - S(10.0f)));
        ImGui::Dummy(ImVec2(width, height));
    }

    // CS2's wears: the button, the full name, the float a button sets, and the wear's floats [from, to) (BS to 1.01:
    // 1.0 is Battle-Scarred too).
    struct WearPreset
    {
        const char* name;
        const char* full;
        float wear;
        float from;
        float to;
    };

    constexpr WearPreset WEAR_PRESETS[] = {
        { "FN", "Factory New", 0.01f, 0.00f, 0.07f }, { "MW", "Minimal Wear", 0.10f, 0.07f, 0.15f },
        { "FT", "Field-Tested", 0.25f, 0.15f, 0.38f }, { "WW", "Well-Worn", 0.41f, 0.38f, 0.45f },
        { "BS", "Battle-Scarred", 0.60f, 0.45f, 1.01f },
    };
    constexpr int WEAR_PRESET_COUNT = static_cast<int>(std::size(WEAR_PRESETS));

    // A skin's float: the number, typed (any value) - its wear named next to it - and the wear buttons FN ... BS (the one
    // it's in is gold; hover: the full name). True = changed.
    bool WearPicker(const char* id, float* wear)
    {
        const auto& presets = WEAR_PRESETS;

        ImGui::PushID(id);
        bool changed = false;

        char label[96];
        snprintf(label, sizeof(label), "%s  (%s)", tr("Float"), tr(Unboxer::WearName(*wear)));

        if (UI::FloatField("##float", label, wear, 0.0f, 1.0f, 0.001f, "%.7f"))
            changed = true;

        const float width = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4.0f) / 5.0f);

        for (int i = 0; i < static_cast<int>(std::size(presets)); ++i)
        {
            if (i)
                ImGui::SameLine();

            char button[48];
            snprintf(button, sizeof(button), "%s##wear_%d", presets[i].name, i);
            const bool current = *wear >= presets[i].from && *wear < presets[i].to;

            if (UI::Button(button, ImVec2(width, S(32.0f)), current ? UI::Kind::Primary : UI::Kind::Secondary))
            {
                *wear = presets[i].wear;
                changed = true;
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s  (%.2f - %.2f)", tr(presets[i].full), presets[i].from, std::min(presets[i].to, 1.0f));
        }

        ImGui::PopID();
        return changed;
    }

    // "Random float" for a finish: the picked wear's floats it really has (CS2's float ranges: a Doppler 0 - 0.08, an Asiimov
    // 0.18 - 1). A wear the finish doesn't have (a Factory New Asiimov): its float nearest to that wear. exists = it has it.
    Unboxer::RandomFloat RandomFloatRange(int wearIndex, int paintKit, bool* exists = nullptr)
    {
        const WearPreset& wear = WEAR_PRESETS[std::clamp(wearIndex, 0, WEAR_PRESET_COUNT - 1)];
        const Cases::WearRange finish = Cases::WearRangeOf(paintKit);
        const float wearTo = wear.to > 1.0f ? 1.0f : std::nextafter(wear.to, 0.0f);   // [from, to): 0.07 is MW
        float from = std::max(wear.from, finish.min);
        float to = std::min(wearTo, finish.max);
        const bool has = from <= to;

        if (!has)
            from = to = wear.from >= finish.max ? finish.max : finish.min;

        if (exists)
            *exists = has;

        return { true, from, to };
    }

    // A skin's float and pattern, each fixed or random (two small switches) - Add items (many = every one added gets its own)
    // and Choose (the picked skin, each time it comes). Random float: the wear buttons FN ... BS + the floats they give.
    // True = changed.
    bool RenderFloatAndSeed(const char* id, int paintKit, bool* randomFloat, float* wear, int* randomWear, bool* randomSeed, int* seed,
        bool many)
    {
        ImGui::PushID(id);
        bool changed = false;

        if (UI::Toggle("Random float##random_float", randomFloat))
            changed = true;

        if (!*randomFloat)
        {
            if (WearPicker("wear", wear))
                changed = true;
        }
        else
        {
            const float width = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4.0f) / 5.0f);

            for (int i = 0; i < WEAR_PRESET_COUNT; ++i)
            {
                if (i)
                    ImGui::SameLine();

                char button[48];
                snprintf(button, sizeof(button), "%s##random_wear_%d", WEAR_PRESETS[i].name, i);

                if (UI::Button(button, ImVec2(width, S(32.0f)), i == *randomWear ? UI::Kind::Primary : UI::Kind::Secondary) && i != *randomWear)
                {
                    *randomWear = i;
                    changed = true;
                }

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s  (%.2f - %.2f)", tr(WEAR_PRESETS[i].full), WEAR_PRESETS[i].from, std::min(WEAR_PRESETS[i].to, 1.0f));
            }

            bool exists = true;
            const Unboxer::RandomFloat range = RandomFloatRange(*randomWear, paintKit, &exists);
            char note[160];

            if (exists)
                snprintf(note, sizeof(note), tr(many ? "Each one gets its own float: %.4f - %.4f" : "A random float each time: %.4f - %.4f"),
                    range.from, range.to);
            else
                snprintf(note, sizeof(note), tr("This finish has no %s - the nearest it has: %.4f"),
                    tr(WEAR_PRESETS[std::clamp(*randomWear, 0, WEAR_PRESET_COUNT - 1)].full), range.from);

            UI::Note(note);
        }

        if (UI::Toggle("Random seed##random_seed", randomSeed))
            changed = true;

        if (!*randomSeed)
        {
            if (UI::Stepper("##seed", "Pattern (seed)", seed, 0, 1000))
                changed = true;
        }
        else
        {
            UI::Note(many ? "Each one gets its own pattern (0 - 1000)." : "A random pattern each time (0 - 1000).");
        }

        ImGui::PopID();
        return changed;
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
    // Cases page
    // ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════

    // The Cases tab's "Armory" tab (after the kinds of container, before Other): CS2's Armory with Lootline's stars.
    constexpr int ARMORY_KIND = static_cast<int>(Cases::Category::Count);

    void RenderArmorySettings();                // the Armory tab: the stars (below)
    void RenderArmoryRewards(float height);     // ... and what they buy, on the right (below)

    // The kinds of container as small tabs that wrap (only kinds the game has), the Armory before Other.
    void RenderCategoryTabs()
    {
        int counts[static_cast<int>(Cases::Category::Count)]{};
        for (const Cases::Case& item : Cases::All())
            ++counts[static_cast<int>(item.category)];

        if (g_caseKind != ARMORY_KIND && (g_caseKind < 0 || g_caseKind >= ARMORY_KIND || counts[g_caseKind] == 0))
        {
            for (int i = 0; i < static_cast<int>(Cases::Category::Count); ++i)
                if (counts[i]) { g_caseKind = i; break; }
        }

        // the order: the kinds of container, the Armory, Other
        int order[static_cast<int>(Cases::Category::Count) + 1]{};
        int shownCount = 0;
        for (int i = 0; i < static_cast<int>(Cases::Category::Count); ++i)
        {
            if (i == static_cast<int>(Cases::Category::Other))
                order[shownCount++] = ARMORY_KIND;
            order[shownCount++] = i;
        }

        const float available = ImGui::GetContentRegionAvail().x;
        const float spacing = S(6.0f);
        float x = 0.0f;
        bool first = true;

        for (int at = 0; at < shownCount; ++at)
        {
            const int i = order[at];

            if (i != ARMORY_KIND && !counts[i])
                continue;

            const char* name = i == ARMORY_KIND ? "Armory" : Cases::CategoryName(static_cast<Cases::Category>(i));
            const float width = ImGui::CalcTextSize(tr(name)).x + S(26.0f);

            if (!first)
            {
                if (x + spacing + width <= available)
                {
                    ImGui::SameLine(0.0f, spacing);
                    x += spacing + width;
                }
                else
                {
                    x = width;
                }
            }
            else
            {
                x = width;
            }

            char label[64];
            snprintf(label, sizeof(label), "%s##cat_%d", name, i);

            if (UI::Button(label, ImVec2(width, S(30.0f)), g_caseKind == i ? UI::Kind::Primary : UI::Kind::Secondary))
                g_caseKind = i;

            first = false;
        }
    }

    void RenderCaseList(float width, float height)
    {
        if (UI::BeginPanel("##case_list", ImVec2(width, height), "Containers"))
        {
            RenderCategoryTabs();

            if (g_caseKind == ARMORY_KIND)
            {
                UI::Gap(4.0f);
                RenderArmorySettings();
                UI::EndPanel();
                return;
            }

            UI::Search("##case_search", "Search cases...", &g_caseSearch);

            if (ImGui::BeginChild("##case_grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
            {
                if (!Cases::Ready())
                {
                    UI::Note("Reading the case list from the game...");
                }
                else
                {
                    UI::Grid grid = UI::MakeGrid(S(130.0f));

                    for (const Cases::Case& item : Cases::All())
                    {
                        if (static_cast<int>(item.category) != g_caseKind || !Matches(item.sortName, g_caseSearch))
                            continue;

                        UI::NextCell(grid);
                        char id[32];
                        snprintf(id, sizeof(id), "case_%u", static_cast<unsigned>(item.definition));

                        const std::string subtitle = std::to_string(item.drops.size()) + " " + tr(Cases::CategoryNoun(item.category)) + (item.hasGold ? "  \xE2\x98\x85" : "");

                        if (UI::Card(id, grid.width, item.displayName, subtitle, 0, g_selectedCase == item.definition, [&] { return CaseImage(item); }))
                            g_selectedCase = item.definition;
                    }

                    if (grid.index == 0)
                        UI::Note("Nothing found.");
                }
            }
            ImGui::EndChild();
        }
        UI::EndPanel();
    }

    void RenderCaseHero(const Cases::Case& item)
    {
        if (UI::BeginPanel("##case_hero"))
        {
            const float width = ImGui::GetContentRegionAvail().x;
            const float imageWidth = std::floor(std::min(width * 0.42f, S(260.0f)));
            Showcase(CaseImage(item), imageWidth, std::floor(imageWidth * 0.72f), UI::Col::Accent);

            ImGui::SameLine(0.0f, S(16.0f));
            ImGui::BeginGroup();

            ImGui::PushFont(UI::FontHeading, ImGui::GetStyle().FontSizeBase * 1.3f);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(item.displayName.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();

            if (item.key != 0)
            {
                const Cases::Key* key = Cases::FindKey(item.key);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s  %s", tr("Key:"), key ? key->displayName.c_str() : "?");

                // what CS2's Buy with Lootline keys has "cost" (Settings -> Prices -> Buy keys with Lootline)
                if (const Unboxer::KeySpending spent = Unboxer::GetKeySpending(); spent.keys > 0)
                {
                    char line[160];
                    snprintf(line, sizeof(line), tr("Spent on keys: %s (%d keys)"), Prices::Format(spent.dollars).c_str(), spent.keys);
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", line);
                }
            }
            else
            {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr("Opens without a key"));
            }

            UI::Chip((std::to_string(item.drops.size()) + " " + tr(Cases::CategoryNoun(item.category))).c_str(), UI::Col::TextDim);
            if (item.hasGold)
            {
                ImGui::SameLine();
                UI::Chip(tr("+ rare special item"), TierColor(Cases::TierGold));
            }

            UI::Gap(4.0f);
            UI::Stepper("##add_count", "How many", &g_addCount, 1, Unboxer::MAX_ADD_COUNT);

            if (item.key != 0)
                UI::Toggle("With keys##with_keys", &g_addWithKeys);

            const Unboxer::Status status = Unboxer::GetStatus();

            if (UI::Button("Add to Inventory##add_case", ImVec2(-1.0f, S(40.0f)), UI::Kind::Primary, status.inventoryReady))
            {
                Unboxer::AddCase(item.definition, g_addCount, item.key != 0 && g_addWithKeys);
                Notify::Push(std::string(tr("Added: ")) + item.displayName + " x" + std::to_string(g_addCount));
            }

            if (item.key != 0 && UI::Button("Keys Only##add_keys", ImVec2(-1.0f, 0.0f), UI::Kind::Secondary, status.inventoryReady))
            {
                Unboxer::AddKey(item.key, g_addCount);
                Notify::Push(std::string(tr("Added keys: x")) + std::to_string(g_addCount));
            }

            if (!status.inventoryReady)
                UI::Note("Your inventory isn't loaded yet - wait on the main menu until CS2 has connected.");

            ImGui::EndGroup();
        }
        UI::EndPanel();
    }

    void RenderOdds(const Cases::Case& item)
    {
        double odds[Cases::TierCount];
        Cases::TierOdds(item, odds);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const float rowHeight = ImGui::GetTextLineHeight() + S(8.0f);

        for (int tier = Cases::TierCount - 1; tier >= 0; --tier)
        {
            if (odds[tier] <= 0.0)
                continue;

            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImU32 color = TierColor(tier);
            const float labelWidth = S(150.0f);
            const float barWidth = width - labelWidth - S(70.0f);

            draw->AddCircleFilled(ImVec2(pos.x + S(5.0f), pos.y + rowHeight * 0.5f), S(4.0f), color);
            draw->AddText(ImVec2(pos.x + S(16.0f), pos.y + S(4.0f)), UI::Col::Text, tr(TierLabel(tier, HoldsSkins(item))));

            const ImVec2 barMin(pos.x + labelWidth, pos.y + rowHeight * 0.5f - S(3.0f));
            draw->AddRectFilled(barMin, ImVec2(barMin.x + barWidth, barMin.y + S(6.0f)), UI::Col::Raised, S(3.0f));
            const float fill = std::max(S(3.0f), static_cast<float>(barWidth * std::sqrt(odds[tier] / 100.0)));   // sqrt: the small ones stay visible
            draw->AddRectFilled(barMin, ImVec2(barMin.x + fill, barMin.y + S(6.0f)), color, S(3.0f));

            char percent[16];
            snprintf(percent, sizeof(percent), odds[tier] < 1.0 ? "%.2f%%" : "%.1f%%", odds[tier]);
            const float percentWidth = ImGui::CalcTextSize(percent).x;
            draw->AddText(ImVec2(pos.x + width - percentWidth, pos.y + S(4.0f)), UI::Col::TextDim, percent);

            ImGui::Dummy(ImVec2(width, rowHeight));
        }
    }

    // "Custom odds": a row per grade the container has - its line is a slider (drag; the scale is square-rooted like the
    // real odds' bars, so the small ones can be set too) and its number is typed. They don't have to make 100 %: the roll
    // scales them. Written back to Unboxer on every change.
    void RenderCustomOdds(const Cases::Case& item, Unboxer::TierOdds odds)
    {
        double real[Cases::TierCount];
        Cases::TierOdds(item, real);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const float rowHeight = std::max(ImGui::GetFrameHeight(), ImGui::GetTextLineHeight() + S(8.0f));
        const float labelWidth = S(150.0f);
        const float fieldWidth = S(76.0f);
        const float barWidth = std::max(S(40.0f), width - labelWidth - fieldWidth - S(14.0f));
        bool changed = false;
        double total = 0.0;

        for (int tier = Cases::TierCount - 1; tier >= 0; --tier)
        {
            if (real[tier] <= 0.0)
                continue;   // a grade this container doesn't have

            ImGui::PushID(tier);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImU32 color = TierColor(tier);

            draw->AddCircleFilled(ImVec2(pos.x + S(5.0f), pos.y + rowHeight * 0.5f), S(4.0f), color);
            draw->AddText(ImVec2(pos.x + S(16.0f), pos.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f), UI::Col::Text,
                tr(TierLabel(tier, HoldsSkins(item))));

            // the line: drag it
            ImGui::SetCursorScreenPos(ImVec2(pos.x + labelWidth, pos.y));
            ImGui::InvisibleButton("##odds_bar", ImVec2(barWidth, rowHeight));
            const bool hovered = ImGui::IsItemHovered();

            if (ImGui::IsItemActive())
            {
                const float t = std::clamp((ImGui::GetIO().MousePos.x - (pos.x + labelWidth)) / barWidth, 0.0f, 1.0f);
                const double value = std::round(static_cast<double>(t) * t * 10000.0) / 100.0;   // 0 - 100, 2 decimals

                if (value != odds[tier])
                {
                    odds[tier] = value;
                    changed = true;
                }
            }

            const ImVec2 barMin(pos.x + labelWidth, pos.y + rowHeight * 0.5f - S(3.0f));
            draw->AddRectFilled(barMin, ImVec2(barMin.x + barWidth, barMin.y + S(6.0f)), UI::Col::Raised, S(3.0f));
            const float fill = static_cast<float>(barWidth * std::sqrt(std::clamp(odds[tier], 0.0, 100.0) / 100.0));
            if (fill > 0.0f)
                draw->AddRectFilled(barMin, ImVec2(barMin.x + std::max(S(3.0f), fill), barMin.y + S(6.0f)), color, S(3.0f));
            draw->AddCircleFilled(ImVec2(barMin.x + fill, barMin.y + S(3.0f)), hovered ? S(7.0f) : S(6.0f), color);

            // the number: type it
            ImGui::SetCursorScreenPos(ImVec2(pos.x + width - fieldWidth, pos.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::SetNextItemWidth(fieldWidth);
            double typed = odds[tier];
            if (ImGui::InputDouble("##odds_value", &typed, 0.0, 0.0, "%.2f", ImGuiInputTextFlags_CharsDecimal))
            {
                typed = std::clamp(typed, 0.0, 100.0);

                if (typed != odds[tier])
                {
                    odds[tier] = typed;
                    changed = true;
                }
            }

            total += odds[tier];
            ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + rowHeight));
            ImGui::Dummy(ImVec2(width, ImGui::GetStyle().ItemSpacing.y * 0.5f));
            ImGui::PopID();
        }

        if (changed)
            Unboxer::SetCustomOdds(item.definition, odds);

        char note[160];
        if (total <= 0.0)
            snprintf(note, sizeof(note), "%s", tr("All 0 % - it rolls with the real odds."));
        else if (std::abs(total - 100.0) >= 0.005)
            snprintf(note, sizeof(note), tr("Together %.2f%% - the roll scales them to 100%%."), total);
        else
            note[0] = '\0';

        if (note[0])
            UI::Note(note);

        if (UI::Button("Back to the real odds##odds_reset", ImVec2(-1.0f, 0.0f), UI::Kind::Ghost))
        {
            Unboxer::TierOdds reset{};
            for (int tier = 0; tier < Cases::TierCount; ++tier)
                reset[tier] = real[tier];
            Unboxer::SetCustomOdds(item.definition, reset);
        }
    }

    // ── the pick ──
    void Pick(Unboxer::DropSettings& settings, bool& changed, std::uint16_t weapon, int paintKit, int tier)
    {
        settings.random = false;
        settings.weapon = weapon;
        settings.paintKit = paintKit;
        settings.tier = tier;
        changed = true;
    }

    bool IsGold(std::uint16_t definition)
    {
        const ItemDefinitionInfo* item = Skins::g_ItemSchema.GetItem(definition);
        return item && (item->isKnife || item->isGlove);
    }

    // Every knife / gloves type (★), knives first.
    const std::vector<const ItemDefinitionInfo*>& GoldTypes()
    {
        static std::vector<const ItemDefinitionInfo*> golds;   // render thread

        if (golds.empty())
        {
            for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
            {
                if ((item.isKnife && item.definitionIndex >= 500) || (item.isGlove && item.definitionIndex != 5028 && item.definitionIndex != 5029))
                    golds.push_back(&item);
            }

            std::sort(golds.begin(), golds.end(), [](const ItemDefinitionInfo* a, const ItemDefinitionInfo* b)
            {
                if (a->isKnife != b->isKnife)
                    return a->isKnife;
                return a->displayName < b->displayName;
            });
        }

        return golds;
    }

    // Every gun with finishes, by name.
    const std::vector<const ItemDefinitionInfo*>& Guns()
    {
        static std::vector<const ItemDefinitionInfo*> guns;   // render thread

        if (guns.empty())
        {
            for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
            {
                const std::vector<PaintKitInfo>* finishes = Skins::g_ItemSchema.GetPaintKitsForItem(item.definitionIndex);

                if (item.isWeapon && !item.isKnife && !item.isGlove && finishes && !finishes->empty())
                    guns.push_back(&item);
            }

            std::sort(guns.begin(), guns.end(), [](const ItemDefinitionInfo* a, const ItemDefinitionInfo* b) { return a->displayName < b->displayName; });
        }

        return guns;
    }

    // A gun finish's tier: its tier in any case that has it, else in its collection (map / operation / Armory ones have
    // no case), else the finish's own rarity (a grade off for some guns: the USP-S | Business Class finish is Industrial,
    // the gun Mil-Spec).
    int SkinTier(std::uint16_t weapon, const PaintKitInfo& finish)
    {
        static std::unordered_map<std::uint32_t, int> tiers;   // render thread, built once the cases are read
        static bool built = false;

        if (!built && Cases::Ready())
        {
            for (const Cases::Case& item : Cases::All())
                for (const Cases::Drop& drop : item.drops)
                    tiers.emplace((static_cast<std::uint32_t>(drop.weapon) << 16) | static_cast<std::uint16_t>(drop.paintKit), drop.tier);

            for (const Cases::Case& collection : Cases::SetCollections())
                for (const Cases::Drop& drop : collection.drops)
                    tiers.emplace((static_cast<std::uint32_t>(drop.weapon) << 16) | static_cast<std::uint16_t>(drop.paintKit), drop.tier);

            built = true;
        }

        if (const auto it = tiers.find((static_cast<std::uint32_t>(weapon) << 16) | static_cast<std::uint16_t>(finish.id)); it != tiers.end())
            return it->second;

        return std::clamp(finish.rarity - 1, static_cast<int>(Cases::TierConsumer), static_cast<int>(Cases::TierCovert));
    }

    using PickFn = std::function<void(std::uint16_t weapon, int paintKit, int tier)>;

    // A combo of item types (knives / gloves / guns) and the finishes of the chosen one as cards.
    //   types: what the combo lists; current: the chosen type (kept valid); selected*: the picked one (outlined);
    //   item: the case whose real drops get an "IN CASE" badge (null = no badges)
    //   typeCombo: false = no combo, only the finishes of `current` (it's picked elsewhere)
    //   sort: -1 = the game's order, 0 = A-Z, 1 = rarity (best first, then A-Z)
    // "Popular" order (Add items' sort switch): the item's market price - the famous / most wanted ones cost the most
    // (Katowice 2014 holos, Crown (Foil), Dragon Lore...). 0 = not on the market. Defined with the prices below.
    double PopularPrice(std::uint16_t definition, int paintKit);

    void RenderFinishPicker(const char* id, std::uint16_t selectedWeapon, int selectedPaintKit, const PickFn& onPick,
        const std::vector<const ItemDefinitionInfo*>& types, std::uint16_t& current, const Cases::Case* item, bool gold, const std::string& search = {},
        bool typeCombo = true, int sort = -1)
    {
        if (types.empty())
            return;

        if (std::none_of(types.begin(), types.end(), [&](const ItemDefinitionInfo* type) { return type->definitionIndex == current; }))
            current = types.front()->definitionIndex;

        const ItemDefinitionInfo* chosen = Skins::g_ItemSchema.GetItem(current);
        const std::string inCaseSuffix = std::string("   \xE2\x80\xA2 ") + tr("in this case");   // •
        const auto comboLabel = [&](const ItemDefinitionInfo* type)
        {
            const bool inCase = item && (gold ? Cases::GoldTypeFrom(*item, type->definitionIndex) :
                std::any_of(item->drops.begin(), item->drops.end(), [&](const Cases::Drop& drop) { return drop.weapon == type->definitionIndex; }));
            return type->displayName + (inCase ? inCaseSuffix : std::string());
        };

        char comboId[48];
        snprintf(comboId, sizeof(comboId), "##%s_type", id);
        ImGui::SetNextItemWidth(-FLT_MIN);

        if (typeCombo && ImGui::BeginCombo(comboId, chosen ? comboLabel(chosen).c_str() : "?", ImGuiComboFlags_HeightLarge))
        {
            for (const ItemDefinitionInfo* type : types)
            {
                if (ImGui::Selectable(comboLabel(type).c_str(), type->definitionIndex == current))
                    current = type->definitionIndex;
            }

            ImGui::EndCombo();
        }

        const std::vector<PaintKitInfo>* finishes = Skins::g_ItemSchema.GetPaintKitsForItem(current);

        if (!finishes || finishes->empty())
            return;

        UI::Grid grid = UI::MakeGridFitting(static_cast<int>(finishes->size()), S(108.0f), S(240.0f));
        const char* badge = tr("IN CASE");

        const auto finishName = [](const PaintKitInfo& finish) -> const std::string& { return finish.displayName.empty() ? finish.name : finish.displayName; };
        // a ★ finish's rarity is its paint's own (they're all the same tier); a gun's, its tier in the cases
        const auto finishRank = [&](const PaintKitInfo& finish) { return gold ? finish.rarity : SkinTier(current, finish); };

        std::vector<const PaintKitInfo*> order;
        order.reserve(finishes->size());
        for (const PaintKitInfo& finish : *finishes)
            order.push_back(&finish);

        if (sort >= 0)
        {
            // Popular: each finish's price once (Field-Tested, else its nearest wear)
            std::unordered_map<const PaintKitInfo*, double> popular;

            if (sort == 2)
                for (const PaintKitInfo* finish : order)
                    popular.emplace(finish, PopularPrice(current, finish->id));

            std::stable_sort(order.begin(), order.end(), [&](const PaintKitInfo* a, const PaintKitInfo* b)
            {
                if ((a->id == 0) != (b->id == 0))
                    return a->id == 0;   // a vanilla knife first

                if (sort == 1)
                    if (const int ra = finishRank(*a), rb = finishRank(*b); ra != rb)
                        return ra > rb;   // best first

                if (sort == 2)
                    if (const double pa = popular[a], pb = popular[b]; pa != pb)
                        return pa > pb;   // the most wanted first

                return Lower(finishName(*a)) < Lower(finishName(*b));
            });
        }

        for (const PaintKitInfo* finishPointer : order)
        {
            const PaintKitInfo& finish = *finishPointer;

            if (finish.id == 0 && (!gold || (chosen && chosen->isGlove)))
                continue;   // a plain gun isn't a case drop; plain gloves aren't an item

            const std::string name = finish.id == 0 ? std::string(tr("Vanilla")) : (finish.displayName.empty() ? finish.name : finish.displayName);

            if (!Matches(Lower(name), search))
                continue;

            UI::NextCell(grid);
            char cardId[64];
            snprintf(cardId, sizeof(cardId), "%s_%u_%d", id, static_cast<unsigned>(current), finish.id);

            const int tier = gold ? static_cast<int>(Cases::TierGold) : SkinTier(current, finish);
            const bool selected = selectedWeapon == current && selectedPaintKit == finish.id;
            const bool inCase = item && Cases::DropsFrom(*item, current, finish.id);
            const std::string subtitle = gold ? std::string("\xE2\x98\x85") : std::string(tr(Cases::TierName(static_cast<Cases::Tier>(tier))));

            if (UI::Card(cardId, grid.width, name, subtitle, TierColor(tier), selected, [&] { return DropImage(current, finish.id); }, inCase ? badge : nullptr))
                onPick(current, finish.id, tier);
        }

        if (grid.index == 0)
            UI::Note("Nothing found.");
    }

    void RenderNextDrop(const Cases::Case& item)
    {
        if (!UI::BeginPanel("##next_drop", ImVec2(0.0f, 0.0f), "Next Drop"))
        {
            UI::EndPanel();
            return;
        }

        Unboxer::DropSettings settings = Unboxer::GetDropSettings();
        bool changed = false;

        static const char* const modes[] = { "Random", "Choose" };
        int mode = settings.random ? 0 : 1;

        if (UI::Segmented("##drop_mode", modes, &mode))
        {
            settings.random = mode == 0;
            changed = true;
        }

        if (settings.random)
        {
            // real odds, or this container's own (a small switch; typed or dragged per grade, saved with the items)
            Unboxer::TierOdds custom{};
            bool customOn = Unboxer::GetCustomOdds(item.definition, custom);

            if (UI::Toggle("Custom odds##odds_custom", &customOn))
            {
                if (customOn)
                {
                    double real[Cases::TierCount];
                    Cases::TierOdds(item, real);

                    for (int tier = 0; tier < Cases::TierCount; ++tier)
                        custom[tier] = real[tier];

                    Unboxer::SetCustomOdds(item.definition, custom);   // starts from the real ones
                }
                else
                {
                    Unboxer::ClearCustomOdds(item.definition);
                }
            }

            if (customOn)
            {
                RenderCustomOdds(item, custom);
            }
            else
            {
                UI::Hint(!item.hasGold ? "Rolled from the container you open, with the real odds." : item.goldTypes.empty() ?
                    "Rolled from the case you open, with the real odds. A rare special item is any knife or gloves (the game doesn't say which ones this case has)." :
                    "Rolled from the case you open, with the real odds. A rare special item is one of this case's knives / gloves.");
                RenderOdds(item);
            }
        }
        else
        {
            // ── what's picked ──
            const bool gold = settings.weapon != 0 && IsGold(settings.weapon);
            const bool inCase = settings.weapon != 0 && Cases::DropsFrom(item, settings.weapon, settings.paintKit);

            if (settings.weapon != 0)
            {
                Unboxer::Item picked;
                picked.kind = Unboxer::Kind::Drop;
                picked.definition = settings.weapon;
                picked.paintKit = settings.paintKit;
                picked.wear = settings.randomFloat ? (settings.floatFrom + settings.floatTo) * 0.5f : settings.wear;
                picked.statTrak = settings.statTrak;

                const float size = S(150.0f);
                Showcase(DropImage(settings.weapon, settings.paintKit), size, size * 0.72f, TierColor(settings.tier));
                ImGui::SameLine(0.0f, S(14.0f));
                ImGui::BeginGroup();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr("You'll get"));
                ImGui::PushFont(UI::FontSemibold);
                ImGui::PushStyleColor(ImGuiCol_Text, TierColor(settings.tier));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(Unboxer::DisplayName(picked).c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::PopFont();

                if (inCase)
                    UI::Chip(tr("Drops from this case"), UI::Col::Success);
                else if (gold && item.hasGold && item.goldTypes.empty())
                    UI::Chip(tr("Could be in this case"), UI::Col::TextDim);
                else
                    UI::Chip(tr("Not in this case (prank)"), UI::Col::Warning);

                ImGui::EndGroup();
            }
            else
            {
                UI::Hint("Pick a skin below: from this case, any knife / gloves, or any skin at all.");
            }

            // ── wear, pattern, StatTrak ──
            const ItemDefinitionInfo* weapon = settings.weapon ? Skins::g_ItemSchema.GetItem(settings.weapon) : nullptr;
            const bool gloves = weapon && weapon->isGlove;
            const Cases::DropKind pickedKind = settings.weapon ? Cases::KindOf(settings.weapon) : Cases::DropKind::Skin;
            const bool skin = pickedKind == Cases::DropKind::Skin;
            const bool canStatTrak = (skin && !gloves) || pickedKind == Cases::DropKind::MusicKit;

            if (skin)
            {
                // the float and the pattern: fixed, or random each time it comes (two small switches)
                if (RenderFloatAndSeed("drop_skin", settings.paintKit, &settings.randomFloat, &settings.wear, &settings.randomWear,
                    &settings.randomSeed, &settings.seed, false))
                    changed = true;

                // the random float's range for the roll: the picked wear inside this finish's floats
                if (settings.randomFloat)
                {
                    const Unboxer::RandomFloat range = RandomFloatRange(settings.randomWear, settings.paintKit);

                    if (range.from != settings.floatFrom || range.to != settings.floatTo)
                    {
                        settings.floatFrom = range.from;
                        settings.floatTo = range.to;
                        changed = true;
                    }
                }
            }

            bool statTrak = settings.statTrak && canStatTrak;
            if (UI::Toggle("StatTrak##drop_st", &statTrak, canStatTrak))
            {
                settings.statTrak = statTrak;
                changed = true;
            }

            // ── when it drops ──
            char keepLabel[160];

            if (item.terminal)
            {
                // a terminal: which one, and which of its offers (the others are rolled)
                if (UI::Stepper("##terminal_on", "Comes in terminal number", &settings.terminalOn, 1, 1000))
                    changed = true;

                if (UI::Stepper("##offer_on", "As offer number", &settings.offerOn, 1, 5))
                    changed = true;

                char progress[256];
                if (settings.terminalOn <= 1)
                    snprintf(progress, sizeof(progress), tr("The next terminal you unseal offers it as offer %d of 5. The other offers are random (real odds)."),
                        settings.offerOn);
                else
                    snprintf(progress, sizeof(progress), tr("Next terminal: %d of %d. That one offers it as offer %d of 5; the other offers are random (real odds)."),
                        std::min(Unboxer::PickedTerminals(), settings.terminalOn - 1) + 1, settings.terminalOn, settings.offerOn);
                UI::Note(progress);

                if (settings.terminalOn <= 1)
                    snprintf(keepLabel, sizeof(keepLabel), "%s##drop_keep", tr("Every terminal (off: only the next one)"));
                else
                    snprintf(keepLabel, sizeof(keepLabel), tr("Again every %d terminals (off: only once)##drop_keep"), settings.terminalOn);
            }
            else
            {
                if (UI::Stepper("##drop_on", "Drops on opening number", &settings.dropOn, 1, 1000))
                    changed = true;

                if (settings.dropOn <= 1)
                {
                    UI::Hint("1 = the next case you open gives it.");
                }
                else
                {
                    char progress[256];
                    snprintf(progress, sizeof(progress), tr("Next case: %d of %d. The ones before the last give random drops (real odds)."),
                        std::min(Unboxer::PickedOpenings(), settings.dropOn - 1) + 1, settings.dropOn);
                    UI::Note(progress);
                }

                if (settings.dropOn <= 1)
                    snprintf(keepLabel, sizeof(keepLabel), "%s##drop_keep", tr("Every opening (off: only the next one)"));
                else
                    snprintf(keepLabel, sizeof(keepLabel), tr("Again every %d openings (off: only once)##drop_keep"), settings.dropOn);
            }

            if (UI::Toggle(keepLabel, &settings.keepPicked))
                changed = true;

            // ── make it look legit ──
            if (UI::Toggle("Show it in the case (contents + reel)##show_in_case", &settings.showInCase))
                changed = true;

            if (settings.weapon != 0)
            {
                if (!settings.showInCase)
                    UI::Hint("Off: you still get it, but the case's contents in CS2 stay as they are.");
                else if (inCase)
                    UI::Hint("It really drops from this case - it's in the contents anyway.");
                else if (gold && item.hasGold)
                    UI::Hint("Knives and gloves always show as the case's gold \"Rare Special Item\" - it looks legit already.");
                else
                    UI::Hint("CS2 will list it in the case's contents and spin it on the reel, like it belongs there (cases added with Lootline).");
            }

            // ── what to pick from ──
            const std::uint16_t pickedWeapon = settings.random ? 0 : settings.weapon;
            const PickFn pick = [&](std::uint16_t weapon, int paintKit, int tier) { Pick(settings, changed, weapon, paintKit, tier); };

            UI::SectionTitle("Pick an item");
            static const char* const categories[] = { "This case", "Knives & Gloves", "All skins" };
            UI::Segmented("##pick_category", categories, &g_pickCategory);
            UI::Gap(2.0f);

            if (g_pickCategory == 0)
            {
                UI::Grid grid = UI::MakeGrid(S(108.0f));

                for (std::size_t i = item.drops.size(); i-- > 0;)   // best first
                {
                    const Cases::Drop& drop = item.drops[i];
                    UI::NextCell(grid);
                    char id[48];
                    snprintf(id, sizeof(id), "drop_%u_%d", static_cast<unsigned>(drop.weapon), drop.paintKit);

                    const bool selected = settings.weapon == drop.weapon && settings.paintKit == drop.paintKit;

                    if (UI::Card(id, grid.width, DropTitle(drop.weapon, drop.paintKit), tr(TierLabel(drop.tier, Cases::KindOf(drop.weapon) == Cases::DropKind::Skin)), TierColor(drop.tier), selected,
                        [&] { return DropImage(drop.weapon, drop.paintKit); }))
                        Pick(settings, changed, drop.weapon, drop.paintKit, drop.tier);
                }

                // its knives / gloves: every finish that can drop from it, in the same grid
                for (const Cases::Drop& gold : item.golds)
                {
                    UI::NextCell(grid);
                    char id[48];
                    snprintf(id, sizeof(id), "drop_gold_%u_%d", static_cast<unsigned>(gold.weapon), gold.paintKit);

                    const bool selected = settings.weapon == gold.weapon && settings.paintKit == gold.paintKit;

                    if (UI::Card(id, grid.width, DropTitle(gold.weapon, gold.paintKit), tr(TierLabel(Cases::TierGold, true)), TierColor(Cases::TierGold), selected,
                        [&] { return DropImage(gold.weapon, gold.paintKit); }))
                        Pick(settings, changed, gold.weapon, gold.paintKit, Cases::TierGold);
                }

                if (item.hasGold && item.golds.empty())
                {
                    UI::SectionTitle("\xE2\x98\x85 This case's knives / gloves");

                    if (item.goldTypes.empty())
                    {
                        UI::Note("The game doesn't say which knives or gloves this case has (Valve keeps that list on its servers). Pick one in Knives & Gloves.");
                    }
                    else
                    {
                        std::vector<const ItemDefinitionInfo*> caseGolds;
                        for (const ItemDefinitionInfo* type : GoldTypes())
                            if (Cases::GoldTypeFrom(item, type->definitionIndex))
                                caseGolds.push_back(type);

                        RenderFinishPicker("case_gold", pickedWeapon, settings.paintKit, pick, caseGolds, g_caseGoldItem, &item, true);
                    }
                }
            }
            else if (g_pickCategory == 1)
            {
                RenderFinishPicker("gold", pickedWeapon, settings.paintKit, pick, GoldTypes(), g_goldItem, &item, true);
            }
            else
            {
                UI::Search("##skin_search", "Search finishes...", &g_skinSearch);
                RenderFinishPicker("gun", pickedWeapon, settings.paintKit, pick, Guns(), g_gunItem, &item, false, g_skinSearch);
            }
        }

        if (changed)
            Unboxer::SetDropSettings(settings);

        UI::EndPanel();
    }

    // Everything the case can give in one grid, best first - its knives / gloves too, every finish that can drop.
    void RenderCaseContents(const Cases::Case& item)
    {
        if (UI::BeginPanel("##case_contents", ImVec2(0.0f, 0.0f), "Contents"))
        {
            const bool skins = HoldsSkins(item);
            char summary[128];

            if (skins && !item.golds.empty())
                snprintf(summary, sizeof(summary), tr("Skins: %d  -  Knives / gloves: %d"), static_cast<int>(item.drops.size()), static_cast<int>(item.golds.size()));
            else
                snprintf(summary, sizeof(summary), "%d %s", static_cast<int>(item.drops.size()), tr(Cases::CategoryNoun(item.category)));

            UI::Note(summary);
            UI::Gap(2.0f);

            UI::Grid grid = UI::MakeGrid(S(108.0f));

            for (std::size_t i = item.drops.size(); i-- > 0;)   // best first
            {
                const Cases::Drop& drop = item.drops[i];
                UI::NextCell(grid);
                char id[48];
                snprintf(id, sizeof(id), "content_%u_%d", static_cast<unsigned>(drop.weapon), drop.paintKit);

                UI::Card(id, grid.width, DropTitle(drop.weapon, drop.paintKit), tr(TierLabel(drop.tier, Cases::KindOf(drop.weapon) == Cases::DropKind::Skin)),
                    TierColor(drop.tier), false, [&] { return DropImage(drop.weapon, drop.paintKit); });
            }

            for (const Cases::Drop& gold : item.golds)
            {
                UI::NextCell(grid);
                char id[48];
                snprintf(id, sizeof(id), "content_gold_%u_%d", static_cast<unsigned>(gold.weapon), gold.paintKit);

                UI::Card(id, grid.width, DropTitle(gold.weapon, gold.paintKit), tr(TierLabel(Cases::TierGold, true)), TierColor(Cases::TierGold), false,
                    [&] { return DropImage(gold.weapon, gold.paintKit); });
            }

            if (item.hasGold && item.golds.empty())
                UI::Note("+ a rare special item: which knives / gloves this case has isn't known.");
        }
        UI::EndPanel();
    }

    void RenderHowTo()
    {
        if (UI::BeginPanel("##how_to", ImVec2(0.0f, 0.0f), "How to open"))
        {
            UI::Hint("1) Add a case with its key.  2) In CS2 open Inventory, click the case, press Unlock.  3) The reel lands on your drop.");
            UI::Hint("These items only exist on this PC: they can't be traded, sold or used online. They come back every time you load Lootline.");

            const Unboxer::Status status = Unboxer::GetStatus();

            if (!status.lastDrop.empty())
            {
                UI::Gap(2.0f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr("Last opened:"));
                ImGui::SameLine();
                ImGui::PushFont(UI::FontSemibold);
                ImGui::TextUnformatted(status.lastDrop.c_str());
                ImGui::PopFont();
            }
        }
        UI::EndPanel();
    }
}

namespace
{
    // "1 star", "4 stars"; Russian: "1 звезда", "3 звезды", "5 звёзд"
    std::string StarCount(int n)
    {
        const bool russian = Config::menu.language == 1;
        const int last = n % 10;
        const int lastTwo = n % 100;
        const bool one = russian ? last == 1 && lastTwo != 11 : n == 1;
        const bool few = russian && last >= 2 && last <= 4 && (lastTwo < 12 || lastTwo > 14);
        return std::to_string(n) + " " + (one ? tr("star") : few ? tr("stars#few") : tr("stars"));
    }

    // Cases -> Armory (a tab next to the kinds of container): Lootline's stars for CS2's Armory (Store -> Armory) in the
    // list's place, this season's rewards (items_game "seasonaloperations") with what each costs on the right - gold when
    // the stars are enough.
    void RenderArmorySettings()
    {
        if (UI::Toggle("Armory with Lootline stars##armory_lootline", &Config::menu.armoryLootline))
            MenuState::MarkDirty();

        int stars = Unboxer::ArmoryStars();
        if (UI::Stepper("##armory_stars", "Stars", &stars, 0, 99999))
            Unboxer::SetArmoryStars(stars);

        const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
        if (UI::Button("+5 stars##armory_5", ImVec2(half, 0.0f), UI::Kind::Secondary))
            Unboxer::SetArmoryStars(stars + 5);

        ImGui::SameLine();
        if (UI::Button("+40 stars (a pass)##armory_40", ImVec2(-1.0f, 0.0f), UI::Kind::Secondary))
            Unboxer::SetArmoryStars(stars + 40);

        UI::Note("Spend them in CS2: Store -> Armory.");
        UI::Hint("CS2's Armory (Store -> Armory) shows your Lootline stars: redeem anything there - a collection, a case, stickers, charms - and you get it with the real odds, only on this PC. Off: Valve's Armory as it is.");
    }

    void RenderArmoryRewards(float height)
    {
        if (UI::BeginPanel("##armory_rewards", ImVec2(0.0f, height), "Rewards", true))
        {
            if (!Cases::Ready())
            {
                UI::Note("Reading the case list from the game...");
            }
            else if (Cases::ArmoryRewards().empty())
            {
                UI::Note("No Armory rewards in the game files.");
            }
            else
            {
                const int stars = Unboxer::ArmoryStars();
                const float spacing = ImGui::GetStyle().ItemSpacing.x;

                for (const Cases::ArmoryReward& reward : Cases::ArmoryRewards())
                {
                    const std::string& name = reward.displayName.empty() ? reward.itemName : reward.displayName;
                    const std::string cost = reward.bid ? std::string(tr("bids - not done by Lootline")) : StarCount(reward.points);
                    const bool enough = !reward.bid && stars >= reward.points;
                    const float start = ImGui::GetCursorPosX();
                    const float right = start + ImGui::GetContentRegionAvail().x;

                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(reward.bid ? UI::Col::TextFaint : UI::Col::Text));
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::PopStyleColor();

                    ImGui::SameLine(std::max(start + ImGui::CalcTextSize(name.c_str()).x + spacing, right - ImGui::CalcTextSize(cost.c_str()).x));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(enough ? UI::Col::Accent : UI::Col::TextDim));
                    ImGui::TextUnformatted(cost.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
        UI::EndPanel();
    }
}

void RenderCasesPage()
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float height = std::max(1.0f, available.y);
    const float listWidth = std::floor(available.x * 0.43f);

    RenderCaseList(listWidth, height);   // the Armory tab: its stars there
    ImGui::SameLine();

    // the Armory tab: what the stars buy
    if (g_caseKind == ARMORY_KIND)
    {
        RenderArmoryRewards(height);
        return;
    }

    if (ImGui::BeginChild("##case_side", ImVec2(0.0f, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
    {
        const Cases::Case* item = Cases::Ready() ? Cases::Find(g_selectedCase) : nullptr;

        if (!item && Cases::Ready() && !Cases::All().empty())
        {
            g_selectedCase = Cases::All().front().definition;
            item = &Cases::All().front();
        }

        if (item)
        {
            RenderCaseHero(*item);
            RenderNextDrop(*item);

            if (Unboxer::GetDropSettings().random)   // Choose already lists all of it ("This case")
                RenderCaseContents(*item);

            RenderHowTo();
        }
        else
        {
            UI::Note("Reading the case list from the game...");
        }
    }
    ImGui::EndChild();
}

namespace
{
    std::uint64_t g_itemsFocus = 0;   // the item shown in the Inventory tab's panel (its last left click)

    // the panel (defined with the Add items preview below); items = the whole inventory (a Storage Unit lists what's in it)
    void RenderInventoryPanel(const Unboxer::Item* item, const std::vector<Unboxer::Item>& items);
    // a card's stickers / charm as little icons along the bottom of its picture (defined below)
    void DrawCardOnIt(const Unboxer::Item& item, const ImVec2& cardMin, float cardWidth);
}

namespace
{
    bool g_askRemoveAll = false;   // "Remove Everything" clicked: the dialog opens
    constexpr ImU32 STORED_BADGE = IM_COL32(120, 172, 236, 255);   // the "IN UNIT" tag on a stored item's card

    // "Remove Everything": are you sure? - a dialog in the middle of the menu, the rest dimmed.
    void ConfirmRemoveAll(int count)
    {
        ImGui::SetNextWindowPos(MenuState::windowCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(S(460.0f), 0.0f), ImVec2(S(460.0f), FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(24.0f, 20.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, UI::Col::Panel);
        ImGui::PushStyleColor(ImGuiCol_Border, UI::WithAlpha(UI::Col::Danger, 0.6f));

        const bool open = ImGui::BeginPopupModal("##confirm_remove_all", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        if (!open)
            return;

        ImGui::PushFont(UI::FontHeading, ImGui::GetFontSize() * 1.2f);
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::Danger);
        ImGui::TextUnformatted(tr("Remove everything?"));
        ImGui::PopStyleColor();
        ImGui::PopFont();
        UI::Gap(4.0f);

        char text[320];
        snprintf(text, sizeof(text), tr("Are you sure? All %d items you added with Lootline will be removed from your inventory - skins, cases, keys, everything."), count);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        UI::Gap(2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::TextDim);
        ImGui::TextUnformatted(tr("Your real CS2 items stay. This can't be undone."));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        UI::Gap(10.0f);

        const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

        if (UI::Button("Cancel##cancel_remove", ImVec2(half, S(40.0f)), UI::Kind::Secondary) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();

        ImGui::SameLine();

        if (UI::Button("Yes, remove all##yes_remove", ImVec2(-1.0f, S(40.0f)), UI::Kind::Danger))
        {
            Unboxer::RemoveAll();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    bool g_openLoadouts = false;      // "Loadouts" clicked: the dialog opens
    char g_loadoutName[40] = {};

    // "Loadouts": what's equipped saved under a name, and the saved ones - put on / delete. A dialog in the middle of the menu.
    void LoadoutsDialog(int equippedCount)
    {
        ImGui::SetNextWindowPos(MenuState::windowCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(S(480.0f), 0.0f), ImVec2(S(480.0f), S(560.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(24.0f, 20.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, UI::Col::Panel);
        ImGui::PushStyleColor(ImGuiCol_Border, UI::WithAlpha(UI::Col::Accent, 0.5f));

        const bool open = ImGui::BeginPopupModal("##loadouts", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        if (!open)
            return;

        ImGui::PushFont(UI::FontHeading, ImGui::GetFontSize() * 1.2f);
        ImGui::TextUnformatted(tr("Loadouts"));
        ImGui::PopFont();
        UI::Note("Save what you have equipped and put it back on with one click. Only what's equipped - your items stay as they are.");
        UI::Gap(6.0f);

        // save what's equipped now
        const float saveWidth = S(150.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - saveWidth - ImGui::GetStyle().ItemSpacing.x);
        const bool entered = ImGui::InputTextWithHint("##loadout_name", tr("Name (e.g. Main, Red, Blue)"), g_loadoutName, sizeof(g_loadoutName),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const bool canSave = g_loadoutName[0] != '\0' && equippedCount > 0;

        if ((UI::Button("Save equipped##loadout_save", ImVec2(saveWidth, 0.0f), UI::Kind::Primary, canSave) || (entered && canSave)) &&
            Unboxer::SaveLoadout(g_loadoutName))
        {
            char text[96];
            snprintf(text, sizeof(text), "%s: %s", tr("Loadout saved"), g_loadoutName);
            Notify::Push(text);
            g_loadoutName[0] = '\0';
        }

        if (equippedCount == 0)
            UI::Note("Nothing of Lootline's is equipped right now.");

        UI::Gap(8.0f);

        const std::vector<Unboxer::LoadoutInfo> loadouts = Unboxer::Loadouts();

        if (loadouts.empty())
            UI::Note("No loadouts saved yet.");

        std::string remove;

        for (const Unboxer::LoadoutInfo& loadout : loadouts)
        {
            ImGui::PushID(loadout.name.c_str());
            const float putOnWidth = S(110.0f);
            const float deleteWidth = S(34.0f);
            const float rowStart = ImGui::GetCursorPosX();

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(loadout.name.c_str());
            ImGui::SameLine();

            char counts[96];
            if (loadout.missing > 0)
                snprintf(counts, sizeof(counts), tr("%d items, %d gone"), loadout.items, loadout.missing);
            else
                snprintf(counts, sizeof(counts), tr("%d items"), loadout.items);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", counts);

            ImGui::SameLine(rowStart + ImGui::GetContentRegionAvail().x - putOnWidth - deleteWidth - ImGui::GetStyle().ItemSpacing.x);
            if (UI::Button("Put on##loadout_apply", ImVec2(putOnWidth, 0.0f), UI::Kind::Secondary, loadout.items > 0))
            {
                const int equipped = Unboxer::ApplyLoadout(loadout.name);
                char text[120];
                snprintf(text, sizeof(text), tr("Loadout %s: %d items put on"), loadout.name.c_str(), equipped);
                Notify::Push(text);
            }

            ImGui::SameLine();
            if (UI::Button("x##loadout_delete", ImVec2(deleteWidth, 0.0f), UI::Kind::Ghost))
                remove = loadout.name;

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Delete this loadout"));

            ImGui::PopID();
        }

        if (!remove.empty())
            Unboxer::DeleteLoadout(remove);

        UI::Gap(10.0f);

        if (UI::Button("Close##loadouts_close", ImVec2(-1.0f, S(38.0f)), UI::Kind::Secondary) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

void RenderMyItemsPage()
{
    const std::vector<Unboxer::Item> items = Unboxer::Items();
    int cases = 0, keys = 0, drops = 0;

    for (const Unboxer::Item& item : items)
    {
        cases += item.kind == Unboxer::Kind::Case;
        keys += item.kind == Unboxer::Kind::Key;
        drops += item.kind == Unboxer::Kind::Drop;
    }

    // ── top: counts, filter, search, remove all ──
    if (UI::BeginPanel("##items_top"))
    {
        char text[64];
        snprintf(text, sizeof(text), "%d %s", cases, tr("cases"));
        UI::Chip(text, UI::Col::TextDim);
        ImGui::SameLine();
        snprintf(text, sizeof(text), "%d %s", keys, tr("keys"));
        UI::Chip(text, UI::Col::TextDim);
        ImGui::SameLine();
        snprintf(text, sizeof(text), "%d %s", drops, tr("opened"));
        UI::Chip(text, UI::Col::Accent);

        if (!g_itemsSelected.empty())
        {
            ImGui::SameLine();
            snprintf(text, sizeof(text), "%d %s", static_cast<int>(g_itemsSelected.size()), tr("selected"));
            UI::Chip(text, UI::Col::Text);
        }

        const float removeWidth = S(190.0f);
        const float loadoutsWidth = S(130.0f);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - removeWidth - loadoutsWidth - ImGui::GetStyle().ItemSpacing.x);
        if (UI::Button("Loadouts##loadouts", ImVec2(loadoutsWidth, 0.0f), UI::Kind::Secondary))
            g_openLoadouts = true;

        ImGui::SameLine();
        if (UI::Button("Remove Everything##remove_all", ImVec2(removeWidth, 0.0f), UI::Kind::Danger, !items.empty()))
            g_askRemoveAll = true;

        if (g_askRemoveAll)
        {
            ImGui::OpenPopup("##confirm_remove_all");
            g_askRemoveAll = false;
        }

        if (g_openLoadouts)
        {
            ImGui::OpenPopup("##loadouts");
            g_openLoadouts = false;
        }

        ConfirmRemoveAll(static_cast<int>(items.size()));

        int equippedCount = 0;
        for (const Unboxer::Item& item : items)
            equippedCount += item.kind == Unboxer::Kind::Drop && item.equipped != 0;

        LoadoutsDialog(equippedCount);

        static const char* const filters[] = { "All", "Cases", "Keys", "Opened", "Equipped", "Storage units" };
        UI::Segmented("##items_filter", filters, &g_itemsFilter);
        UI::Search("##items_search", "Search...", &g_itemsSearch);
        UI::Hint("Click an item: it shows on the right with its stickers and charm, and you equip it there (right-click: its menu).");
        UI::Hint("Ctrl+click starts selecting (Shift+click: everything in between); then any click checks / unchecks. Right-click a checked one -> Remove. Delete works too, Esc clears.");
    }
    UI::EndPanel();

    // what's on screen, in order (newest first): the Shift+click range and the selection go by this
    std::vector<const Unboxer::Item*> shown;
    std::vector<std::string> shownNames;
    shown.reserve(items.size());

    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        const Unboxer::Item& item = *it;

        if ((g_itemsFilter == 1 && item.kind != Unboxer::Kind::Case) || (g_itemsFilter == 2 && item.kind != Unboxer::Kind::Key) ||
            (g_itemsFilter == 3 && item.kind != Unboxer::Kind::Drop) || (g_itemsFilter == 4 && !item.equipped) ||
            (g_itemsFilter == 5 && item.definition != Unboxer::DEF_CASKET && item.casket == 0))
            continue;

        std::string name = Unboxer::DisplayName(item);

        if (!Matches(Lower(item.customName.empty() ? name : name + " " + item.customName), g_itemsSearch))   // its name tag finds it too
            continue;

        shown.push_back(&item);
        shownNames.push_back(std::move(name));
    }

    // only cards on screen stay selected (removed / filtered out ones drop out)
    std::erase_if(g_itemsSelected, [&](std::uint64_t id)
    {
        return std::none_of(shown.begin(), shown.end(), [&](const Unboxer::Item* item) { return item->id == id; });
    });

    const auto isSelected = [](std::uint64_t id) { return std::find(g_itemsSelected.begin(), g_itemsSelected.end(), id) != g_itemsSelected.end(); };
    const auto removeSelected = []
    {
        for (const std::uint64_t id : g_itemsSelected)
            Unboxer::Remove(id);

        g_itemsSelected.clear();
        g_itemsAnchor = 0;
    };

    // Delete: take the selected items out; Esc: forget the selection (not while typing or a menu is open)
    if (!g_itemsSelected.empty() && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            removeSelected();
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_itemsSelected.clear();
    }

    // the grid on the left, the clicked item on the right
    const Unboxer::Item* focus = nullptr;
    for (const Unboxer::Item& item : items)
        if (item.id == g_itemsFocus)
            focus = &item;

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float side = std::clamp(available.x * 0.34f, S(300.0f), S(440.0f));
    const float gridWidth = available.x - side - ImGui::GetStyle().ItemSpacing.x;

    if (ImGui::BeginChild("##items_grid", ImVec2(gridWidth, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
    {
        UI::Grid grid = UI::MakeGridFitting(static_cast<int>(shown.size()), S(140.0f), S(230.0f));

        for (std::size_t index = 0; index < shown.size(); ++index)
        {
            const Unboxer::Item& item = *shown[index];
            const std::string& name = shownNames[index];

            UI::NextCell(grid);
            char id[40];
            snprintf(id, sizeof(id), "item_%llu", static_cast<unsigned long long>(item.id));

            const char* kind = item.kind == Unboxer::Kind::Case ? tr("Case") : item.kind == Unboxer::Kind::Key ? tr("Key") : tr("Opened");
            const bool equippable = Unboxer::CanEquip(item);
            const bool musicKit = Cases::KindOf(item.definition) == Cases::DropKind::MusicKit;
            const bool pet = item.definition == Unboxer::DEF_PET;   // CS2's pet slot: one for both sides, like a music kit
            const bool spray = item.definition == Cases::DEF_SPRAY_PAINT;   // unsealed graffiti: CS2's "spray0", both sides too
            const bool c4 = item.definition == Unboxer::DEF_C4;   // the T side's only
            const bool pin = Cases::KindOf(item.definition) == Cases::DropKind::Pin;   // CS2's "flair0": both sides
            const ItemDefinitionInfo* itemInfo = Skins::g_ItemSchema.GetItem(item.definition);
            const bool agent = itemInfo && itemInfo->isAgent;
            // equipped: the side; in a Storage Unit (CS2 lists it inside the unit, not in the inventory): IN UNIT
            const bool stored = item.casket != 0;
            const char* badge = !item.equipped ? (stored ? tr("IN UNIT") : nullptr) : (musicKit || pet || spray || pin) ? tr("ON") :
                item.equipped == Unboxer::EQUIP_BOTH ? "T+CT" : item.equipped == Unboxer::EQUIP_T ? "T" : "CT";

            // a name tag: the name on top in quotes and the item under it, like CS2's inventory tile; a Storage Unit: how
            // many items are in it
            const bool named = !item.customName.empty();
            std::string subtitle = named ? name : std::string(kind);

            if (item.definition == Unboxer::DEF_CASKET)
                subtitle += "  (" + std::to_string(item.casketItems) + ")";

            const bool clicked = UI::Card(id, grid.width, named ? "\"" + item.customName + "\"" : name, subtitle, ItemColor(item),
                item.equipped != 0, [&] { return ItemImage(item); }, badge, stored && !item.equipped ? STORED_BADGE : UI::Col::Accent);
            const bool selected = isSelected(item.id);

            if (item.kind == Unboxer::Kind::Drop)
                DrawCardOnIt(item, ImGui::GetItemRectMin(), grid.width);

            // the one shown on the right: a thin outline
            if (item.id == g_itemsFocus && !selected)
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), UI::WithAlpha(UI::Col::Text, 0.55f), S(10.0f), 0, S(1.5f));

            if (selected)
            {
                // a gold tint and a check mark in the corner (the gold outline alone means "equipped")
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(min, max, UI::WithAlpha(UI::Col::Accent, 0.16f), S(10.0f));
                draw->AddRect(min, max, UI::Col::Text, S(10.0f), 0, S(2.0f));

                const ImVec2 center(max.x - S(17.0f), min.y + S(17.0f));   // top right: the T / CT badge is top left
                draw->AddCircleFilled(center, S(10.0f), UI::Col::Accent);
                const ImVec2 check[] = { ImVec2(center.x - S(4.5f), center.y), ImVec2(center.x - S(1.0f), center.y + S(3.5f)), ImVec2(center.x + S(5.0f), center.y - S(3.5f)) };
                draw->AddPolyline(check, 3, UI::Col::OnAccent, ImDrawFlags_None, S(2.0f));
            }

            if (clicked)
            {
                const ImGuiIO& io = ImGui::GetIO();

                if (io.KeyCtrl || (!io.KeyShift && !g_itemsSelected.empty()))
                {
                    // Ctrl+click, or any click once you're selecting: the check mark on / off
                    if (selected)
                        std::erase(g_itemsSelected, item.id);
                    else
                        g_itemsSelected.push_back(item.id);

                    g_itemsAnchor = item.id;
                }
                else if (io.KeyShift)
                {
                    // everything from the last clicked card to this one
                    std::size_t from = index;
                    for (std::size_t i = 0; i < shown.size(); ++i)
                        if (shown[i]->id == g_itemsAnchor)
                            from = i;

                    for (std::size_t i = std::min(from, index); i <= std::max(from, index); ++i)
                        if (!isSelected(shown[i]->id))
                            g_itemsSelected.push_back(shown[i]->id);

                    if (g_itemsAnchor == 0)
                        g_itemsAnchor = item.id;
                }
                else
                {
                    g_itemsAnchor = item.id;   // nothing selected: shown on the right (its buttons are there)
                    g_itemsFocus = item.id;
                }
            }

            if (ImGui::IsItemHovered())
            {
                if (g_itemsSelected.empty())
                    ImGui::SetTooltip("%s\n%s\n%s", name.c_str(), tr(equippable ? "Click: show it - right-click: equip or take it out" : "Click: show it - right-click: take it out"),
                        tr("Ctrl+click: select  -  Shift+click: select a range"));
                else
                    ImGui::SetTooltip("%s\n%s", name.c_str(), tr("Click: check / uncheck  -  Right-click a checked one: remove"));
            }

            if (ImGui::BeginPopupContextItem(id))
            {
                // right-click on a selected one: the menu is for all of them
                if (!g_itemsSelected.empty() && isSelected(item.id))
                {
                    char line[64];
                    snprintf(line, sizeof(line), tr("%d selected"), static_cast<int>(g_itemsSelected.size()));
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", line);

                    snprintf(line, sizeof(line), tr("Remove %d selected"), static_cast<int>(g_itemsSelected.size()));
                    if (ImGui::MenuItem(line))
                        removeSelected();

                    if (ImGui::MenuItem(tr("Clear selection")))
                        g_itemsSelected.clear();

                    ImGui::EndPopup();
                    continue;
                }

                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", name.c_str());

                if (equippable)
                {
                    if (musicKit || pet || spray || pin || agent || c4)
                    {
                        if (ImGui::MenuItem(tr("Equip"), nullptr, item.equipped != 0))
                            Unboxer::Equip(item.id, item.equipped ? 0 : Unboxer::EQUIP_BOTH);
                    }
                    else
                    {
                        if (ImGui::MenuItem(tr("Equip on T"), nullptr, item.equipped == Unboxer::EQUIP_T))
                            Unboxer::Equip(item.id, Unboxer::EQUIP_T);
                        if (ImGui::MenuItem(tr("Equip on CT"), nullptr, item.equipped == Unboxer::EQUIP_CT))
                            Unboxer::Equip(item.id, Unboxer::EQUIP_CT);
                        if (ImGui::MenuItem(tr("Equip on both sides"), nullptr, item.equipped == Unboxer::EQUIP_BOTH))
                            Unboxer::Equip(item.id, Unboxer::EQUIP_BOTH);
                        if (item.equipped && ImGui::MenuItem(tr("Take off")))
                            Unboxer::Equip(item.id, 0);
                    }

                    ImGui::Separator();
                }

                if (ImGui::MenuItem(tr("Remove from inventory")))
                {
                    Unboxer::Remove(item.id);
                    std::erase(g_itemsSelected, item.id);
                }

                ImGui::EndPopup();
            }
        }

        if (items.empty())
        {
            UI::Gap(20.0f);
            UI::Note("Nothing yet - add a case on the Cases tab.");
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("##items_side", ImVec2(0.0f, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        RenderInventoryPanel(focus, items);
    ImGui::EndChild();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// Upgrader page
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // ── render thread state ──
    std::vector<std::uint64_t> g_upInputs;        // the drops being put in (their prices add up)
    float g_upTargetWear = 0.0f;                  // the float the target comes with (typed in the stage)
    int g_upTargetWearFor = -1;                   // the target it was rolled for
    int g_upTarget = -1;                          // index into g_catalog
    int g_upMultiplier = 1;                       // index into MULTIPLIERS
    std::string g_upSearch;
    std::string g_upItemsSearch;
    std::uint64_t g_upSelectNewerThan = 0;        // after a win: pick the new item as the next input
    std::mt19937 g_upRng{ std::random_device{}() };

    constexpr double MULTIPLIERS[] = { 1.5, 2.0, 3.0, 5.0, 10.0, 20.0 };
    constexpr const char* MULTIPLIER_LABELS[] = { "x1.5", "x2", "x3", "x5", "x10", "x20" };
    constexpr std::size_t TARGETS_SHOWN = 18;
    constexpr std::size_t MAX_UPGRADE_INPUTS = 6; // "Up to 6 items at once"
    constexpr std::size_t SEARCH_SHOWN = 60;
    constexpr double HOUSE_EDGE = 0.05;           // like upgrade sites: a fair x2 is 47.5 %, not 50 %

    // the wheel
    constexpr std::uint64_t SPIN_MS = 4600;
    constexpr float PI = 3.14159265358979f;
    constexpr float TOP = -PI * 0.5f;             // the win zone starts at 12 o'clock
    bool g_spinning = false;
    std::uint64_t g_spinStart = 0;
    float g_marker = TOP;                         // radians, screen space (clockwise)
    float g_spinFrom = TOP;
    float g_spinTo = TOP;
    bool g_spinWin = false;
    std::string g_spinTarget;
    int g_result = 0;                             // after a spin: 1 won, -1 lost

    // The market's wear brackets.
    struct WearBracket
    {
        const char* shortName;
        float from;
        float to;
    };

    constexpr WearBracket WEARS[] = { { "FN", 0.00f, 0.07f }, { "MW", 0.07f, 0.15f }, { "FT", 0.15f, 0.38f }, { "WW", 0.38f, 0.45f }, { "BS", 0.45f, 1.00f } };

    float MidWear(int wear)
    {
        return wear < 0 ? 0.0f : (WEARS[wear].from + std::min(WEARS[wear].to, 0.8f)) * 0.5f;
    }

    // Everything you can upgrade to: every gun finish / knife / gloves in every wear (and StatTrak) the market lists,
    // with its price. Without prices (offline): one entry per finish with an estimate.
    struct CatalogEntry
    {
        std::uint16_t weapon = 0;
        int paintKit = 0;
        int tier = 0;
        int wear = -1;                            // WEARS index, -1 = none (vanilla knife)
        bool statTrak = false;
        double price = 0.0;
        std::string title;                        // "StatTrak™ AK-47 | Redline"
        std::string search;                       // lowercase title + wear
    };

    std::vector<CatalogEntry> g_catalog;
    int g_catalogBuiltFor = -100;                 // Prices::Generation() it was built with, -1 = estimates

    std::uint64_t NowMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Estimate when the market doesn't list it: each tier is worth 5x the one below (like the drop odds).
    double Estimate(int tier, float wear, bool statTrak)
    {
        static const double values[] = { 0.03, 0.08, 0.25, 1.25, 6.25, 31.25, 156.25 };
        const double wearFactor = wear < 0.07f ? 1.0 : wear < 0.15f ? 0.85 : wear < 0.38f ? 0.7 : wear < 0.45f ? 0.6 : 0.5;
        return values[std::clamp(tier, 0, static_cast<int>(std::size(values)) - 1)] * wearFactor * (statTrak ? 1.8 : 1.0);
    }

    int ItemTier(const Unboxer::Item& item)
    {
        if (item.quality == 3 || IsGold(item.definition))
            return Cases::TierGold;

        return std::clamp(item.rarity - 1, 0, static_cast<int>(Cases::TierCovert));
    }

    int WearIndex(float wear)
    {
        for (int i = 0; i < static_cast<int>(std::size(WEARS)) - 1; ++i)
            if (wear < WEARS[i].to)
                return i;

        return static_cast<int>(std::size(WEARS)) - 1;
    }

    // What the market says an item is worth. exact = false: it isn't sold exactly like this (a StatTrak Dragon Lore,
    // a skin in a wear it never comes in, a graffiti without its colour...) and the nearest real item's price is used.
    struct MarketPrice
    {
        double price = -1.0;   // < 0: nothing close on the market either
        bool exact = false;
    };

    // ── stickers Valve never released ──
    // items_game has sticker finishes that were never sold (most old Majors' gold / foil autographs, the Katowice 2014
    // foils): the market has no price. They're priced from the same team's / player's real sticker of that event in
    // another finish, times how those two finishes compare - at that event (its median prices), else over every sticker
    // the market has in both finishes ("titan (foil) | katowice 2014" = its holo x 0.4).
    constexpr const char* STICKER_FINISHES[] = { "holo", "foil", "gold", "glitter", "lenticular" };

    struct StickerName
    {
        std::string who;
        std::string finish;     // "" = paper
        std::string event;
    };

    bool SplitStickerName(const std::string& english, StickerName& out)
    {
        const std::size_t bar = english.rfind(" | ");

        if (bar == std::string::npos)
            return false;

        out.event = english.substr(bar + 3);
        out.who = english.substr(0, bar);
        out.finish.clear();

        if (!out.who.empty() && out.who.back() == ')')
        {
            if (const std::size_t open = out.who.rfind(" ("); open != std::string::npos)
            {
                const std::string finish = out.who.substr(open + 2, out.who.size() - open - 3);

                if (std::any_of(std::begin(STICKER_FINISHES), std::end(STICKER_FINISHES), [&](const char* known) { return finish == known; }))
                {
                    out.finish = finish;
                    out.who.resize(open);
                }
            }
        }

        return !out.who.empty() && !out.event.empty();
    }

    std::string StickerMarketName(const std::string& who, const std::string& finish, const std::string& event)
    {
        return "sticker | " + who + (finish.empty() ? std::string() : " (" + finish + ")") + " | " + event;
    }

    double Median(std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        const std::size_t count = values.size();
        return count % 2 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) * 0.5;
    }

    struct StickerFinishTable
    {
        int generation = -100;
        std::unordered_map<std::string, double> eventMedian;   // "event|finish" -> its median price (3+ stickers)
        std::unordered_map<std::string, double> ratio;         // "a|b" -> median of a / b over stickers sold in both (3+)
    };

    StickerFinishTable g_stickerFinishes;   // render thread, made again for each price list

    const StickerFinishTable& StickerFinishes()
    {
        const int generation = Prices::Generation();

        if (g_stickerFinishes.generation == generation)
            return g_stickerFinishes;

        StickerFinishTable table;
        table.generation = generation;
        std::unordered_map<std::string, std::unordered_map<std::string, double>> bySticker;   // "who|event" -> finish -> price
        std::unordered_map<std::string, std::vector<double>> byEvent;                         // "event|finish" -> prices

        for (const auto& [name, price] : Prices::WithPrefix("sticker | "))
        {
            StickerName parts;

            if (!SplitStickerName(name.substr(10), parts))
                continue;

            bySticker[parts.who + "|" + parts.event][parts.finish] = price;
            byEvent[parts.event + "|" + parts.finish].push_back(price);
        }

        for (auto& [key, prices] : byEvent)
            if (prices.size() >= 3)
                table.eventMedian[key] = Median(std::move(prices));

        std::unordered_map<std::string, std::vector<double>> ratios;

        for (const auto& [sticker, finishes] : bySticker)
            for (const auto& [a, priceA] : finishes)
                for (const auto& [b, priceB] : finishes)
                    if (a != b && priceB > 0.0)
                        ratios[a + "|" + b].push_back(priceA / priceB);

        for (auto& [key, values] : ratios)
            if (values.size() >= 3)
                table.ratio[key] = Median(std::move(values));

        g_stickerFinishes = std::move(table);
        return g_stickerFinishes;
    }

    double UnreleasedStickerPrice(const std::string& english)
    {
        StickerName name;

        if (!SplitStickerName(english, name))
            return -1.0;

        // the closest real finish first (a foil from its holo, a gold from its foil...), paper last
        const auto siblings = [](const std::string& finish) -> std::array<const char*, 5>
        {
            if (finish == "holo")
                return { "foil", "glitter", "lenticular", "gold", "" };
            if (finish == "foil")
                return { "holo", "gold", "glitter", "lenticular", "" };
            if (finish == "gold")
                return { "foil", "holo", "glitter", "lenticular", "" };
            if (finish == "glitter")
                return { "holo", "foil", "lenticular", "gold", "" };
            if (finish == "lenticular")
                return { "holo", "foil", "glitter", "gold", "" };
            return { "holo", "foil", "glitter", "gold", "lenticular" };
        };

        const StickerFinishTable& table = StickerFinishes();

        for (const char* other : siblings(name.finish))
        {
            const double sibling = Prices::Find(StickerMarketName(name.who, other, name.event));

            if (sibling <= 0.0)
                continue;

            double ratio = 1.0;
            const auto mine = table.eventMedian.find(name.event + "|" + name.finish);
            const auto theirs = table.eventMedian.find(name.event + "|" + other);

            if (mine != table.eventMedian.end() && theirs != table.eventMedian.end() && theirs->second > 0.0)
                ratio = mine->second / theirs->second;
            else if (const auto all = table.ratio.find(name.finish + "|" + other); all != table.ratio.end())
                ratio = all->second;

            return sibling * ratio;
        }

        return -1.0;
    }

    MarketPrice LookUpPrice(std::uint16_t definition, int paintKit, float wear, bool statTrak, bool souvenir)
    {
        const Cases::DropKind kind = Cases::KindOf(definition);
        bool phaseGuessed = false;   // a Doppler phase without a price of its own (Prices::FindPhased)
        const auto find = [&](float atWear, bool asStatTrak, bool asSouvenir)
        {
            return Prices::FindPhased(Cases::MarketName(definition, paintKit, atWear, asStatTrak, asSouvenir), paintKit, phaseGuessed);
        };

        // 1. exactly this item
        if (const double price = find(wear, statTrak, souvenir); price > 0.0)
            return { price, !phaseGuessed };

        // 2. the same item without StatTrak / Souvenir (a StatTrak Dragon Lore isn't sold anywhere: it's a Dragon Lore)
        if (statTrak || souvenir)
            if (const double price = find(wear, false, false); price > 0.0)
                return { price, false };

        // 3. a skin in a wear the market doesn't have: the nearest wear it has (as it is first, then plain)
        if (kind == Cases::DropKind::Skin && paintKit > 0)
        {
            const int own = WearIndex(wear);

            for (int distance = 1; distance < static_cast<int>(std::size(WEARS)); ++distance)
                for (const int other : { own - distance, own + distance })
                {
                    if (other < 0 || other >= static_cast<int>(std::size(WEARS)))
                        continue;

                    if (const double price = find(MidWear(other), statTrak, souvenir); price > 0.0)
                        return { price, false };

                    if (statTrak || souvenir)
                        if (const double price = find(MidWear(other), false, false); price > 0.0)
                            return { price, false };
                }

            // a skin the market only has as a Souvenir (from a souvenir package), nearest wear first
            if (!souvenir)
                for (int distance = 0; distance < static_cast<int>(std::size(WEARS)); ++distance)
                    for (const int other : { own - distance, own + distance })
                        if (other >= 0 && other < static_cast<int>(std::size(WEARS)))
                            if (const double price = find(MidWear(other), false, true); price > 0.0)
                                return { price, false };
        }

        // 4. a music kit only sold as StatTrak (or only plain)
        if (kind == Cases::DropKind::MusicKit)
            if (const double price = find(wear, !statTrak, false); price > 0.0)
                return { price, false };

        // 5. graffiti: the market sells them by colour ("sealed graffiti | 1g (brick red)") - the cheapest colour
        if (kind == Cases::DropKind::Graffiti)
            if (const std::string name = Cases::MarketName(definition, paintKit, wear, false); !name.empty())
                if (const double price = Prices::FindLowest(name + " ("); price > 0.0)
                    return { price, false };

        // 6. a charm the market only sells as its highlights (a Major's highlight charm): its cheapest highlight
        if (definition == Cases::DEF_KEYCHAIN)
            if (const std::string name = Cases::MarketName(definition, paintKit, wear, false); name.starts_with("charm | "))
                if (const double price = Prices::FindLowest("souvenir charm | " + name.substr(8) + " | "); price > 0.0)
                    return { price, false };

        // 7. a sticker Valve never released: from the same sticker's real finishes (UnreleasedStickerPrice)
        if (kind == Cases::DropKind::Sticker)
            if (const Cases::Kit* kit = Cases::FindKit(kind, paintKit); kit && !kit->english.empty())
                if (const double price = UnreleasedStickerPrice(kit->english); price > 0.0)
                    return { price, false };

        return {};
    }

    // Looked-up prices (the upgrader asks for every card, every frame), per item kind / finish / wear bracket /
    // StatTrak / Souvenir. Emptied when another price list is loaded. Render thread.
    std::unordered_map<std::uint64_t, MarketPrice> g_marketPrices;
    int g_marketPricesFor = -100;

    MarketPrice CachedPrice(std::uint16_t definition, int paintKit, float wear, bool statTrak, bool souvenir)
    {
        if (const int generation = Prices::Ready() ? Prices::Generation() : -1; generation != g_marketPricesFor)
        {
            g_marketPrices.clear();
            g_marketPricesFor = generation;
        }

        const std::uint64_t key = (static_cast<std::uint64_t>(definition) << 40) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(paintKit) & 0xFFFFFFu) << 16) |
            (static_cast<std::uint64_t>(WearIndex(wear)) << 2) | (statTrak ? 2u : 0u) | (souvenir ? 1u : 0u);

        if (const auto it = g_marketPrices.find(key); it != g_marketPrices.end())
            return it->second;

        const MarketPrice price = Prices::Ready() ? LookUpPrice(definition, paintKit, wear, statTrak, souvenir) : MarketPrice{};
        g_marketPrices.emplace(key, price);
        return price;
    }

    // Graffiti for "Popular": the market sells them by colour ("sealed graffiti | 1g (brick red)") - each graffiti's
    // cheapest colour, from one pass over the list (made again when another price list is loaded). Render thread.
    std::unordered_map<std::string, double> g_graffitiLowest;
    int g_graffitiLowestFor = -100;

    double PopularPrice(std::uint16_t definition, int paintKit)
    {
        if (!Prices::Ready())
            return 0.0;

        if (Cases::KindOf(definition) == Cases::DropKind::Graffiti)
        {
            if (g_graffitiLowestFor != Prices::Generation())
            {
                g_graffitiLowestFor = Prices::Generation();
                g_graffitiLowest.clear();

                for (const auto& [name, price] : Prices::WithPrefix("sealed graffiti | "))
                {
                    const std::size_t colour = name.rfind(" (");
                    const std::string base = colour == std::string::npos ? name : name.substr(0, colour);
                    const auto it = g_graffitiLowest.find(base);

                    if (it == g_graffitiLowest.end() || price < it->second)
                        g_graffitiLowest[base] = price;
                }
            }

            const auto it = g_graffitiLowest.find(Cases::MarketName(definition, paintKit, 0.0f, false));
            return it != g_graffitiLowest.end() ? it->second : 0.0;
        }

        const MarketPrice price = CachedPrice(definition, paintKit, MidWear(2), false, false);   // a skin: Field-Tested (or its nearest)
        return std::max(0.0, price.price);
    }

    // An item's price: the market's (or the nearest real item's), else the estimate. estimated = not its exact price.
    double ItemPrice(const Unboxer::Item& item, bool& estimated)
    {
        // a Sticker Slab: "sticker slab | <its sticker>", else the sticker's own price
        if (item.definition == Unboxer::DEF_CHARM && item.charmSticker > 0 && Prices::Ready())
        {
            if (const Cases::Kit* kit = Cases::FindKit(Cases::DropKind::Sticker, item.charmSticker); kit && !kit->english.empty())
            {
                if (const double price = Prices::Find("sticker slab | " + kit->english); price > 0.0)
                {
                    estimated = false;
                    return price;
                }
            }

            if (const MarketPrice sticker = CachedPrice(Cases::DEF_STICKER, item.charmSticker, 0.0f, false, false); sticker.price > 0.0)
            {
                estimated = true;
                return sticker.price;
            }
        }

        // a Major highlight charm's clip: "souvenir charm | austin 2025 highlight | ace unleashed"
        if (item.definition == Unboxer::DEF_CHARM && Prices::Ready())
        {
            const int number = item.highlight > 0 ? item.highlight : Cases::HighlightOf(item.paintKit);

            if (const Cases::Highlight* clip = number > 0 ? Cases::FindHighlight(number) : nullptr; clip && !clip->english.empty())
            {
                if (const double price = Prices::Find(clip->english); price > 0.0)
                {
                    estimated = false;
                    return price;
                }
            }
        }

        // a coloured graffiti: "sealed graffiti | 1g (battle green)" - that colour's own price
        if (item.tint > 0 && Prices::Ready() && Cases::KindOf(item.definition) == Cases::DropKind::Graffiti)
        {
            if (const std::string name = Cases::MarketName(item.definition, item.paintKit, item.wear, false); !name.empty())
            {
                if (const double price = Prices::Find(name + " (" + Lower(Cases::TintName(item.tint)) + ")"); price > 0.0)
                {
                    estimated = false;
                    return price;
                }
            }
        }

        const MarketPrice market = CachedPrice(item.definition, item.paintKit, item.wear, item.statTrak, item.quality == 12);
        estimated = !market.exact;
        return market.price > 0.0 ? market.price : Estimate(ItemTier(item), item.wear, item.statTrak);
    }

    double Chance(double inputValue, double targetValue)
    {
        if (inputValue <= 0.0 || targetValue <= 0.0)
            return 0.0;

        return std::clamp(inputValue / targetValue * (1.0 - HOUSE_EDGE), 0.001, 0.95);
    }

    // A price (USD) in the currency picked in Settings -> Prices.
    std::string Money(double value)
    {
        return Prices::Format(value);
    }

    // "StatTrak™ AK-47 | Redline" — the display name without the wear.
    std::string TitleOf(const Unboxer::Item& item)
    {
        std::string name = Unboxer::DisplayName(item);
        const std::size_t wearStart = name.rfind(" (");
        return wearStart != std::string::npos && item.paintKit > 0 && Cases::KindOf(item.definition) == Cases::DropKind::Skin ? name.substr(0, wearStart) : name;
    }

    // The catalog is made a few ms a frame: ~27 000 price lookups (and names) at once froze the game when the upgrader
    // was opened. The previous one (or none, "Reading the prices...") stays until the new one is complete.
    struct CatalogJob
    {
        std::uint16_t weapon = 0;
        const PaintKitInfo* finish = nullptr;
        int tier = 0;
        bool statTrakToo = true;      // gloves have no StatTrak
    };

    std::vector<CatalogJob> g_catalogJobs;
    std::vector<CatalogEntry> g_catalogNext;     // being made
    std::size_t g_catalogJobsDone = 0;
    int g_catalogBuilding = -100;                // the Prices::Generation() it's being made for (-1 = estimates)

    void AddCatalogEntry(bool priced, std::uint16_t weapon, int paintKit, int tier, int wear, bool statTrak)
    {
        double price = 0.0;

        if (priced)
        {
            bool phaseGuessed = false;
            price = Prices::FindPhased(Cases::MarketName(weapon, paintKit, MidWear(wear), statTrak), paintKit, phaseGuessed);
            if (price <= 0.0)
                return;
        }
        else
        {
            if (statTrak || (wear != 2 && wear != -1))
                return;   // estimates: one per finish (Field-Tested)
            price = Estimate(tier, MidWear(wear), false);
        }

        Unboxer::Item item;
        item.kind = Unboxer::Kind::Drop;
        item.definition = weapon;
        item.paintKit = paintKit;
        item.wear = MidWear(wear);
        item.statTrak = statTrak;

        CatalogEntry entry;
        entry.weapon = weapon;
        entry.paintKit = paintKit;
        entry.tier = tier;
        entry.wear = wear;
        entry.statTrak = statTrak;
        entry.price = price;
        entry.title = TitleOf(item);
        entry.search = Lower(entry.title + (wear >= 0 ? std::string(" ") + WEARS[wear].shortName : std::string()));
        g_catalogNext.push_back(std::move(entry));
    }

    void BuildCatalog()
    {
        const bool priced = Prices::Ready();
        const int builtFor = priced ? Prices::Generation() : -1;

        if (builtFor == g_catalogBuiltFor || !Cases::Ready())
            return;

        // (again) from the start: what to look up - every gun finish, every knife / glove finish (vanilla knives too)
        if (g_catalogBuilding != builtFor)
        {
            g_catalogBuilding = builtFor;
            g_catalogJobs.clear();
            g_catalogNext.clear();
            g_catalogJobsDone = 0;

            for (const ItemDefinitionInfo* gun : Guns())
                if (const std::vector<PaintKitInfo>* finishes = Skins::g_ItemSchema.GetPaintKitsForItem(gun->definitionIndex))
                    for (const PaintKitInfo& finish : *finishes)
                        if (finish.id != 0)
                            g_catalogJobs.push_back({ gun->definitionIndex, &finish, SkinTier(gun->definitionIndex, finish), true });

            for (const ItemDefinitionInfo* gold : GoldTypes())
                if (const std::vector<PaintKitInfo>* finishes = Skins::g_ItemSchema.GetPaintKitsForItem(gold->definitionIndex))
                    for (const PaintKitInfo& finish : *finishes)
                        g_catalogJobs.push_back({ gold->definitionIndex, &finish, Cases::TierGold, !gold->isGlove });
        }

        const auto started = std::chrono::steady_clock::now();

        while (g_catalogJobsDone < g_catalogJobs.size())
        {
            const CatalogJob& job = g_catalogJobs[g_catalogJobsDone++];

            if (job.finish->id == 0)   // a vanilla knife: no wear
            {
                AddCatalogEntry(priced, job.weapon, 0, job.tier, -1, false);
                if (job.statTrakToo)
                    AddCatalogEntry(priced, job.weapon, 0, job.tier, -1, true);
            }
            else
            {
                for (int wear = 0; wear < static_cast<int>(std::size(WEARS)); ++wear)
                {
                    AddCatalogEntry(priced, job.weapon, job.finish->id, job.tier, wear, false);
                    if (job.statTrakToo)
                        AddCatalogEntry(priced, job.weapon, job.finish->id, job.tier, wear, true);
                }
            }

            if (std::chrono::steady_clock::now() - started > std::chrono::milliseconds(3))
                return;   // more next frame
        }

        g_catalog = std::move(g_catalogNext);
        g_catalogNext.clear();
        g_catalogJobs.clear();
        g_catalogJobsDone = 0;
        g_catalogBuiltFor = builtFor;
        g_upTarget = -1;          // indexes into the new one
        g_upTargetWearFor = -1;
    }

    // Still making the first catalog: how far (percent).
    bool CatalogLoading(int& percent)
    {
        if (g_catalogJobs.empty() || g_catalogBuilding == g_catalogBuiltFor)
            return false;

        percent = static_cast<int>(100 * g_catalogJobsDone / g_catalogJobs.size());
        return true;
    }

    // What the right panel lists: ~18 skins worth about the multiplier (or search results), worth more than yours.
    std::vector<int> Targets(double inputValue)
    {
        std::vector<int> list;

        if (inputValue <= 0.0)
            return list;

        const double floor = inputValue * 1.001;

        if (!g_upSearch.empty())
        {
            const std::string search = Lower(g_upSearch);

            for (int i = 0; i < static_cast<int>(g_catalog.size()); ++i)
                if (g_catalog[i].price > floor && g_catalog[i].search.find(search) != std::string::npos)
                    list.push_back(i);

            std::sort(list.begin(), list.end(), [](int a, int b) { return g_catalog[a].price < g_catalog[b].price; });
            if (list.size() > SEARCH_SHOWN)
                list.resize(SEARCH_SHOWN);
            return list;
        }

        const double wanted = inputValue * MULTIPLIERS[std::clamp(g_upMultiplier, 0, static_cast<int>(std::size(MULTIPLIERS)) - 1)];

        for (int i = 0; i < static_cast<int>(g_catalog.size()); ++i)
            if (g_catalog[i].price > floor)
                list.push_back(i);

        const auto distance = [&](int i) { return std::abs(std::log(g_catalog[i].price / wanted)); };

        if (list.size() > TARGETS_SHOWN)
        {
            std::nth_element(list.begin(), list.begin() + TARGETS_SHOWN, list.end(), [&](int a, int b) { return distance(a) < distance(b); });
            list.resize(TARGETS_SHOWN);
        }

        std::sort(list.begin(), list.end(), [](int a, int b) { return g_catalog[a].price < g_catalog[b].price; });
        return list;
    }

    float EaseOut(float t)
    {
        const float inverse = 1.0f - std::clamp(t, 0.0f, 1.0f);
        return 1.0f - inverse * inverse * inverse * inverse;   // quartic: long, slow finish
    }

    // Where the marker stops: inside the win zone (0 .. chance of the circle, from the top) or outside it.
    float LandingAngle(double chance, bool win)
    {
        const float zone = static_cast<float>(chance) * 2.0f * PI;
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        if (win)
        {
            const float margin = zone * 0.12f;
            return TOP + margin + unit(g_upRng) * std::max(0.0f, zone - margin * 2.0f);
        }

        const float rest = 2.0f * PI - zone;
        const float margin = std::min(rest * 0.12f, 0.35f);
        return TOP + zone + margin + unit(g_upRng) * std::max(0.0f, rest - margin * 2.0f);
    }

    // The wheel: a ring with the win zone lit from the top, a marker that runs around it, the chance in the middle.
    void DrawWheel(float size, double chance, double multiplier)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 center(std::floor(pos.x + width * 0.5f), std::floor(pos.y + size * 0.5f));
        const float thickness = S(14.0f);
        const float radius = size * 0.5f - thickness - S(8.0f);   // room for the marker outside the ring
        ImDrawList* draw = ImGui::GetWindowDrawList();

        const bool won = !g_spinning && g_result == 1;
        const bool lost = !g_spinning && g_result == -1;
        const ImU32 zone = won ? UI::Col::Success : lost ? IM_COL32(112, 88, 40, 255) : UI::Col::Accent;   // lost: dim gold

        // the track, the losing part a little red, the win zone with a soft glow (AddCircle: a closed arc from
        // PathArcTo repeats its first point and leaves a notch where it joins)
        draw->AddCircle(center, radius, UI::Col::Raised, 128, thickness);
        draw->AddCircle(center, radius, UI::WithAlpha(UI::Col::Danger, lost ? 0.45f : 0.16f), 128, thickness);

        if (chance > 0.0)
        {
            const float end = TOP + static_cast<float>(chance) * 2.0f * PI;
            const int segments = std::max(6, static_cast<int>(128.0 * chance));
            draw->PathArcTo(center, radius, TOP, end, segments);
            draw->PathStroke(UI::WithAlpha(zone, 0.16f), 0, thickness * 2.2f);
            draw->PathArcTo(center, radius, TOP, end, segments);
            draw->PathStroke(zone, 0, thickness);
        }

        draw->AddCircle(center, radius - thickness * 0.5f - S(7.0f), UI::WithAlpha(UI::Col::Border, 0.9f), 128, S(1.0f));

        // a fading trail behind the marker while it spins
        if (g_spinning)
        {
            for (int i = 0; i < 10; ++i)
            {
                const float a0 = g_marker - 0.06f * static_cast<float>(i + 1);
                draw->PathArcTo(center, radius, a0, a0 + 0.06f, 3);
                draw->PathStroke(UI::WithAlpha(UI::Col::Text, 0.35f * (1.0f - i / 10.0f)), 0, thickness * 0.45f);
            }
        }

        // the marker: a pointer outside the ring, its tip on the ring
        const ImVec2 dir(std::cos(g_marker), std::sin(g_marker));
        const ImVec2 normal(-dir.y, dir.x);
        const ImVec2 tip(center.x + dir.x * (radius - thickness * 0.15f), center.y + dir.y * (radius - thickness * 0.15f));
        const float baseDistance = radius + thickness * 0.5f + S(10.0f);
        const ImVec2 base(center.x + dir.x * baseDistance, center.y + dir.y * baseDistance);
        const ImVec2 baseA(base.x + normal.x * S(7.0f), base.y + normal.y * S(7.0f));
        const ImVec2 baseB(base.x - normal.x * S(7.0f), base.y - normal.y * S(7.0f));
        const ImU32 markerColor = won ? UI::Col::Success : lost ? UI::Col::Danger : UI::Col::Text;
        draw->AddTriangleFilled(baseA, tip, baseB, markerColor);
        draw->AddCircleFilled(tip, S(3.5f), markerColor, 16);

        // the middle, sized to stay inside the ring
        char big[32];
        const char* caption = nullptr;
        char second[32] = {};
        ImU32 bigColor = UI::Col::Text;

        if (won)
        {
            snprintf(big, sizeof(big), "%s", tr("WON"));
            bigColor = UI::Col::Success;
            caption = tr("upgraded");
        }
        else if (lost)
        {
            snprintf(big, sizeof(big), "%s", tr("LOST"));
            bigColor = UI::Col::Danger;
            caption = tr("try again");
        }
        else
        {
            snprintf(big, sizeof(big), chance < 0.1 ? "%.2f%%" : "%.1f%%", chance * 100.0);
            caption = tr("chance");
            if (multiplier > 0.0)
                snprintf(second, sizeof(second), "x%.2f", multiplier);
        }

        const float inner = radius - thickness * 0.5f - S(12.0f);
        float bigSize = std::min(ImGui::GetFontSize() * 2.2f, inner * 0.46f);
        const float bigWidth = UI::FontHeading->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, big).x;
        if (bigWidth > inner * 1.6f)   // long words (Russian "ПРОИГРЫШ") stay inside the ring
            bigSize *= inner * 1.6f / bigWidth;

        const float captionSize = ImGui::GetFontSize() * 0.8f;
        const ImVec2 bigText = UI::FontHeading->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, big);
        const ImVec2 captionText = UI::FontText->CalcTextSizeA(captionSize, FLT_MAX, 0.0f, caption);
        const ImVec2 secondText = second[0] ? UI::FontSemibold->CalcTextSizeA(captionSize, FLT_MAX, 0.0f, second) : ImVec2(0.0f, 0.0f);
        const float total = bigText.y + captionText.y + (second[0] ? secondText.y + S(2.0f) : 0.0f);
        float y = std::floor(center.y - total * 0.5f);

        draw->AddText(UI::FontHeading, bigSize, ImVec2(std::floor(center.x - bigText.x * 0.5f), y), bigColor, big);
        y += bigText.y;
        draw->AddText(UI::FontText, captionSize, ImVec2(std::floor(center.x - captionText.x * 0.5f), y), UI::Col::TextDim, caption);
        y += captionText.y + S(2.0f);
        if (second[0])
            draw->AddText(UI::FontSemibold, captionSize, ImVec2(std::floor(center.x - secondText.x * 0.5f), y), UI::Col::Accent, second);

        ImGui::Dummy(ImVec2(width, size));
    }

    // An empty picture spot with a line of text in it (nothing picked yet).
    void EmptyShowcase(float width, float height, const char* text)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, UI::Col::Raised, S(10.0f));
        draw->AddRect(min, max, UI::WithAlpha(UI::Col::Border, 0.8f), S(10.0f), 0, S(1.0f));

        const std::string line = UI::Fit(tr(text), UI::FontText, ImGui::GetFontSize(), width - S(24.0f));
        const ImVec2 size = UI::FontText->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, line.c_str());
        draw->AddText(UI::FontText, ImGui::GetFontSize(), ImVec2(std::floor(min.x + (width - size.x) * 0.5f), std::floor(min.y + (height - size.y) * 0.5f)),
            UI::Col::TextFaint, line.c_str());
        ImGui::Dummy(ImVec2(width, height));
    }

    void Caption(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::TextDim);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    void NameLine(const std::string& name, float width)
    {
        ImGui::PushFont(UI::FontSemibold);
        ImGui::TextUnformatted(UI::Fit(name.c_str(), UI::FontSemibold, ImGui::GetFontSize(), width).c_str());
        ImGui::PopFont();
    }

    bool Picked(std::uint64_t id)
    {
        return std::find(g_upInputs.begin(), g_upInputs.end(), id) != g_upInputs.end();
    }

    // Click: in / out of the upgrade (up to MAX_UPGRADE_INPUTS, their prices add up).
    void TogglePicked(std::uint64_t id)
    {
        if (g_spinning)
            return;

        if (const auto it = std::find(g_upInputs.begin(), g_upInputs.end(), id); it != g_upInputs.end())
            g_upInputs.erase(it);
        else if (g_upInputs.size() < MAX_UPGRADE_INPUTS)
            g_upInputs.push_back(id);
        else
            Notify::Push(tr("Up to 6 items at once."), UI::Col::Warning);

        g_result = 0;
    }

    // The stats on the stage's title line, right-aligned: "14 upgrades  6 won  8 lost  43%  best x7.80  reset".
    void StageStats(float titleY, float right)
    {
        const Unboxer::UpgradeStats stats = Unboxer::GetUpgradeStats();

        if (stats.tries <= 0)
            return;

        struct Part
        {
            std::string text;
            ImU32 color;
        };

        char text[64];
        std::vector<Part> parts;
        snprintf(text, sizeof(text), "%d %s", stats.tries, tr("upgrades"));
        parts.push_back({ text, UI::Col::TextDim });
        snprintf(text, sizeof(text), "%d %s", stats.wins, tr("won"));
        parts.push_back({ text, UI::Col::Success });
        snprintf(text, sizeof(text), "%d %s", stats.losses, tr("lost"));
        parts.push_back({ text, UI::Col::Danger });
        snprintf(text, sizeof(text), "%.0f%%", 100.0 * stats.wins / stats.tries);
        parts.push_back({ text, UI::Col::Accent });

        if (stats.bestMultiplier > 0.0)
        {
            snprintf(text, sizeof(text), "%s x%.2f", tr("best"), stats.bestMultiplier);
            parts.push_back({ text, UI::Col::Accent });
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImFont* font = UI::FontSemibold;
        const float size = ImGui::GetFontSize() * 0.78f;
        const float space = S(14.0f);
        const char* reset = tr("reset");
        const ImVec2 resetSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, reset);

        float width = resetSize.x;
        for (const Part& part : parts)
            width += font->CalcTextSizeA(size, FLT_MAX, 0.0f, part.text.c_str()).x + space;

        float x = std::floor(right - width);
        for (const Part& part : parts)
        {
            draw->AddText(font, size, ImVec2(x, titleY), part.color, part.text.c_str());
            x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, part.text.c_str()).x + space;
        }

        // "reset": a small text button
        const ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(x, titleY));
        const bool clicked = ImGui::InvisibleButton("##up_reset", ImVec2(std::max(1.0f, resetSize.x), std::max(1.0f, resetSize.y)));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(back);

        draw->AddText(font, size, ImVec2(x, titleY), hovered ? UI::Col::Text : UI::Col::TextFaint, reset);

        if (hovered)
            ImGui::SetTooltip("%s", stats.bestName.empty() ? tr("Start the stats again") : (std::string(tr("Best win: ")) + stats.bestName).c_str());

        if (clicked && !g_spinning)
            Unboxer::ResetUpgradeStats();
    }

    // The picture's height in a stage column: as big as the column lets it, the text under it still fitting.
    float StagePicture(float width, float height, float below)
    {
        const float caption = ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y;
        return std::max(S(70.0f), std::min(width * 0.62f, height - caption - below));
    }

    // Left: what goes in - the most valuable one big, all of them small under it (click one: out).
    void StageInputs(const std::vector<const Unboxer::Item*>& inputs, double value, bool estimated, float width, float height)
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float line = ImGui::GetFontSize() + spacing;
        const float thumb = S(38.0f);
        const float picture = StagePicture(width, height, line * 2.0f + (inputs.size() > 1 ? thumb + spacing : 0.0f) + S(4.0f));

        char caption[64];
        if (inputs.size() > 1)
            snprintf(caption, sizeof(caption), tr("Your items (%d)"), static_cast<int>(inputs.size()));
        else
            snprintf(caption, sizeof(caption), "%s", tr("Your item"));
        Caption(caption);

        if (inputs.empty())
        {
            EmptyShowcase(width, picture, "Pick your item below");
            Caption(tr("Click items to put them in (up to 6) - their prices add up."));
            return;
        }

        const Unboxer::Item* main = inputs.front();
        double best = -1.0;
        for (const Unboxer::Item* item : inputs)
        {
            bool unused = false;
            if (const double price = ItemPrice(*item, unused); price > best)
            {
                best = price;
                main = item;
            }
        }

        Showcase(ItemImage(*main), width, picture, ItemColor(*main));

        char more[32] = {};
        if (inputs.size() > 1)
            snprintf(more, sizeof(more), "  +%d", static_cast<int>(inputs.size()) - 1);
        NameLine(Unboxer::DisplayName(*main) + more, width);

        std::string worth = (estimated ? "~" : "") + Money(value);
        if (inputs.size() > 1)
            worth += std::string("  ") + tr("together");
        Caption(worth.c_str());

        if (inputs.size() < 2)
            return;

        // every one of them small: click = out
        ImDrawList* draw = ImGui::GetWindowDrawList();
        std::uint64_t remove = 0;

        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const Unboxer::Item& item = *inputs[i];

            if (i)
                ImGui::SameLine(0.0f, S(6.0f));

            ImGui::PushID(static_cast<int>(i));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##up_thumb", ImVec2(thumb, thumb));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const ImVec2 max(min.x + thumb, min.y + thumb);
            draw->AddRectFilled(min, max, hovered ? UI::Col::Hover : UI::Col::Raised, S(7.0f));
            UI::DrawImage(draw, ItemImage(item), ImVec2(min.x + S(3.0f), min.y + S(3.0f)), ImVec2(max.x - S(3.0f), max.y - S(5.0f)));
            draw->AddRectFilled(ImVec2(min.x + S(6.0f), max.y - S(3.0f)), ImVec2(max.x - S(6.0f), max.y - S(1.5f)), ItemColor(item), S(1.0f));

            if (hovered)
            {
                draw->AddRect(min, max, UI::WithAlpha(UI::Col::Danger, 0.8f), S(7.0f), 0, S(1.5f));
                bool unused = false;
                ImGui::SetTooltip("%s  %s\n%s", Unboxer::DisplayName(item).c_str(), Money(ItemPrice(item, unused)).c_str(), tr("Click: take it out"));
            }

            if (clicked)
                remove = item.id;
        }

        if (remove)
            TogglePicked(remove);
    }

    // Right: what it becomes, and the float it comes with (typed, inside its wear).
    void StageTarget(const CatalogEntry* target, float width, float height)
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float line = ImGui::GetFontSize() + spacing;
        const bool hasFloat = target && target->wear >= 0;
        const float picture = StagePicture(width, height, line * 2.0f + (hasFloat ? S(34.0f) + spacing : 0.0f) + S(4.0f));

        Caption(tr("Upgrade to"));

        if (!target)
        {
            EmptyShowcase(width, picture, "Pick what it becomes below");
            Caption(tr("Worth more than yours: the higher the multiplier, the lower the chance."));
            return;
        }

        Showcase(DropImage(target->weapon, target->paintKit), width, picture, TierColor(target->tier));
        NameLine(target->title, width);

        const std::string worth = (target->wear >= 0 ? std::string(tr(Unboxer::WearName(MidWear(target->wear)))) + "  " : std::string()) + Money(target->price);
        Caption(worth.c_str());

        if (hasFloat)
        {
            const WearBracket& bracket = WEARS[target->wear];
            const float top = bracket.to >= 1.0f ? 1.0f : bracket.to - 0.000001f;

            ImGui::BeginDisabled(g_spinning);
            UI::FloatField("##up_float", "Float", &g_upTargetWear, bracket.from, top, 0.001f, "%.6f");
            ImGui::EndDisabled();
        }
    }

    // Middle: the wheel, the numbers, the button.
    void StageCenter(const std::vector<const Unboxer::Item*>& inputs, const CatalogEntry* target, double inputValue, float height)
    {
        const double targetValue = target ? target->price : 0.0;
        const bool valid = !inputs.empty() && target && targetValue > inputValue;
        const double chance = valid ? Chance(inputValue, targetValue) : 0.0;
        const double multiplier = valid ? targetValue / inputValue : 0.0;

        const float button = S(46.0f);
        const float line = ImGui::GetFontSize() * 1.25f;
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float ring = std::max(S(120.0f), std::min({ ImGui::GetContentRegionAvail().x, S(300.0f), height - button - line - spacing * 2.0f - S(4.0f) }));

        DrawWheel(ring, chance, multiplier);

        // under it: the multiplier and what you'd make
        {
            char left[48], right[64];
            if (valid)
                snprintf(left, sizeof(left), "%s x%.2f", tr("Multiplier"), multiplier);
            else
                snprintf(left, sizeof(left), "%s -", tr("Multiplier"));
            snprintf(right, sizeof(right), "%s %s", tr("Profit"), valid ? ("+" + Money(targetValue - inputValue)).c_str() : "-");

            ImFont* font = UI::FontSemibold;
            const float size = ImGui::GetFontSize() * 0.9f;
            const float gap = S(18.0f);
            const float a = font->CalcTextSizeA(size, FLT_MAX, 0.0f, left).x;
            const float b = font->CalcTextSizeA(size, FLT_MAX, 0.0f, right).x;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float x = std::floor(pos.x + (ImGui::GetContentRegionAvail().x - a - gap - b) * 0.5f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddText(font, size, ImVec2(x, pos.y), valid ? UI::Col::Accent : UI::Col::TextFaint, left);
            draw->AddText(font, size, ImVec2(x + a + gap, pos.y), valid ? UI::Col::Success : UI::Col::TextFaint, right);
            ImGui::Dummy(ImVec2(1.0f, line));
        }

        const bool canSpin = valid && !g_spinning && Unboxer::GetStatus().inventoryReady;

        if (UI::Button(g_spinning ? "Spinning...##up_go" : "UPGRADE##up_go", ImVec2(-1.0f, button), UI::Kind::Primary, canSpin))
        {
            const bool win = std::uniform_real_distribution<double>(0.0, 1.0)(g_upRng) < chance;
            const bool gold = target->tier == Cases::TierGold || IsGold(target->weapon);

            Unboxer::Item created;
            created.kind = Unboxer::Kind::Drop;
            created.definition = target->weapon;
            created.paintKit = target->paintKit;
            created.statTrak = target->statTrak;
            created.wear = target->wear >= 0 ? std::clamp(g_upTargetWear, WEARS[target->wear].from, std::min(WEARS[target->wear].to, 1.0f)) : 0.0f;
            created.seed = std::uniform_int_distribution<int>(0, 1000)(g_upRng);
            created.rarity = Cases::Rarity(static_cast<Cases::Tier>(target->tier));
            created.quality = gold ? 3 : (target->statTrak ? 9 : 4);

            std::vector<std::uint64_t> ids;
            for (const Unboxer::Item* item : inputs)
                ids.push_back(item->id);

            if (Unboxer::Upgrade(ids, created, win, multiplier, static_cast<unsigned>(SPIN_MS)))
            {
                float current = std::fmod(g_marker - TOP, 2.0f * PI);
                if (current < 0.0f)
                    current += 2.0f * PI;

                const float landing = LandingAngle(chance, win) - TOP;
                float delta = landing - current;
                while (delta < 0.0f)
                    delta += 2.0f * PI;

                g_spinFrom = TOP + current;
                g_spinTo = g_spinFrom + delta + 5.0f * 2.0f * PI;   // five full turns first
                g_spinStart = NowMs();
                g_spinWin = win;
                g_spinTarget = Unboxer::DisplayName(created);
                g_spinning = true;
                g_result = 0;
            }
            else
            {
                Notify::Push(tr("That item can't be upgraded right now."), UI::Col::Warning);
            }
        }
    }

    // The stage (top, full width): your item(s) -> the wheel -> what it becomes.
    void RenderUpgradeStage(const std::vector<const Unboxer::Item*>& inputs, double inputValue, bool estimated, float height)
    {
        // ── the spin ──
        if (g_spinning)
        {
            const float t = static_cast<float>(NowMs() - g_spinStart) / static_cast<float>(SPIN_MS);
            g_marker = g_spinFrom + (g_spinTo - g_spinFrom) * EaseOut(t);

            if (t >= 1.0f)
            {
                g_spinning = false;
                g_marker = std::fmod(g_spinTo - TOP, 2.0f * PI) + TOP;
                g_result = g_spinWin ? 1 : -1;
                g_upInputs.clear();   // gone either way

                if (g_spinWin)   // the new item becomes the next input once it's in the inventory
                {
                    for (const Unboxer::Item& item : Unboxer::Items())
                        g_upSelectNewerThan = std::max(g_upSelectNewerThan, item.id);
                    g_upTarget = -1;
                }

                Notify::Push(g_spinWin ? std::string(tr("Upgraded: ")) + g_spinTarget : std::string(tr("Lost the upgrade to ")) + g_spinTarget,
                    g_spinWin ? UI::Col::Success : UI::Col::Danger);
            }
        }

        const CatalogEntry* target = g_upTarget >= 0 && g_upTarget < static_cast<int>(g_catalog.size()) ? &g_catalog[static_cast<std::size_t>(g_upTarget)] : nullptr;

        // a new target: its float, somewhere in its wear (typed over in the stage)
        if (target && g_upTargetWearFor != g_upTarget)
        {
            g_upTargetWearFor = g_upTarget;
            g_upTargetWear = target->wear >= 0 ?
                std::uniform_real_distribution<float>(WEARS[target->wear].from, std::min(WEARS[target->wear].to, 0.8f) - 0.0005f)(g_upRng) : 0.0f;
        }

        if (UI::BeginPanel("##up_stage", ImVec2(ImGui::GetContentRegionAvail().x, height), "Upgrade"))
        {
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float titleY = cursor.y - ImGui::GetFontSize() * 0.78f - S(8.0f) + S(1.0f);
            StageStats(titleY, cursor.x + ImGui::GetContentRegionAvail().x);

            const ImVec2 inner = ImGui::GetContentRegionAvail();
            const float gap = S(18.0f);
            const float side = std::floor((inner.x - gap * 2.0f) * 0.33f);
            const float middle = std::max(1.0f, inner.x - side * 2.0f - gap * 2.0f);
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

            if (ImGui::BeginChild("##up_stage_in", ImVec2(side, inner.y), ImGuiChildFlags_None, flags))
                StageInputs(inputs, inputValue, estimated, side, inner.y);
            ImGui::EndChild();

            ImGui::SameLine(0.0f, gap);
            if (ImGui::BeginChild("##up_stage_mid", ImVec2(middle, inner.y), ImGuiChildFlags_None, flags))
                StageCenter(inputs, target, inputValue, inner.y);
            ImGui::EndChild();

            ImGui::SameLine(0.0f, gap);
            if (ImGui::BeginChild("##up_stage_out", ImVec2(side, inner.y), ImGuiChildFlags_None, flags))
                StageTarget(target, side, inner.y);
            ImGui::EndChild();
        }
        UI::EndPanel();
    }

    void RenderUpgradeInputs(const std::vector<Unboxer::Item>& drops, float height)
    {
        if (UI::BeginPanel("##up_items", ImVec2(ImGui::GetContentRegionAvail().x, height), "Your items"))
        {
            // the title line, right: how many are in, and "clear"
            if (!g_upInputs.empty())
            {
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const float size = ImGui::GetFontSize() * 0.78f;
                const float titleY = cursor.y - size - S(8.0f) + S(1.0f);
                const float right = cursor.x + ImGui::GetContentRegionAvail().x;
                const char* clear = tr("clear");

                char count[48];
                snprintf(count, sizeof(count), tr("%d of 6 in"), static_cast<int>(g_upInputs.size()));

                ImFont* font = UI::FontSemibold;
                const ImVec2 clearSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, clear);
                const float countWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, count).x;
                const float clearX = std::floor(right - clearSize.x);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddText(font, size, ImVec2(clearX - S(14.0f) - countWidth, titleY), UI::Col::Accent, count);

                ImGui::SetCursorScreenPos(ImVec2(clearX, titleY));
                const bool clicked = ImGui::InvisibleButton("##up_clear", ImVec2(std::max(1.0f, clearSize.x), std::max(1.0f, clearSize.y)));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::SetCursorScreenPos(cursor);
                draw->AddText(font, size, ImVec2(clearX, titleY), hovered ? UI::Col::Text : UI::Col::TextFaint, clear);

                if (clicked && !g_spinning)
                {
                    g_upInputs.clear();
                    g_result = 0;
                }
            }

            UI::Search("##up_items_search", "Search your items...", &g_upItemsSearch);

            if (ImGui::BeginChild("##up_items_grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
            {
                UI::Grid grid = UI::MakeGrid(S(112.0f));

                for (auto it = drops.rbegin(); it != drops.rend(); ++it)   // newest first
                {
                    const Unboxer::Item& item = *it;
                    const std::string name = Unboxer::DisplayName(item);

                    if (!Matches(Lower(name), g_upItemsSearch))
                        continue;

                    UI::NextCell(grid);
                    char id[48];
                    snprintf(id, sizeof(id), "up_in_%llu", static_cast<unsigned long long>(item.id));

                    bool estimated = false;
                    const double price = ItemPrice(item, estimated);

                    if (UI::Card(id, grid.width, name, (estimated ? "~" : "") + Money(price), ItemColor(item), Picked(item.id), [&] { return ItemImage(item); }))
                        TogglePicked(item.id);
                }

                if (drops.empty())
                    UI::Note("No skins yet - open a case first. Cases and keys can't be upgraded.");
                else if (grid.index == 0)
                    UI::Note("Nothing found.");
            }
            ImGui::EndChild();
        }
        UI::EndPanel();
    }

    void RenderUpgradeTargets(double inputValue, bool haveInput, float height)
    {
        if (UI::BeginPanel("##up_targets", ImVec2(ImGui::GetContentRegionAvail().x, height), "Upgrade to"))
        {
            // the title line, right: where the prices come from
            {
                const Prices::Info prices = Prices::GetInfo();
                char pricesLine[160];

                if (prices.state == Prices::State::Ready)
                    snprintf(pricesLine, sizeof(pricesLine), tr(prices.fromFile ? "Prices: %d items, your prices_csfloat.json from %s" :
                        "Prices: %d items, built-in list from %s"), prices.count, prices.date.c_str());
                else
                    snprintf(pricesLine, sizeof(pricesLine), "%s", tr(prices.state == Prices::State::Missing ? "Prices: no price list - estimates are used" :
                        "Prices: not loaded yet"));

                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const float size = ImGui::GetFontSize() * 0.78f;
                const float titleY = cursor.y - size - S(8.0f) + S(1.0f);
                const float room = ImGui::GetContentRegionAvail().x * 0.62f;
                const std::string shown = UI::Fit(pricesLine, UI::FontText, size, room);
                const ImVec2 textSize = UI::FontText->CalcTextSizeA(size, FLT_MAX, 0.0f, shown.c_str());
                const ImVec2 at(std::floor(cursor.x + ImGui::GetContentRegionAvail().x - textSize.x), titleY);
                ImGui::GetWindowDrawList()->AddText(UI::FontText, size, at, UI::Col::TextFaint, shown.c_str());

                if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + textSize.x, at.y + textSize.y)))
                    ImGui::SetTooltip("%s\n%s", pricesLine, tr("Lowest CSFloat listings, market.csgo.com where CSFloat has none. The list is built into Lootline: it never goes online."));
            }

            UI::Segmented("##up_multiplier", MULTIPLIER_LABELS, &g_upMultiplier);
            UI::Search("##up_search", "Search any skin, knife, gloves...", &g_upSearch);

            if (ImGui::BeginChild("##up_targets_grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
            {
                const std::vector<int> targets = Targets(inputValue);
                UI::Grid grid = UI::MakeGrid(S(112.0f));

                for (const int index : targets)
                {
                    const CatalogEntry& entry = g_catalog[static_cast<std::size_t>(index)];
                    UI::NextCell(grid);

                    char id[64];
                    snprintf(id, sizeof(id), "up_to_%u_%d_%d_%d", static_cast<unsigned>(entry.weapon), entry.paintKit, entry.wear, entry.statTrak ? 1 : 0);

                    char subtitle[64];
                    snprintf(subtitle, sizeof(subtitle), "%s%s%s  %.1f%%", entry.wear >= 0 ? WEARS[entry.wear].shortName : "", entry.wear >= 0 ? "  " : "",
                        Money(entry.price).c_str(), Chance(inputValue, entry.price) * 100.0);

                    if (UI::Card(id, grid.width, entry.title, subtitle, TierColor(entry.tier), g_upTarget == index,
                        [&] { return DropImage(entry.weapon, entry.paintKit); }) && !g_spinning)
                    {
                        g_upTarget = index;
                        g_result = 0;
                    }
                }

                int percent = 0;

                if (g_catalog.empty() && CatalogLoading(percent))
                {
                    char loading[96];
                    snprintf(loading, sizeof(loading), tr("Reading the prices... %d%%"), percent);
                    UI::Note(loading);
                }
                else if (!haveInput)
                    UI::Note("Pick your item on the left first - this list shows what it can become.");
                else if (targets.empty())
                    UI::Note(g_upSearch.empty() ? "Nothing is worth more than this item." : "Nothing found that's worth more than your item.");
            }
            ImGui::EndChild();
        }
        UI::EndPanel();
    }
}

void RenderUpgraderPage()
{
    BuildCatalog();

    const std::vector<Unboxer::Item> items = Unboxer::Items();
    std::vector<Unboxer::Item> drops;

    for (const Unboxer::Item& item : items)
        if (item.kind == Unboxer::Kind::Drop)
            drops.push_back(item);

    // after a win: the new item is the next input
    if (g_upSelectNewerThan != 0 && !g_spinning)
    {
        for (const Unboxer::Item& item : drops)
        {
            if (item.id > g_upSelectNewerThan)
            {
                g_upInputs = { item.id };
                g_upSelectNewerThan = 0;
                break;
            }
        }
    }

    // what's in (an item removed meanwhile drops out)
    std::vector<const Unboxer::Item*> inputs;
    for (const std::uint64_t id : g_upInputs)
        for (const Unboxer::Item& item : drops)
            if (item.id == id)
                inputs.push_back(&item);

    if (!g_spinning && inputs.size() != g_upInputs.size())
    {
        g_upInputs.clear();
        for (const Unboxer::Item* item : inputs)
            g_upInputs.push_back(item->id);
    }

    double inputValue = 0.0;
    bool estimated = false;
    for (const Unboxer::Item* item : inputs)
    {
        bool guessed = false;
        inputValue += ItemPrice(*item, guessed);
        estimated = estimated || guessed;
    }

    // more went in and the target isn't worth more anymore: pick another
    if (!g_spinning && g_upTarget >= 0 && g_upTarget < static_cast<int>(g_catalog.size()) && !inputs.empty() &&
        g_catalog[static_cast<std::size_t>(g_upTarget)].price <= inputValue)
        g_upTarget = -1;

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float stage = std::clamp(std::floor(available.y * 0.5f), std::min(S(330.0f), available.y), S(470.0f));
    RenderUpgradeStage(inputs, inputValue, estimated, stage);

    const float lists = std::max(S(160.0f), ImGui::GetContentRegionAvail().y);
    const float half = std::floor((available.x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

    if (ImGui::BeginChild("##up_left", ImVec2(half, lists), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar))
        RenderUpgradeInputs(drops, lists);
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##up_right", ImVec2(0.0f, lists), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar))
        RenderUpgradeTargets(inputValue, !inputs.empty(), lists);
    ImGui::EndChild();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// Add items page: any item the game has straight into your inventory (the inventory changer)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
namespace
{
    enum AddCategory : int
    {
        AddGuns = 0, AddKnives, AddGloves, AddAgents, AddStickers, AddPatches, AddGraffiti, AddCharms, AddMusic, AddCollectibles, AddPets, AddTools,
        AddCount
    };

    constexpr const char* ADD_CATEGORY_NAMES[AddCount] = {
        "Guns", "Knives", "Gloves", "Agents", "Stickers", "Patches", "Graffiti", "Charms", "Music kits", "Pins & coins", "Pets", "Tools",
    };

    // what's picked
    struct AddChoice
    {
        std::uint16_t definition = 0;
        int kit = 0;                  // finish / sticker / patch / graffiti / music kit / charm id (0 = none)
        int tier = Cases::TierMilSpec;
        int highlight = 0;            // a Major highlight charm: the clip (Cases::Highlight number), 0 = none
    };

    int g_addCategory = AddGuns;
    AddChoice g_addChoice;
    std::string g_addSearch;
    std::string g_addClipSearch;      // the highlight clip list's search (RenderClipPicker)
    std::string g_addTypeSearch;      // the type pictures' search (guns / knives / gloves)
    std::string g_addGlobal;          // the search at the top: every category at once (empty = browse the categories)
    int g_addGlobalCategory = -1;     // its chips: -1 = all, else one AddCategory
    std::uint16_t g_addGun = 0;
    std::uint16_t g_addKnife = 0;
    std::uint16_t g_addGlove = 0;

    // options
    float g_addWear = 0.01f;
    bool g_addRandomFloat = false;    // "Random float": every one added gets its own float within the picked wear
    int g_addRandomWear = 0;          // that wear (WEAR_PRESETS: FN ... BS)
    int g_addSeed = 0;
    bool g_addRandomSeed = false;     // "Random seed": every one added gets its own pattern (0 - 1000)
    bool g_addStatTrak = false;
    bool g_addRealKills = true;
    int g_addKills = 0;
    bool g_addSouvenir = false;
    char g_addName[24] = {};
    int g_addCharm = 0;
    int g_addCharmSeed = 0;
    std::string g_addCharmSearch;
    int g_addBreed = 0;               // 0 Catalana, 1 Silkie, 2 Polish
    int g_addStage = 2;               // 0 chick, 1 pullet, 2 hen
    int g_addPetSeed = 0;
    int g_addItemCount = 1;

    // the filtered list of a big category (stickers...), made again when the category or the search changes
    struct FilterCache
    {
        int category = -1;
        std::string search;
        int sort = -1;
        bool ready = false;
        std::vector<int> indices;
    };
    FilterCache g_addFilter;

    std::vector<const ItemDefinitionInfo*> TypesOf(bool knives)
    {
        std::vector<const ItemDefinitionInfo*> out;
        for (const ItemDefinitionInfo* type : GoldTypes())
            if (type->isKnife == knives)
                out.push_back(type);
        return out;
    }

    const std::vector<const ItemDefinitionInfo*>& Knives()
    {
        static std::vector<const ItemDefinitionInfo*> list;   // render thread
        if (list.empty())
            list = TypesOf(true);
        return list;
    }

    const std::vector<const ItemDefinitionInfo*>& GloveTypes()
    {
        static std::vector<const ItemDefinitionInfo*> list;   // render thread
        if (list.empty())
            list = TypesOf(false);
        return list;
    }

    // Every agent with a model, T first, by name - not the map's default ones ("Default T Agent" / "Default CT Agent",
    // the 78 items_game names #CSGO_CustomPlayer_t_map_based / _ct_map_based: what you have without an agent anyway).
    const std::vector<const ItemDefinitionInfo*>& Agents()
    {
        static std::vector<const ItemDefinitionInfo*> list;   // render thread

        if (list.empty())
        {
            for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
                if (item.isAgent && item.hasUsableAgentModel && (item.isTerroristAgent || item.isCounterTerroristAgent) &&
                    item.baseName.find("_map_based") == std::string::npos)
                    list.push_back(&item);

            std::sort(list.begin(), list.end(), [](const ItemDefinitionInfo* a, const ItemDefinitionInfo* b)
            {
                if (a->isTerroristAgent != b->isTerroristAgent)
                    return a->isTerroristAgent;
                return a->displayName < b->displayName;
            });
        }

        return list;
    }

    int AgentTier(std::uint16_t definition)
    {
        const int tier = Cases::ItemTier(definition);
        return tier >= 0 ? tier : static_cast<int>(Cases::TierMilSpec);
    }

    int CharmTier(const KeychainInfo& charm)
    {
        return std::clamp(charm.rarity - 1, static_cast<int>(Cases::TierConsumer), static_cast<int>(Cases::TierCovert));
    }

    // Add items' charms: every charm but a clip's own (Cologne 2026 on). A Major's highlights are ONE card - its event
    // charm - and the clip is picked on the right (RenderClipPicker). Indices into GetKeychains().
    std::vector<int> g_charmList;   // render thread
    std::size_t g_charmListFrom = static_cast<std::size_t>(-1);

    const std::vector<int>& CharmList()
    {
        const auto& charms = Skins::g_ItemSchema.GetKeychains();

        if (g_charmListFrom == charms.size())
            return g_charmList;

        g_charmList.clear();

        for (int i = 0; i < static_cast<int>(charms.size()); ++i)
            if (charms[i].base.empty())
                g_charmList.push_back(i);

        g_charmListFrom = charms.size();
        g_addFilter.ready = false;   // the shown list was made from the old one
        return g_charmList;
    }

    // Every word of `search` (lowercase) is somewhere in `text` (lowercase): "flamez ace".
    bool HasAllWords(const std::string& text, const std::string& search)
    {
        std::size_t start = 0;

        while (start < search.size())
        {
            const std::size_t end = std::min(search.find(' ', start), search.size());

            if (end > start && text.find(search.substr(start, end - start)) == std::string::npos)
                return false;

            start = end + 1;
        }

        return true;
    }

    // An event charm's clips as search text (the charm card / the global search find it by a clip, player, team...).
    std::unordered_map<int, std::string> g_eventSearch;   // render thread

    const std::string& EventSearchText(int event)
    {
        auto it = g_eventSearch.find(event);

        if (it == g_eventSearch.end())
        {
            std::string text;

            for (const Cases::Highlight* clip : Cases::HighlightsOf(event))
                text += " " + clip->search;

            it = g_eventSearch.emplace(event, std::move(text)).first;
        }

        return it->second;
    }

    // Picks a Major's highlight charm: the first clip matching `search` (lowercase words, e.g. from the global search),
    // else the clip already picked if it's this event's, else the first. True = a clip matched the search (the clip
    // list keeps it; "austin" found the event's card, not a clip).
    bool PickHighlightEvent(int event, int tier, const std::string& search)
    {
        const std::vector<const Cases::Highlight*>& clips = Cases::HighlightsOf(event);

        if (clips.empty())
        {
            g_addChoice = { Unboxer::DEF_CHARM, event, tier };
            return false;
        }

        const Cases::Highlight* pick = nullptr;

        if (!search.empty())
            for (const Cases::Highlight* clip : clips)
                if (HasAllWords(clip->search, search))
                {
                    pick = clip;
                    break;
                }

        const bool matched = pick != nullptr;

        if (!pick && g_addChoice.definition == Unboxer::DEF_CHARM)
            if (const Cases::Highlight* current = Cases::FindHighlight(g_addChoice.highlight); current && current->event == event)
                pick = current;

        if (!pick)
            pick = clips.front();

        g_addChoice = { Unboxer::DEF_CHARM, pick->keychain, tier, pick->number };
        return matched;
    }

    bool IsSkinCategory(int category)
    {
        return category == AddGuns || category == AddKnives || category == AddGloves;
    }

    void ResetChoice()
    {
        g_addChoice = {};
        g_addSearch.clear();
        g_addTypeSearch.clear();
        g_addFilter.ready = false;
    }

    // A grid of `count` cards where only the rows on screen are drawn (thousands of stickers stay fast).
    template <typename DrawCard>
    void ClippedGrid(const char* id, int count, DrawCard&& draw)
    {
        if (ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        {
            const UI::Grid grid = UI::MakeGridFitting(count, S(108.0f), S(240.0f));
            const int rows = (count + grid.columns - 1) / grid.columns;

            ImGuiListClipper clipper;
            clipper.Begin(rows);

            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    for (int column = 0; column < grid.columns; ++column)
                    {
                        const int index = row * grid.columns + column;
                        if (index >= count)
                            break;

                        if (column)
                            ImGui::SameLine();

                        draw(index, grid.width);
                    }
                }
            }

            clipper.End();

            if (count == 0)
                UI::Note("Nothing found.");
        }
        ImGui::EndChild();
    }

    // The search filter over a list by name (lowercase match), made again only when something changed.
    //   tierOf(i): Cases::Tier for the "Rarity" order (best first, then A-Z); "A-Z" sorts by nameOf
    //   popularOf(i): PopularPrice for the "Popular" order (the most wanted first, then A-Z)
    template <typename NameOf, typename TierOf, typename PopularOf>
    const std::vector<int>& Filtered(int category, int size, NameOf&& nameOf, TierOf&& tierOf, PopularOf&& popularOf)
    {
        const int sort = std::clamp(Config::menu.skinSort, 0, 2);

        if (!g_addFilter.ready || g_addFilter.category != category || g_addFilter.search != g_addSearch || g_addFilter.sort != sort)
        {
            g_addFilter.category = category;
            g_addFilter.search = g_addSearch;
            g_addFilter.sort = sort;
            g_addFilter.indices.clear();
            const std::string search = Lower(g_addSearch);
            std::vector<std::string> names(static_cast<std::size_t>(size));

            for (int i = 0; i < size; ++i)
            {
                names[i] = Lower(nameOf(i));

                if (search.empty() || names[i].find(search) != std::string::npos)
                    g_addFilter.indices.push_back(i);
            }

            std::vector<double> popular;

            if (sort == 2)
            {
                popular.assign(static_cast<std::size_t>(size), 0.0);

                for (const int i : g_addFilter.indices)
                    popular[static_cast<std::size_t>(i)] = popularOf(i);
            }

            std::stable_sort(g_addFilter.indices.begin(), g_addFilter.indices.end(), [&](int a, int b)
            {
                if (sort == 1)
                    if (const int ta = tierOf(a), tb = tierOf(b); ta != tb)
                        return ta > tb;

                if (sort == 2)
                    if (const double pa = popular[static_cast<std::size_t>(a)], pb = popular[static_cast<std::size_t>(b)]; pa != pb)
                        return pa > pb;

                return names[a] < names[b];
            });

            g_addFilter.ready = true;
        }

        return g_addFilter.indices;
    }

    // The categories in as few rows as fit, the same number of tabs in each row, each row filling the width.
    void RenderAddCategoryTabs()
    {
        const float available = ImGui::GetContentRegionAvail().x;
        const float spacing = S(6.0f);
        float natural[AddCount]{};

        for (int i = 0; i < AddCount; ++i)
            natural[i] = ImGui::CalcTextSize(tr(ADD_CATEGORY_NAMES[i])).x + S(22.0f);

        // the fewest rows where every row (perRow tabs) still fits
        int perRow = AddCount;
        for (int rows = 1; rows <= AddCount; ++rows)
        {
            perRow = (AddCount + rows - 1) / rows;
            bool fits = true;

            for (int start = 0; start < AddCount && fits; start += perRow)
            {
                float width = 0.0f;
                for (int i = start; i < std::min(static_cast<int>(AddCount), start + perRow); ++i)
                    width += natural[i] + (i > start ? spacing : 0.0f);
                fits = width <= available;
            }

            if (fits)
                break;
        }

        for (int start = 0; start < AddCount; start += perRow)
        {
            const int end = std::min(static_cast<int>(AddCount), start + perRow);
            float used = spacing * static_cast<float>(end - start - 1);
            for (int i = start; i < end; ++i)
                used += natural[i];

            const float extra = std::max(0.0f, (available - used) / static_cast<float>(end - start));

            for (int i = start; i < end; ++i)
            {
                if (i > start)
                    ImGui::SameLine(0.0f, spacing);

                char label[64];
                snprintf(label, sizeof(label), "%s##add_cat_%d", ADD_CATEGORY_NAMES[i], i);

                const bool active = g_addCategory == i && g_addGlobal.empty();

                if (UI::Button(label, ImVec2(std::floor(natural[i] + extra), S(30.0f)), active ? UI::Kind::Primary : UI::Kind::Secondary))
                {
                    g_addGlobal.clear();   // searching: back to browsing

                    if (g_addCategory != i)
                    {
                        g_addCategory = i;
                        ResetChoice();
                    }
                }
            }
        }
    }

    // Guns by group, like Chadware's gun browser (from the item's type: "#CSGO_Type_Pistol"...).
    constexpr const char* WEAPON_GROUPS[] = { "Pistols", "Rifles", "Sniper Rifles", "SMGs", "Shotguns", "Machine Guns", "Equipment" };

    int WeaponGroupOf(const ItemDefinitionInfo& item)
    {
        const std::string& type = item.typeName;

        if (type.find("Pistol") != std::string::npos)
            return 0;
        if (type.find("Sniper") != std::string::npos)
            return 2;
        if (type.find("Rifle") != std::string::npos)
            return 1;
        if (type.find("SubMachinegun") != std::string::npos || type.find("SMG") != std::string::npos)
            return 3;
        if (type.find("Shotgun") != std::string::npos)
            return 4;
        if (type.find("Machinegun") != std::string::npos || type.find("MachineGun") != std::string::npos)
            return 5;
        return 6;
    }

    // A type's picture, like Chadware's browser: the gun's model; for knives / gloves one of its skins (plain gloves
    // have no picture of their own), else its icon.
    UI::CardImage TypeImage(const ItemDefinitionInfo& type)
    {
        UI::CardImage image = ToCard(SkinImages::GetModelPreview(type));
        return image.texture ? image : DropImage(type.definitionIndex, 0);
    }

    // "Search..." with the "A-Z / Rarity / Popular" switch next to it (Config::menu.skinSort, saved with the menu).
    void SearchWithSort(const char* id, const char* hint, std::string* text)
    {
        if (!ImGui::BeginTable("##add_search_row", 2, ImGuiTableFlags_None))
            return;

        ImGui::TableSetupColumn("search", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("sort", ImGuiTableColumnFlags_WidthFixed, S(260.0f));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        UI::Search(id, hint, text);
        ImGui::TableNextColumn();

        constexpr const char* SORTS[] = { "A-Z", "Rarity", "Popular" };
        int sort = std::clamp(Config::menu.skinSort, 0, 2);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1.0f));

        if (UI::Segmented("##add_sort", SORTS, &sort))
        {
            Config::menu.skinSort = sort;
            MenuState::MarkDirty();
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Popular: the most wanted first - the famous ones that cost the most on the market (Titan Holo, Crown Foil, Dragon Lore...)."));

        ImGui::EndTable();
    }

    // Pictures of the item types (guns in their groups / knives / gloves). A click opens that type's finishes.
    void RenderAddTypes(const std::vector<const ItemDefinitionInfo*>& types, std::uint16_t& open, bool grouped, bool gold, float cardWidth)
    {
        if (ImGui::BeginChild("##add_types", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        {
            const std::string search = Lower(g_addTypeSearch);
            int shown = 0;

            const auto matches = [&](const ItemDefinitionInfo* type, int group)
            {
                return (group < 0 || WeaponGroupOf(*type) == group) && Matches(Lower(type->displayName), search);
            };

            const auto drawGroup = [&](int group)
            {
                const int matching = static_cast<int>(std::count_if(types.begin(), types.end(), [&](const ItemDefinitionInfo* type) { return matches(type, group); }));
                UI::Grid grid = grouped ? UI::MakeGrid(S(cardWidth)) : UI::MakeGridFitting(matching, S(cardWidth), S(260.0f));

                for (const ItemDefinitionInfo* type : types)
                {
                    if (!matches(type, group))
                        continue;

                    const std::uint16_t definition = type->definitionIndex;
                    const std::vector<PaintKitInfo>* finishes = Skins::g_ItemSchema.GetPaintKitsForItem(definition);
                    const int count = finishes ? static_cast<int>(std::count_if(finishes->begin(), finishes->end(), [](const PaintKitInfo& finish) { return finish.id != 0; })) : 0;

                    char id[40];
                    snprintf(id, sizeof(id), "add_type_%u", static_cast<unsigned>(definition));
                    char subtitle[48];
                    snprintf(subtitle, sizeof(subtitle), tr("Finishes: %d"), count);

                    UI::NextCell(grid);
                    ++shown;

                    if (UI::Card(id, grid.width, type->displayName, subtitle, gold ? TierColor(Cases::TierGold) : UI::Col::TextFaint,
                        g_addChoice.definition == definition, [&] { return TypeImage(*type); }))
                    {
                        open = definition;
                        g_addSearch.clear();
                    }
                }
            };

            if (grouped)
            {
                for (int group = 0; group < static_cast<int>(std::size(WEAPON_GROUPS)); ++group)
                {
                    if (std::none_of(types.begin(), types.end(), [&](const ItemDefinitionInfo* type) { return matches(type, group); }))
                        continue;

                    UI::SectionTitle(WEAPON_GROUPS[group]);
                    drawGroup(group);
                    UI::Gap(6.0f);
                }
            }
            else
            {
                drawGroup(-1);
            }

            if (shown == 0)
                UI::Note("Nothing found.");
        }
        ImGui::EndChild();
    }

    // One type's finishes: Back, its name, a search, the finish cards.
    void RenderAddFinishes(std::uint16_t& open, bool gold, const PickFn& pick, const char* id)
    {
        const ItemDefinitionInfo* type = Skins::g_ItemSchema.GetItem(open);

        if (!type)
        {
            open = 0;
            return;
        }

        if (UI::Button("< Back##add_back", ImVec2(0.0f, S(30.0f))))
        {
            open = 0;
            g_addSearch.clear();
            return;
        }

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::Text);
        ImGui::TextUnformatted(type->displayName.c_str());
        ImGui::PopStyleColor();

        if (ImGui::BeginChild("##add_skins", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        {
            const std::vector<const ItemDefinitionInfo*> only{ type };
            std::uint16_t current = open;
            RenderFinishPicker(id, g_addChoice.definition, g_addChoice.kit, pick, only, current, nullptr, gold, g_addSearch, false,
                std::clamp(Config::menu.skinSort, 0, 2));
        }
        ImGui::EndChild();
    }

    void RenderCollectibleList();   // Pins & coins (below)
    void RenderGlobalResults();     // the search at the top (below)

    // ── the list (left) ──
    void RenderAddList()
    {
        // one search over everything, with the A-Z / Rarity order; empty = the categories
        SearchWithSort("##add_global", "Search everything: skins, knives, gloves, agents, stickers...", &g_addGlobal);
        RenderAddCategoryTabs();

        if (!g_addGlobal.empty())
        {
            RenderGlobalResults();
            return;
        }

        const PickFn pick = [](std::uint16_t weapon, int paintKit, int tier) { g_addChoice = { weapon, paintKit, tier }; };

        // guns / knives / gloves: pictures of the types first (like Chadware), then the chosen one's finishes
        if (g_addCategory == AddGuns || g_addCategory == AddKnives || g_addCategory == AddGloves)
        {
            const bool guns = g_addCategory == AddGuns;
            const bool knives = g_addCategory == AddKnives;
            std::uint16_t& open = guns ? g_addGun : knives ? g_addKnife : g_addGlove;

            if (open == 0)
                RenderAddTypes(guns ? Guns() : knives ? Knives() : GloveTypes(), open, guns, !guns,
                    guns ? 140.0f : 108.0f);   // guns: bigger pictures (2026-10-18); knives / gloves: as big as fits (MakeGridFitting)
            else
                RenderAddFinishes(open, !guns, pick, guns ? "add_gun" : knives ? "add_knife" : "add_glove");
            return;
        }

        if (g_addCategory == AddAgents)
        {
            const auto& agents = Agents();
            const std::vector<int>& shown = Filtered(g_addCategory, static_cast<int>(agents.size()), [&](int i) { return agents[i]->displayName; },
                [&](int i) { return AgentTier(agents[i]->definitionIndex); }, [&](int i) { return PopularPrice(agents[i]->definitionIndex, 0); });

            ClippedGrid("##add_agents", static_cast<int>(shown.size()), [&](int index, float width)
            {
                const ItemDefinitionInfo* agent = agents[shown[index]];
                char id[40];
                snprintf(id, sizeof(id), "add_agent_%u", static_cast<unsigned>(agent->definitionIndex));
                const int tier = AgentTier(agent->definitionIndex);

                if (UI::Card(id, width, agent->displayName, agent->isTerroristAgent ? "T" : "CT", TierColor(tier), g_addChoice.definition == agent->definitionIndex,
                    [&] { return DropImage(agent->definitionIndex, 0); }))
                    g_addChoice = { agent->definitionIndex, 0, tier };
            });
            return;
        }

        if (g_addCategory == AddCharms)
        {
            const auto& charms = Skins::g_ItemSchema.GetKeychains();
            const std::vector<int>& list = CharmList();
            const std::vector<int>& shown = Filtered(g_addCategory, static_cast<int>(list.size()),
                [&](int i) { return charms[list[i]].displayName + EventSearchText(charms[list[i]].id); },   // found by a clip too
                [&](int i) { return CharmTier(charms[list[i]]); }, [&](int i) { return PopularPrice(Unboxer::DEF_CHARM, charms[list[i]].id); });

            ClippedGrid("##add_charms", static_cast<int>(shown.size()), [&](int index, float width)
            {
                const KeychainInfo& charm = charms[list[shown[index]]];
                char id[40];
                snprintf(id, sizeof(id), "add_charm_%d", charm.id);
                const int tier = CharmTier(charm);
                const std::size_t clips = Cases::HighlightsOf(charm.id).size();
                const std::string subtitle = clips ? std::to_string(clips) + " " + tr("clips") : std::string(tr(TierLabel(tier, false)));
                const bool selected = g_addChoice.definition == Unboxer::DEF_CHARM &&
                    (g_addChoice.kit == charm.id || (clips && Cases::HighlightEvent(g_addChoice.kit) == charm.id));

                if (UI::Card(id, width, charm.displayName, subtitle, TierColor(tier), selected, [&] { return GameOrExtraImage(charm.image); }))
                {
                    if (clips)   // searched "flamez": that clip, and the list shows flameZ's clips
                        g_addClipSearch = PickHighlightEvent(charm.id, tier, Lower(g_addSearch)) ? g_addSearch : std::string();
                    else
                        g_addChoice = { Unboxer::DEF_CHARM, charm.id, tier };
                }
            });
            return;
        }

        if (g_addCategory == AddPets)
        {
            struct PetCard { std::uint16_t definition; const char* name; };
            static constexpr PetCard pets[] = { { Unboxer::DEF_CHICKEN_EGG, "Chicken Egg" }, { Unboxer::DEF_PET, "Chicken" }, { Unboxer::DEF_CHICKEN_FEED, "Chicken Feed" } };

            UI::Grid grid = UI::MakeGridFitting(static_cast<int>(std::size(pets)), S(108.0f), S(240.0f));
            for (const PetCard& pet : pets)
            {
                UI::NextCell(grid);
                char id[32];
                snprintf(id, sizeof(id), "add_pet_%u", static_cast<unsigned>(pet.definition));

                if (UI::Card(id, grid.width, tr(pet.name), tr("Pet"), TierColor(Cases::TierMilSpec), g_addChoice.definition == pet.definition, [&]
                    {
                        return pet.definition == Unboxer::DEF_PET ? ToCard(SkinImages::GetUiIcon("pet_adult")) : DropImage(pet.definition, 0);
                    }))
                    g_addChoice = { pet.definition, 0, Cases::TierMilSpec };
            }
            return;
        }

        // tools: a Storage Unit (name it, then put Lootline items in), a Name Tag, the StatTrak Swap Tool, Charm Detachments
        if (g_addCategory == AddTools)
        {
            struct ToolCard { std::uint16_t definition; const char* name; };
            static constexpr ToolCard tools[] = {
                { Unboxer::DEF_CASKET, "Storage Unit" }, { Unboxer::DEF_NAME_TAG_TOOL, "Name Tag" },
                { Unboxer::DEF_STATTRAK_SWAP, "StatTrak Swap Tool" }, { Unboxer::DEF_CHARM_DETACH_PACK, "Charm Detachment Pack" },
            };

            UI::Grid grid = UI::MakeGridFitting(static_cast<int>(std::size(tools)), S(108.0f), S(240.0f));
            for (const ToolCard& tool : tools)
            {
                UI::NextCell(grid);
                char id[32];
                snprintf(id, sizeof(id), "add_tool_%u", static_cast<unsigned>(tool.definition));

                if (UI::Card(id, grid.width, tr(tool.name), tr("Tool"), TierColor(Cases::TierConsumer), g_addChoice.definition == tool.definition,
                    [&] { return DropImage(tool.definition, 0); }))
                    g_addChoice = { tool.definition, 0, Cases::TierConsumer };
            }

            UI::Hint("A Storage Unit: give it a name on the right (or name it in CS2: right-click -> Name), then put Lootline items in and take them out like a real one.");
            return;
        }

        // pins, coins, medals, trophies: one card each, its grades on the right
        if (g_addCategory == AddCollectibles)
        {
            RenderCollectibleList();
            return;
        }

        // stickers, patches, graffiti, music kits, collectibles: everything the game has
        const Cases::DropKind kind = g_addCategory == AddStickers ? Cases::DropKind::Sticker : g_addCategory == AddPatches ? Cases::DropKind::Patch :
            g_addCategory == AddGraffiti ? Cases::DropKind::Graffiti : g_addCategory == AddMusic ? Cases::DropKind::MusicKit : Cases::DropKind::Pin;
        const std::uint16_t base = kind == Cases::DropKind::Sticker ? Cases::DEF_STICKER : kind == Cases::DropKind::Patch ? Cases::DEF_PATCH :
            kind == Cases::DropKind::Graffiti ? Cases::DEF_GRAFFITI : kind == Cases::DropKind::MusicKit ? Cases::DEF_MUSIC_KIT : 0;

        const std::vector<Cases::Kit>& kits = Cases::AllKits(kind);
        const std::vector<int>& shown = Filtered(g_addCategory, static_cast<int>(kits.size()), [&](int i) { return kits[i].displayName + " " + kits[i].english; },
            [&](int i) { return kits[i].tier; },
            [&](int i) { return PopularPrice(base ? base : static_cast<std::uint16_t>(kits[i].id), base ? kits[i].id : 0); });

        ClippedGrid("##add_kits", static_cast<int>(shown.size()), [&](int index, float width)
        {
            const Cases::Kit& kit = kits[shown[index]];
            const std::uint16_t definition = base ? base : static_cast<std::uint16_t>(kit.id);   // collectibles are their own definition
            const int kitId = base ? kit.id : 0;
            char id[48];
            snprintf(id, sizeof(id), "add_kit_%d_%d", static_cast<int>(kind), kit.id);

            if (UI::Card(id, width, kit.displayName, tr(TierLabel(kit.tier, false)), TierColor(kit.tier),
                g_addChoice.definition == definition && g_addChoice.kit == kitId, [&] { return GameOrExtraImage(kit.image); }))
                g_addChoice = { definition, kitId, kit.tier };
        });
    }

    // ── the options (right) ──

    // "Random float": the picked wear's floats this finish really has (CS2's float range: a Doppler 0 - 0.08, an Asiimov
    // 0.18 - 1). A wear the finish doesn't have (Factory New Asiimov): its float nearest to that wear. exists = it has it.
    // Add items' "Random float": the picked wear in the picked finish (RandomFloatRange)
    Unboxer::RandomFloat AddRandomFloat()
    {
        return RandomFloatRange(g_addRandomWear, g_addChoice.kit);
    }

    // The float and the pattern: fixed (one for all) or random (every one added its own) - two small switches.
    void RenderWearControls()
    {
        RenderFloatAndSeed("add_skin", g_addChoice.kit, &g_addRandomFloat, &g_addWear, &g_addRandomWear, &g_addRandomSeed, &g_addSeed, true);
    }

    void RenderNameTag()
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr("Name tag"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##add_name", tr("No name tag"), g_addName, 21);
    }

    // A graffiti that comes in colours: which one (19 swatches).
    int g_addTint = Cases::TINT_COUNT;   // Shark White

    // A Major highlight charm's clip: the event's clips as a list - the clip's name, its match (teams, map). The search
    // finds a clip, a player (in the clip's id: "flamez"), a team or a map.
    void RenderClipPicker(int event)
    {
        const std::vector<const Cases::Highlight*>& clips = Cases::HighlightsOf(event);
        UI::Search("##add_clip_search", "Search clips, players, teams...", &g_addClipSearch);

        const std::string search = Lower(g_addClipSearch);
        std::vector<const Cases::Highlight*> shown;
        shown.reserve(clips.size());

        for (const Cases::Highlight* clip : clips)
            if (HasAllWords(clip->search, search))
                shown.push_back(clip);

        if (shown.empty())
        {
            UI::Note("No clip matches the search.");
            return;
        }

        const float line = ImGui::GetTextLineHeight();
        const float rowHeight = line * 2.0f + S(8.0f);
        const float step = rowHeight + ImGui::GetStyle().ItemSpacing.y;
        const float height = std::min(S(250.0f), step * static_cast<float>(shown.size()) + ImGui::GetStyle().WindowPadding.y * 2.0f);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, UI::Col::Raised);
        if (ImGui::BeginChild("##add_clips", ImVec2(-FLT_MIN, height)))
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()), step);

            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const Cases::Highlight* clip = shown[static_cast<std::size_t>(row)];
                    ImGui::PushID(clip->number);
                    const ImVec2 min = ImGui::GetCursorScreenPos();

                    if (ImGui::Selectable("##clip", g_addChoice.highlight == clip->number, ImGuiSelectableFlags_None, ImVec2(0.0f, rowHeight)))
                    {
                        g_addChoice.kit = clip->keychain;   // a clip's own charm (Cologne 2026 on) / the event's
                        g_addChoice.highlight = clip->number;
                    }

                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    draw->AddText(ImVec2(min.x + S(6.0f), min.y + S(4.0f)), UI::Col::Text, clip->clipName.c_str());

                    if (!clip->match.empty())
                        draw->AddText(ImVec2(min.x + S(6.0f), min.y + S(4.0f) + line), UI::Col::TextDim, clip->match.c_str());

                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void RenderTintPicker()
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s: %s", tr("Colour"), Cases::TintName(g_addTint));

        const float size = S(26.0f);
        const float spacing = ImGui::GetStyle().ItemSpacing.x * 0.5f;
        const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (size + spacing)));

        for (int tint = 1; tint <= Cases::TINT_COUNT; ++tint)
        {
            if ((tint - 1) % perRow)
                ImGui::SameLine(0.0f, spacing);

            const std::uint32_t rgb = Cases::TintRgb(tint);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            char id[32];
            snprintf(id, sizeof(id), "##tint_%d", tint);

            if (ImGui::InvisibleButton(id, ImVec2(size, size)))
                g_addTint = tint;

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Cases::TintName(tint));

            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 max(min.x + size, min.y + size);
            draw->AddRectFilled(min, max, IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255), S(6.0f));

            if (tint == g_addTint)
                draw->AddRect(ImVec2(min.x - S(2.0f), min.y - S(2.0f)), ImVec2(max.x + S(2.0f), max.y + S(2.0f)), UI::Col::Text, S(7.0f), 0, S(2.0f));
        }
    }

    // The Sticker Slab's sealed sticker (Add items: the slab charm, or the slab on a gun). None: an empty slab.
    int g_addSlabSticker = 0;
    std::string g_addSlabSearch;

    void RenderSlabStickerPicker(bool onGun)
    {
        const std::vector<Cases::Kit>& stickers = Cases::AllKits(Cases::DropKind::Sticker);
        const Cases::Kit* current = g_addSlabSticker > 0 ? Cases::FindKit(Cases::DropKind::Sticker, g_addSlabSticker) : nullptr;

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr("Sticker in the slab"));
        ImGui::SetNextItemWidth(-FLT_MIN);

        if (ImGui::BeginCombo("##add_slab_sticker", current ? current->displayName.c_str() : tr("Empty"), ImGuiComboFlags_HeightLarge))
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##add_slab_search", tr("Search..."), &g_addSlabSearch);

            if (ImGui::Selectable(tr("Empty"), g_addSlabSticker == 0))
                g_addSlabSticker = 0;

            const std::string search = Lower(g_addSlabSearch);
            std::vector<int> shown;
            shown.reserve(stickers.size());

            for (int i = 0; i < static_cast<int>(stickers.size()); ++i)
                if (search.empty() || Lower(stickers[i].displayName).find(search) != std::string::npos)
                    shown.push_back(i);

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()));

            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const Cases::Kit& kit = stickers[shown[row]];
                    char label[192];
                    snprintf(label, sizeof(label), "%s##slab_%d", kit.displayName.c_str(), kit.id);

                    if (ImGui::Selectable(label, g_addSlabSticker == kit.id))
                        g_addSlabSticker = kit.id;
                }
            }

            ImGui::EndCombo();
        }

        if (!onGun)
            UI::Hint(g_addSlabSticker > 0 ? "A slab with this sticker sealed in: hang it on any gun." :
                "Empty: CS2's Sticker Slab - right-click a sticker -> Seal in Sticker Slab.");
    }

    void RenderCharmPicker()
    {
        const auto& charms = Skins::g_ItemSchema.GetKeychains();
        const KeychainInfo* current = g_addCharm ? Skins::g_ItemSchema.GetKeychain(g_addCharm) : nullptr;

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr("Charm"));
        ImGui::SetNextItemWidth(-FLT_MIN);

        if (ImGui::BeginCombo("##add_charm", current ? current->displayName.c_str() : tr("No charm"), ImGuiComboFlags_HeightLarge))
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##add_charm_search", tr("Search..."), &g_addCharmSearch);

            if (ImGui::Selectable(tr("No charm"), g_addCharm == 0))
                g_addCharm = 0;

            const std::string search = Lower(g_addCharmSearch);
            for (const KeychainInfo& charm : charms)
            {
                if (!search.empty() && Lower(charm.displayName).find(search) == std::string::npos)
                    continue;

                char label[160];
                snprintf(label, sizeof(label), "%s##charm_%d", charm.displayName.c_str(), charm.id);
                if (ImGui::Selectable(label, g_addCharm == charm.id))
                    g_addCharm = charm.id;
            }

            ImGui::EndCombo();
        }

        if (g_addCharm)
            UI::Stepper("##add_charm_seed", "Charm pattern", &g_addCharmSeed, 0, 100000);

        if (g_addCharm == Unboxer::KEYCHAIN_STICKER_SLAB)
            RenderSlabStickerPicker(true);
    }

    void RenderPetOptions()
    {
        static const char* const stages[] = { "Chick", "Pullet", "Hen" };
        UI::Segmented("##add_stage", stages, &g_addStage);

        static const char* const breeds[] = { "Catalana", "Silkie", "Polish" };
        ImGui::BeginDisabled(g_addStage == 0);
        UI::Segmented("##add_breed", breeds, &g_addBreed);
        ImGui::EndDisabled();

        UI::Stepper("##add_pet_seed", "Colour (seed)", &g_addPetSeed, 0, 9999);

        if (UI::Button("Random colour##add_pet_random", ImVec2(-1.0f, 0.0f), UI::Kind::Ghost))
            g_addPetSeed = static_cast<int>(NowMs() % 10000ull);

        UI::Hint(g_addStage == 0 ? "A chick: the breed shows once it grows." : "It stays this age and is always fed. Equip it in CS2's loadout (Pet).");
    }

    Unboxer::Item BuildAddItem()
    {
        const bool skin = IsSkinCategory(g_addCategory);
        const bool gold = g_addCategory == AddKnives || g_addCategory == AddGloves;
        const bool canStatTrak = g_addCategory == AddGuns || g_addCategory == AddKnives || g_addCategory == AddMusic;

        Unboxer::Item item;
        item.kind = Unboxer::Kind::Drop;
        item.definition = g_addChoice.definition;
        item.paintKit = g_addChoice.kit;

        if (g_addChoice.definition == Unboxer::DEF_CHARM)
            item.highlight = g_addChoice.highlight;   // a Major's highlight charm: its clip

        if (skin)
        {
            if (g_addRandomFloat)
            {
                const Unboxer::RandomFloat range = AddRandomFloat();   // the preview / name: the middle of it
                item.wear = (range.from + range.to) * 0.5f;
            }
            else
            {
                item.wear = std::clamp(g_addWear, 0.0f, 1.0f);
            }

            item.seed = std::clamp(g_addSeed, 0, 1000);
        }

        item.statTrak = canStatTrak && g_addStatTrak;
        item.kills = item.statTrak && !g_addRealKills && g_addCategory != AddMusic ? std::max(0, g_addKills) : -1;

        const bool souvenir = g_addCategory == AddGuns && g_addSouvenir && !item.statTrak;
        item.quality = gold ? 3 : souvenir ? 12 : item.statTrak ? 9 : 4;
        item.rarity = Cases::Rarity(gold ? Cases::TierGold : static_cast<Cases::Tier>(std::clamp(g_addChoice.tier, 0, static_cast<int>(Cases::TierCovert))));

        if (skin || g_addChoice.definition == Unboxer::DEF_PET || g_addChoice.definition == Unboxer::DEF_CASKET)
            item.customName = g_addName;   // a Storage Unit with a name comes ready to fill

        if (g_addCategory == AddGuns && g_addCharm)
        {
            item.charm = g_addCharm;
            item.charmSeed = g_addCharmSeed;
        }

        // a graffiti that comes in colours: the picked colour
        if (Cases::KindOf(item.definition) == Cases::DropKind::Graffiti)
            if (const Cases::Kit* kit = Cases::FindKit(Cases::DropKind::Graffiti, item.paintKit); kit && kit->tintable)
                item.tint = std::clamp(g_addTint, 1, Cases::TINT_COUNT);

        // the slab on a gun: its sticker
        if (g_addCategory == AddGuns && g_addCharm == Unboxer::KEYCHAIN_STICKER_SLAB)
            item.charmSticker = g_addSlabSticker;

        // the Sticker Slab: with a sticker picked, the slab charm with it sealed in; empty, CS2's Sticker Slab tool
        if (g_addChoice.definition == Unboxer::DEF_CHARM && g_addChoice.kit == Unboxer::KEYCHAIN_STICKER_SLAB)
        {
            if (g_addSlabSticker > 0)
            {
                item.charmSticker = g_addSlabSticker;
            }
            else
            {
                item.definition = Unboxer::DEF_STICKER_SLAB;
                item.paintKit = 0;
            }
        }

        if (g_addChoice.definition == Unboxer::DEF_PET)
        {
            item.petLevel = g_addStage + 1;
            item.petId = g_addStage == 0 ? 2 : 3 + std::clamp(g_addBreed, 0, 2);
            item.petSeed = g_addPetSeed;
        }

        return item;
    }

    // ── the 3D preview (Chadware's model preview): guns, knives, gloves, agents ──
    bool g_addPreviewActive = false;   // a 3D preview was asked for (CS2's hidden preview panel is alive)

    // nothing picked: no preview
    void KeepAddItemsPreviewLocal()
    {
        if (g_addPreviewActive)
        {
            ModelPreview::Close();
            g_addPreviewActive = false;
        }
    }

    std::optional<ModelPreview::Request> AddPreviewRequest(const Unboxer::Item& item)
    {
        const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(item.definition);

        if (!info)
            return std::nullopt;

        if (info->isAgent)
            return info->hasUsableAgentModel ? std::optional<ModelPreview::Request>(ModelPreview::MakeAgentRequest(item.definition)) : std::nullopt;

        if (!info->isWeapon && !info->isKnife && !info->isGlove)
            return std::nullopt;

        ModelPreview::Request request{};
        request.presentation = ModelPreview::Presentation::Weapon;
        request.definitionIndex = item.definition;
        request.paintKit = item.paintKit;
        request.seed = item.seed;
        request.wear = std::max(item.wear, 0.0001f);
        request.statTrakEnabled = item.statTrak;
        request.statTrakKills = Unboxer::ShownKills(item);
        request.customName = item.customName;

        if (const PaintKitInfo* kit = item.paintKit > 0 ? Skins::g_ItemSchema.GetPaintKit(item.paintKit) : nullptr)
            request.rarity = kit->rarity;

        for (std::size_t slot = 0; slot < request.stickers.size() && slot < item.stickers.size(); ++slot)
        {
            const Unboxer::Sticker& sticker = item.stickers[slot];
            request.stickers[slot] = { sticker.kit > 0, sticker.kit, sticker.wear, 1.0f, sticker.rotation, sticker.offsetX, sticker.offsetY };
        }

        if (item.charm > 0)
        {
            request.keychain = { true, item.charm, item.charmSeed, item.charmX, item.charmY, item.charmZ };
            request.keychain.sticker = item.charmSticker;
        }

        request.paint = item.paint;
        return request;
    }

    // The item's 3D preview (drag: turn, wheel: zoom, right click: reset) once CS2 has drawn it; its picture until then.
    // What's on an item - its stickers, then its charm - for the small read-only tiles (CS2's own tools put them on).
    struct OnItTile
    {
        UI::CardImage image;
        std::string name;
        bool charm = false;
    };

    std::vector<OnItTile> OnItTiles(const Unboxer::Item& item)
    {
        std::vector<OnItTile> tiles;

        for (const Unboxer::Sticker& sticker : item.stickers)
        {
            if (sticker.kit <= 0)
                continue;

            const Cases::Kit* kit = Cases::FindKit(Cases::DropKind::Sticker, sticker.kit);
            std::string name = kit ? kit->displayName : "Sticker " + std::to_string(sticker.kit);

            if (sticker.wear > 0.0f)
            {
                char scraped[48];
                snprintf(scraped, sizeof(scraped), " (%s %.0f%%)", tr("scraped"), sticker.wear * 100.0f);
                name += scraped;
            }

            tiles.push_back({ kit ? GameOrExtraImage(kit->image) : UI::CardImage{}, std::move(name), false });
        }

        if (item.charm > 0)
        {
            const KeychainInfo* charm = Skins::g_ItemSchema.GetKeychain(item.charm);
            std::string name = charm ? charm->displayName : "Charm " + std::to_string(item.charm);

            if (item.charmSticker > 0)
            {
                const Cases::Kit* sealed = Cases::FindKit(Cases::DropKind::Sticker, item.charmSticker);
                name += " - " + (sealed ? sealed->displayName : std::to_string(item.charmSticker));
            }

            char pattern[48];
            snprintf(pattern, sizeof(pattern), " (%s %d)", tr("pattern"), item.charmSeed);
            name += pattern;
            tiles.push_back({ charm ? GameOrExtraImage(charm->image) : UI::CardImage{}, std::move(name), true });
        }

        return tiles;
    }

    // Above the picture: the stickers and the charm, read-only (hover: the name).
    void RenderOnItRow(const Unboxer::Item& item)
    {
        const std::vector<OnItTile> tiles = OnItTiles(item);

        if (tiles.empty())
            return;

        const float size = S(40.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();

        for (std::size_t i = 0; i < tiles.size(); ++i)
        {
            if (i)
                ImGui::SameLine(0.0f, tiles[i].charm ? S(14.0f) : S(6.0f));

            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::PushID(static_cast<int>(i));
            ImGui::InvisibleButton("##on_it", ImVec2(size, size));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const ImVec2 max(min.x + size, min.y + size);
            draw->AddRectFilled(min, max, hovered ? UI::Col::Hover : UI::Col::Raised, S(8.0f));
            UI::DrawImage(draw, tiles[i].image, ImVec2(min.x + S(4.0f), min.y + S(4.0f)), ImVec2(max.x - S(4.0f), max.y - S(4.0f)));

            if (hovered)
                ImGui::SetTooltip("%s", tiles[i].name.c_str());
        }
    }

    void DrawCardOnIt(const Unboxer::Item& item, const ImVec2& cardMin, float cardWidth)
    {
        const std::vector<OnItTile> tiles = OnItTiles(item);

        if (tiles.empty() || !ImGui::IsRectVisible(cardMin, ImVec2(cardMin.x + cardWidth, cardMin.y + cardWidth)))
            return;

        const float icon = std::clamp(std::floor(cardWidth * 0.12f), S(14.0f), S(22.0f));
        const float imageHeight = std::floor(cardWidth * 0.64f);
        const float y = cardMin.y + imageHeight - S(8.0f) - icon;
        const float right = cardMin.x + cardWidth - S(6.0f);
        float x = cardMin.x + S(7.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();

        for (const OnItTile& tile : tiles)
        {
            if (tile.charm)
                x += S(4.0f);

            if (x + icon > right)
                break;

            draw->AddRectFilled(ImVec2(x - S(1.0f), y - S(1.0f)), ImVec2(x + icon + S(1.0f), y + icon + S(1.0f)), IM_COL32(0, 0, 0, 120), S(4.0f));
            UI::DrawImage(draw, tile.image, ImVec2(x, y), ImVec2(x + icon, y + icon));
            x += icon + S(3.0f);
        }
    }

    void RenderAddPicture(const Unboxer::Item& item, int tier, const std::optional<ModelPreview::Request>& request)
    {
        RenderOnItRow(item);

        const float width = ImGui::GetContentRegionAvail().x;
        const ItemDefinitionInfo* pictured = Skins::g_ItemSchema.GetItem(item.definition);
        const bool gloves = pictured && pictured->isGlove;   // gloves: a bit taller (they're small in the frame)
        const float height = std::floor(std::min(width * (gloves ? 0.82f : 0.7f), S(gloves ? 360.0f : 320.0f)));
        const ImU32 glow = TierColor(tier);

        if (request && ModelPreview::GetFrame().valid)
        {
            // the rarity's backdrop (CS2's loadout card look, like the pictures' Showcase) BEHIND the 3D model: drawn first, in
            // this window - the preview is its own child window on top, rendered with a transparent background, so the
            // skin stays in front of the colour
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + width, min.y + height);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(min, max, UI::Col::Raised, S(10.0f));
            UI::RarityBackdrop(draw, min, max, glow, UI::Col::Raised, 1.0f, S(10.0f));

            ModelPreviewWidget::Render(*request, width, height);
            return;
        }

        Showcase(ItemImage(item), width, height, glow);

        if (request)
            UI::Note(ModelPreview::GetStatusText());
    }

    // ── Pins & coins: a medal / coin with grades (service medal levels, bronze / silver / gold...) is one card ──
    struct CollectibleGroup
    {
        std::string name;                      // without "Bronze " / "Silver " ...
        std::string search;                    // lowercase name + English
        int tier = 0;                          // the best grade's
        std::vector<const Cases::Kit*> grades; // by definition (the game's order)
    };

    std::vector<CollectibleGroup> g_collectibleGroups;   // render thread, made once

    const std::vector<CollectibleGroup>& CollectibleGroups()
    {
        if (!g_collectibleGroups.empty() || !Cases::Ready())
            return g_collectibleGroups;

        std::unordered_map<std::string, std::size_t> byName;

        for (const Cases::Kit& kit : Cases::AllKits(Cases::DropKind::Pin))
        {
            std::string name = kit.displayName;

            for (const char* grade : { "Bronze ", "Silver ", "Gold ", "Diamond ", "Crystal ", "Platinum " })
            {
                if (name.rfind(grade, 0) == 0)
                {
                    name.erase(0, std::strlen(grade));
                    break;
                }
            }

            const auto [it, added] = byName.try_emplace(name, g_collectibleGroups.size());

            if (added)
                g_collectibleGroups.push_back({ name, Lower(name + " " + kit.english), kit.tier, {} });

            CollectibleGroup& group = g_collectibleGroups[it->second];
            group.grades.push_back(&kit);
            group.tier = std::max(group.tier, kit.tier);
        }

        for (CollectibleGroup& group : g_collectibleGroups)
            std::sort(group.grades.begin(), group.grades.end(), [](const Cases::Kit* a, const Cases::Kit* b) { return a->id < b->id; });

        return g_collectibleGroups;
    }

    const CollectibleGroup* CollectibleGroupOf(int definition)
    {
        for (const CollectibleGroup& group : CollectibleGroups())
            for (const Cases::Kit* kit : group.grades)
                if (kit->id == definition)
                    return &group;

        return nullptr;
    }

    void RenderCollectibleList()
    {
        const std::vector<CollectibleGroup>& groups = CollectibleGroups();
        const std::string search = Lower(g_addSearch);
        std::vector<int> shown;

        for (int i = 0; i < static_cast<int>(groups.size()); ++i)
            if (search.empty() || groups[i].search.find(search) != std::string::npos)
                shown.push_back(i);

        if (Config::menu.skinSort == 1)
            std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) { return groups[a].tier > groups[b].tier; });
        else if (Config::menu.skinSort == 2)
        {
            // Popular: a pin / coin's priciest grade
            std::unordered_map<int, double> popular;

            for (const int i : shown)
            {
                double best = 0.0;

                for (const Cases::Kit* grade : groups[i].grades)
                    best = std::max(best, PopularPrice(static_cast<std::uint16_t>(grade->id), 0));

                popular[i] = best;
            }

            std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) { return popular[a] > popular[b]; });
        }

        ClippedGrid("##add_collectibles", static_cast<int>(shown.size()), [&](int index, float width)
        {
            const CollectibleGroup& group = groups[shown[index]];
            const Cases::Kit* pictured = group.grades.front();

            for (const Cases::Kit* kit : group.grades)
                if (kit->id == g_addChoice.definition)
                    pictured = kit;

            char id[48];
            snprintf(id, sizeof(id), "add_collectible_%d", group.grades.front()->id);
            char subtitle[48];

            if (group.grades.size() > 1)
                snprintf(subtitle, sizeof(subtitle), tr("%d grades"), static_cast<int>(group.grades.size()));
            else
                snprintf(subtitle, sizeof(subtitle), "%s", tr(TierLabel(pictured->tier, false)));

            if (UI::Card(id, width, group.name, subtitle, TierColor(pictured->tier), pictured->id == g_addChoice.definition,
                [&] { return GameOrExtraImage(pictured->image); }))
                g_addChoice = { static_cast<std::uint16_t>(pictured->id), 0, pictured->tier };
        });
    }

    // ── the search at the top: every category at once ──
    struct GlobalHit
    {
        int category = 0;               // AddCategory
        std::uint16_t definition = 0;
        int kit = 0;                    // finish / sticker / charm... (as g_addChoice)
        int tier = 0;
        std::string name;
        std::string search;             // lowercase name + English + the category's word
    };

    std::vector<GlobalHit> g_globalIndex;   // render thread, made the first time you search

    struct GlobalResults
    {
        std::string search;
        int category = -2;
        int sort = -1;
        std::vector<int> hits;          // into g_globalIndex
        int counts[AddCount]{};         // matches per category (the chips)
    };

    GlobalResults g_globalResults;

    // What a result card says under its name (and a word you can search by: "knife", "sticker"...).
    const char* CategoryWord(int category)
    {
        static constexpr const char* words[AddCount] = { "Gun", "Knife", "Gloves", "Agent", "Sticker", "Patch", "Graffiti", "Charm", "Music kit",
            "Collectible", "Pet", "Tool" };
        return words[std::clamp(category, 0, static_cast<int>(AddCount) - 1)];
    }

    void BuildGlobalIndex()
    {
        if (!g_globalIndex.empty() || !Cases::Ready())
            return;

        const auto add = [](int category, std::uint16_t definition, int kit, int tier, std::string name, const std::string& english)
        {
            GlobalHit hit;
            hit.category = category;
            hit.definition = definition;
            hit.kit = kit;
            hit.tier = tier;
            hit.search = Lower(name + " " + english + " " + CategoryWord(category));
            hit.name = std::move(name);
            g_globalIndex.push_back(std::move(hit));
        };

        const auto finishName = [](const PaintKitInfo& finish)
        {
            std::string name = finish.displayName.empty() ? finish.name : finish.displayName;

            if (const std::string phase = PhaseOf(finish.id); !phase.empty())
                name += " (" + phase + ")";

            return name;
        };

        for (const ItemDefinitionInfo* gun : Guns())
            if (const std::vector<PaintKitInfo>* finishes = Skins::g_ItemSchema.GetPaintKitsForItem(gun->definitionIndex))
                for (const PaintKitInfo& finish : *finishes)
                    if (finish.id != 0)
                        add(AddGuns, gun->definitionIndex, finish.id, SkinTier(gun->definitionIndex, finish), gun->displayName + " | " + finishName(finish), finish.name);

        for (const bool knives : { true, false })
            for (const ItemDefinitionInfo* type : knives ? Knives() : GloveTypes())
                if (const std::vector<PaintKitInfo>* finishes = Skins::g_ItemSchema.GetPaintKitsForItem(type->definitionIndex))
                    for (const PaintKitInfo& finish : *finishes)
                    {
                        if (finish.id == 0 && !knives)
                            continue;   // plain gloves aren't an item

                        std::string name = "\xE2\x98\x85 " + type->displayName + (finish.id == 0 ? std::string() : " | " + finishName(finish));
                        add(knives ? AddKnives : AddGloves, type->definitionIndex, finish.id, Cases::TierGold, std::move(name), finish.name);
                    }

        for (const ItemDefinitionInfo* agent : Agents())
            add(AddAgents, agent->definitionIndex, 0, AgentTier(agent->definitionIndex), agent->displayName, agent->isTerroristAgent ? "T" : "CT");

        struct KitSource
        {
            int category;
            Cases::DropKind kind;
            std::uint16_t base;
        };

        const KitSource sources[] = {
            { AddStickers, Cases::DropKind::Sticker, Cases::DEF_STICKER }, { AddPatches, Cases::DropKind::Patch, Cases::DEF_PATCH },
            { AddGraffiti, Cases::DropKind::Graffiti, Cases::DEF_GRAFFITI }, { AddMusic, Cases::DropKind::MusicKit, Cases::DEF_MUSIC_KIT },
        };

        for (const KitSource& source : sources)
            for (const Cases::Kit& kit : Cases::AllKits(source.kind))
                add(source.category, source.base, kit.id, kit.tier, kit.displayName, kit.english);

        for (const KeychainInfo& charm : Skins::g_ItemSchema.GetKeychains())
            if (charm.base.empty())   // a clip's own charm is found through its event's (one card per Major)
                add(AddCharms, Unboxer::DEF_CHARM, charm.id, CharmTier(charm), charm.displayName, EventSearchText(charm.id));

        for (const CollectibleGroup& group : CollectibleGroups())
            add(AddCollectibles, static_cast<std::uint16_t>(group.grades.front()->id), 0, group.tier, group.name, group.search);

        add(AddPets, Unboxer::DEF_CHICKEN_EGG, 0, Cases::TierMilSpec, "Chicken Egg", "chicken egg");
        add(AddPets, Unboxer::DEF_PET, 0, Cases::TierMilSpec, "Chicken", "chicken");
        add(AddPets, Unboxer::DEF_CHICKEN_FEED, 0, Cases::TierMilSpec, "Chicken Feed", "chicken feed");
    }

    void UpdateGlobalResults()
    {
        const int sort = std::clamp(Config::menu.skinSort, 0, 2);
        GlobalResults& results = g_globalResults;

        if (results.search == g_addGlobal && results.category == g_addGlobalCategory && results.sort == sort)
            return;

        results.search = g_addGlobal;
        results.category = g_addGlobalCategory;
        results.sort = sort;
        results.hits.clear();
        std::fill(std::begin(results.counts), std::end(results.counts), 0);

        // every word has to be in it: "ak redline" finds "AK-47 | Redline"
        std::vector<std::string> words;
        {
            const std::string text = Lower(g_addGlobal);
            std::size_t start = 0;

            while (start < text.size())
            {
                const std::size_t end = text.find(' ', start);
                const std::size_t stop = end == std::string::npos ? text.size() : end;

                if (stop > start)
                    words.push_back(text.substr(start, stop - start));

                start = stop + 1;
            }
        }

        if (words.empty())
            return;

        for (int i = 0; i < static_cast<int>(g_globalIndex.size()); ++i)
        {
            const GlobalHit& hit = g_globalIndex[static_cast<std::size_t>(i)];

            if (!std::all_of(words.begin(), words.end(), [&](const std::string& word) { return hit.search.find(word) != std::string::npos; }))
                continue;

            ++results.counts[hit.category];

            if (g_addGlobalCategory < 0 || hit.category == g_addGlobalCategory)
                results.hits.push_back(i);
        }

        // Popular: each hit's price once (pets: none)
        std::unordered_map<int, double> popular;

        if (sort == 2)
            for (const int i : results.hits)
            {
                const GlobalHit& hit = g_globalIndex[static_cast<std::size_t>(i)];
                popular[i] = hit.category == AddPets ? 0.0 : PopularPrice(hit.definition, hit.kit);
            }

        // names that start with what you typed first, then (Rarity) the best / (Popular) the most wanted, then by category and name
        const std::string& first = words.front();
        std::stable_sort(results.hits.begin(), results.hits.end(), [&](int a, int b)
        {
            const GlobalHit& x = g_globalIndex[static_cast<std::size_t>(a)];
            const GlobalHit& y = g_globalIndex[static_cast<std::size_t>(b)];
            const bool xStarts = x.search.starts_with(first), yStarts = y.search.starts_with(first);

            if (xStarts != yStarts)
                return xStarts;

            if (sort == 1 && x.tier != y.tier)
                return x.tier > y.tier;

            if (sort == 2)
                if (const double px = popular[a], py = popular[b]; px != py)
                    return px > py;

            if (x.category != y.category)
                return x.category < y.category;

            return x.search < y.search;
        });
    }

    // A result clicked: picked as if you'd browsed to it (its category, its gun / knife / gloves opened).
    void PickGlobal(const GlobalHit& hit)
    {
        if (g_addCategory != hit.category)
            g_addFilter.ready = false;

        g_addCategory = hit.category;
        g_addChoice = { hit.definition, hit.kit, hit.tier };

        // a Major's highlight charm found by a clip ("flamez"): that clip, and the clip list shows the matches
        if (hit.definition == Unboxer::DEF_CHARM && !Cases::HighlightsOf(hit.kit).empty())
            g_addClipSearch = PickHighlightEvent(hit.kit, hit.tier, Lower(g_addGlobal)) ? g_addGlobal : std::string();

        if (hit.category == AddGuns)
            g_addGun = hit.definition;
        else if (hit.category == AddKnives)
            g_addKnife = hit.definition;
        else if (hit.category == AddGloves)
            g_addGlove = hit.definition;
    }

    void RenderGlobalResults()
    {
        BuildGlobalIndex();
        UpdateGlobalResults();

        if (g_addGlobalCategory >= 0 && g_globalResults.counts[g_addGlobalCategory] == 0)
        {
            g_addGlobalCategory = -1;   // nothing of that kind for this search: all of them
            UpdateGlobalResults();
        }

        int total = 0;
        for (const int count : g_globalResults.counts)
            total += count;

        if (total == 0)
        {
            UI::Note("Nothing found in any category.");
            return;
        }

        // the kinds it found: a chip each with how many (click: only those)
        {
            const float available = ImGui::GetContentRegionAvail().x;
            const float spacing = S(6.0f);
            float used = 0.0f;
            bool first = true;

            const auto chip = [&](int category, int count)
            {
                char label[64];
                snprintf(label, sizeof(label), "%s  %d##global_chip_%d", tr(category < 0 ? "All" : ADD_CATEGORY_NAMES[category]), count, category);
                const float width = ImGui::CalcTextSize(label, nullptr, true).x + S(20.0f);

                if (!first && used + spacing + width <= available)
                    ImGui::SameLine(0.0f, spacing);
                else if (!first)
                    used = 0.0f;

                used += (used > 0.0f ? spacing : 0.0f) + width;
                first = false;

                if (UI::Button(label, ImVec2(width, S(26.0f)), g_addGlobalCategory == category ? UI::Kind::Primary : UI::Kind::Secondary))
                    g_addGlobalCategory = category;
            };

            chip(-1, total);

            for (int category = 0; category < AddCount; ++category)
                if (g_globalResults.counts[category] > 0)
                    chip(category, g_globalResults.counts[category]);
        }

        const std::vector<int>& hits = g_globalResults.hits;

        ClippedGrid("##add_global_grid", static_cast<int>(hits.size()), [&](int index, float width)
        {
            const GlobalHit& hit = g_globalIndex[static_cast<std::size_t>(hits[static_cast<std::size_t>(index)])];
            char id[64];
            snprintf(id, sizeof(id), "add_global_%d_%u_%d", hit.category, static_cast<unsigned>(hit.definition), hit.kit);

            const bool selected = g_addCategory == hit.category && g_addChoice.definition == hit.definition && (g_addChoice.kit == hit.kit ||
                (hit.definition == Unboxer::DEF_CHARM && Cases::HighlightEvent(g_addChoice.kit) == hit.kit));
            const std::string name = hit.category == AddPets ? std::string(tr(hit.name.c_str())) : hit.name;

            if (UI::Card(id, width, name, tr(CategoryWord(hit.category)), TierColor(hit.tier), selected, [&] { return DropImage(hit.definition, hit.kit); }))
                PickGlobal(hit);
        });
    }

    // On the right: the picked medal / coin's grades, as pictures (like the graffiti colours).
    void RenderGradePicker()
    {
        const CollectibleGroup* group = CollectibleGroupOf(g_addChoice.definition);

        if (!group || group->grades.size() < 2)
            return;

        UI::SectionTitle("Grade");

        const float size = S(48.0f);
        const float spacing = S(6.0f);
        const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (size + spacing)));
        ImDrawList* draw = ImGui::GetWindowDrawList();

        for (std::size_t i = 0; i < group->grades.size(); ++i)
        {
            const Cases::Kit* kit = group->grades[i];

            if (i % static_cast<std::size_t>(perRow))
                ImGui::SameLine(0.0f, spacing);

            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::PushID(kit->id);
            const bool clicked = ImGui::InvisibleButton("##grade", ImVec2(size, size));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const ImVec2 max(min.x + size, min.y + size);
            const bool current = kit->id == g_addChoice.definition;
            draw->AddRectFilled(min, max, hovered ? UI::Col::Hover : UI::Col::Raised, S(8.0f));
            UI::DrawImage(draw, GameOrExtraImage(kit->image), ImVec2(min.x + S(4.0f), min.y + S(4.0f)), ImVec2(max.x - S(4.0f), max.y - S(4.0f)));

            if (current)
                draw->AddRect(min, max, UI::Col::Accent, S(8.0f), 0, S(2.0f));

            if (hovered)
                ImGui::SetTooltip("%s", kit->displayName.c_str());

            if (clicked)
                g_addChoice = { static_cast<std::uint16_t>(kit->id), 0, kit->tier };
        }
    }

    void RenderAddOptions()
    {
        if (!UI::BeginPanel("##add_options", ImVec2(0.0f, ImGui::GetContentRegionAvail().y), "Add to inventory", true))
        {
            UI::EndPanel();
            return;
        }

        if (g_addChoice.definition == 0)
        {
            UI::Note("Pick an item on the left: any skin, knife, gloves, agent, sticker, charm, music kit, pin or pet.");
            UI::Hint("It goes into your CS2 inventory like a real one: equip it, put stickers on it, trade it up. Only on this PC.");
            KeepAddItemsPreviewLocal();
            UI::EndPanel();
            return;
        }

        Unboxer::Item preview = BuildAddItem();
        const bool gold = g_addCategory == AddKnives || g_addCategory == AddGloves;
        const int tier = gold ? static_cast<int>(Cases::TierGold) : g_addChoice.tier;

        const std::optional<ModelPreview::Request> previewRequest = AddPreviewRequest(preview);
        RenderAddPicture(preview, tier, previewRequest);

        ImGui::PushFont(UI::FontSemibold);
        ImGui::PushStyleColor(ImGuiCol_Text, TierColor(tier));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(Unboxer::DisplayName(preview).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();

        if (IsSkinCategory(g_addCategory))
        {
            UI::SectionTitle("Skin");
            RenderWearControls();
        }

        const bool canStatTrak = g_addCategory == AddGuns || g_addCategory == AddKnives || g_addCategory == AddMusic;
        if (canStatTrak)
        {
            UI::Toggle("StatTrak##add_st", &g_addStatTrak);

            if (g_addStatTrak && g_addCategory != AddMusic)
            {
                UI::Toggle("Count my real kills##add_real_kills", &g_addRealKills);
                if (!g_addRealKills)
                    UI::Stepper("##add_kills", "Kills", &g_addKills, 0, 999999);
            }
        }

        if (g_addCategory == AddGuns)
        {
            ImGui::BeginDisabled(g_addStatTrak);
            UI::Toggle("Souvenir##add_souvenir", &g_addSouvenir, !g_addStatTrak);
            ImGui::EndDisabled();
        }

        if (g_addChoice.definition == Unboxer::DEF_PET)
        {
            UI::SectionTitle("Chicken");
            RenderPetOptions();
        }

        if (g_addChoice.definition == Unboxer::DEF_CHARM && g_addChoice.kit == Unboxer::KEYCHAIN_STICKER_SLAB)
        {
            UI::SectionTitle("Sticker Slab");
            RenderSlabStickerPicker(false);
        }

        // a Major's highlight charm: which clip (inspecting it in CS2 plays it)
        if (g_addChoice.definition == Unboxer::DEF_CHARM)
        {
            if (const int event = Cases::HighlightEvent(g_addChoice.kit); event && !Cases::HighlightsOf(event).empty())
            {
                UI::SectionTitle("Highlight");
                RenderClipPicker(event);
            }
        }

        if (preview.tint > 0)
        {
            UI::SectionTitle("Graffiti");
            RenderTintPicker();
        }

        if (g_addCategory == AddCollectibles)
            RenderGradePicker();

        if (IsSkinCategory(g_addCategory) || g_addChoice.definition == Unboxer::DEF_PET)
        {
            UI::SectionTitle("Extras");
            RenderNameTag();
        }

        if (g_addChoice.definition == Unboxer::DEF_CASKET)
        {
            UI::SectionTitle("Storage Unit");
            RenderNameTag();
            UI::Hint("With a name it comes ready to fill. Without one, name it in CS2 first (right-click -> Name).");
        }

        if (g_addCategory == AddGuns)
        {
            RenderCharmPicker();
            UI::Hint("Stickers: put them on in CS2 with the sticker tool, like on a real gun.");
        }

        UI::Gap(6.0f);
        UI::Stepper("##add_count", "How many", &g_addItemCount, 1, Unboxer::MAX_ADD_COUNT);

        const bool ready = Unboxer::GetStatus().inventoryReady;
        char label[96];
        snprintf(label, sizeof(label), "%s##add_go", tr("Add to inventory"));

        if (UI::Button(label, ImVec2(-1.0f, S(42.0f)), UI::Kind::Primary, ready))
        {
            const bool skin = IsSkinCategory(g_addCategory);
            Unboxer::AddItem(preview, g_addItemCount, g_addRandomFloat && skin ? AddRandomFloat() : Unboxer::RandomFloat{}, g_addRandomSeed && skin);

            char text[256];
            snprintf(text, sizeof(text), "%s: %s x%d", tr("Added"), Unboxer::DisplayName(preview).c_str(), g_addItemCount);
            Notify::Push(text);
        }

        if (!ready)
            UI::Note("Your CS2 inventory isn't loaded yet.");

        // the 3D preview follows what the controls above ended up as (Chadware: submit after the controls)
        if (const std::optional<ModelPreview::Request> finalRequest = AddPreviewRequest(BuildAddItem()))
        {
            ModelPreviewWidget::UpdateRequest(*finalRequest);
            g_addPreviewActive = true;
        }
        else if (g_addPreviewActive)
        {
            ModelPreview::Close();   // a sticker / charm / pet...: no 3D preview
            g_addPreviewActive = false;
        }

        UI::EndPanel();
    }
}

namespace
{
    const char* WearName(float wear)
    {
        return wear < 0.07f ? "Factory New" : wear < 0.15f ? "Minimal Wear" : wear < 0.38f ? "Field-Tested" : wear < 0.45f ? "Well-Worn" : "Battle-Scarred";
    }

    void DetailRow(const char* label, const std::string& value)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::TextDim);
        ImGui::TextUnformatted(tr(label));
        ImGui::PopStyleColor();
        ImGui::SameLine(S(96.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    }

    // One colour picker row (RGB; the game's Color is bytes R, G, B, A). True = changed.
    bool PaintColorRow(const char* label, const char* id, std::uint32_t& color)
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr(label));

        float rgb[3] = {
            static_cast<float>(color & 0xFF) / 255.0f,
            static_cast<float>((color >> 8) & 0xFF) / 255.0f,
            static_cast<float>((color >> 16) & 0xFF) / 255.0f,
        };

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (!ImGui::ColorEdit3(id, rgb, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_Uint8))
            return false;

        const auto channel = [](float value) { return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
        color = channel(rgb[0]) | (channel(rgb[1]) << 8) | (channel(rgb[2]) << 16) | 0xFF000000u;
        return true;
    }

    // Inventory -> a gun / knife / gloves -> Custom colour (from Nerv): the skin's palette in colours you pick - in CS2's
    // own screens, the preview above and your matches (put on again a moment after the last change).
    void RenderPaintEditor(const Unboxer::Item& item)
    {
        UI::SectionTitle("Custom colour");

        CustomColor::Paint paint = item.paint;
        bool changed = UI::Toggle("Recolour this skin##paint_on", &paint.on);

        if (paint.on)
        {
            static const char* const modes[] = { "Solid", "Tint", "Dual", "Gradient" };
            changed |= UI::Segmented("##paint_mode", modes, &paint.mode);

            const bool second = paint.mode == CustomColor::MODE_TWO_TONE || paint.mode == CustomColor::MODE_GRADIENT;
            changed |= PaintColorRow(paint.mode == CustomColor::MODE_GRADIENT ? "Gradient start" : second ? "Base colour" : "Colour",
                "##paint_primary", paint.primary);

            if (second)
                changed |= PaintColorRow(paint.mode == CustomColor::MODE_GRADIENT ? "Gradient end" : "Accent colour", "##paint_secondary",
                    paint.secondary);

            static const char* const hints[] = {
                "Solid: one colour over the whole skin.",
                "Tint: only the base colour - the skin's pattern keeps its own colours.",
                "Dual: a base colour plus an accent colour on the pattern.",
                "Gradient: fades from the first colour into the second across the skin.",
            };
            UI::Note(hints[std::clamp(paint.mode, 0, CustomColor::MODE_COUNT - 1)]);
            UI::Note("Finishes that don't use a colour palette hardly change.");
        }

        if (changed)
            Unboxer::SetPaint(item.id, paint);
    }

    void RenderInventoryPanel(const Unboxer::Item* item, const std::vector<Unboxer::Item>& items)
    {
        // scrolls: a knife with its custom colours open is taller than the panel (the 3D preview keeps the wheel for its
        // zoom while the mouse is on it)
        if (!UI::BeginPanel("##items_detail", ImVec2(0.0f, ImGui::GetContentRegionAvail().y), nullptr, true))
        {
            UI::EndPanel();
            return;
        }

        if (!item)
        {
            UI::Note("Click an item to see it here: the gun with its stickers and charm, its wear and pattern.");
            KeepAddItemsPreviewLocal();
            UI::EndPanel();
            return;
        }

        const std::optional<ModelPreview::Request> request = item->kind == Unboxer::Kind::Drop ? AddPreviewRequest(*item) : std::nullopt;
        const int tier = ItemTier(*item);

        RenderAddPicture(*item, tier, request);

        if (request)
        {
            ModelPreviewWidget::UpdateRequest(*request);
            g_addPreviewActive = true;
        }
        else
        {
            KeepAddItemsPreviewLocal();
        }

        ImGui::PushFont(UI::FontSemibold);
        ImGui::PushStyleColor(ImGuiCol_Text, ItemColor(*item));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(Unboxer::DisplayName(*item).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();

        // custom colours first, right under the name (the panel scrolls to the rest)
        if (Unboxer::CanPaint(*item))
            RenderPaintEditor(*item);

        // what it's worth: the market's lowest listing for exactly this (its wear, StatTrak, Souvenir), in your currency
        {
            bool estimated = false;
            const double price = ItemPrice(*item, estimated);
            DetailRow("Price", (estimated ? "~" : "") + Money(price));

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr(estimated ? "Not sold on the market exactly like this - an estimate (the nearest real item, else its rarity)." :
                    "The lowest listing for exactly this item (its wear, StatTrak, Souvenir): CSFloat, else market.csgo.com."));
        }

        const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(item->definition);
        const bool skin = info && (info->isWeapon || info->isKnife || info->isGlove) && item->kind == Unboxer::Kind::Drop;
        char text[160];

        if (skin && item->paintKit > 0)
        {
            snprintf(text, sizeof(text), "%s (%.7f)", tr(WearName(item->wear)), item->wear);
            DetailRow("Wear", text);
            DetailRow("Pattern", std::to_string(item->seed));
        }

        if (item->statTrak)
            DetailRow("StatTrak", item->kills < 0 ? std::to_string(Unboxer::ShownKills(*item)) + " (" + tr("your real kills") + ")" : std::to_string(item->kills));

        if (!item->customName.empty())
            DetailRow("Name tag", "\"" + item->customName + "\"");

        if (item->tint > 0)
            DetailRow("Colour", Cases::TintName(item->tint));

        if (item->definition == Cases::DEF_SPRAY_PAINT)
            DetailRow("Charges", std::to_string(item->sprays > 0 ? item->sprays : Unboxer::SPRAY_CHARGES) + " / " +
                std::to_string(Unboxer::SPRAY_CHARGES));

        if (item->kind == Unboxer::Kind::Drop && item->fromCase)
            if (const Cases::Case* from = Cases::Find(item->fromCase))
                DetailRow("From", from->displayName);

        // in a Storage Unit: which one (take it out in CS2: right-click the unit -> Retrieve)
        if (item->casket != 0)
        {
            std::string unitName = tr("Storage Unit");

            for (const Unboxer::Item& unit : items)
                if (unit.id == item->casket && !unit.customName.empty())
                    unitName = "\"" + unit.customName + "\"";

            DetailRow("Stored in", unitName);
        }

        // a Storage Unit: what's in it - click one to see it here (its own list, as tall as its rows, then it scrolls)
        if (item->definition == Unboxer::DEF_CASKET)
        {
            char title[64];
            snprintf(title, sizeof(title), "%s (%d)", tr("Inside"), item->casketItems);
            UI::SectionTitle(title);

            const int stored = static_cast<int>(std::count_if(items.begin(), items.end(), [&](const Unboxer::Item& other) { return other.casket == item->id; }));

            if (stored == 0)
            {
                UI::Note("Nothing in it yet - put Lootline items in with CS2's Storage Unit, like a real one.");
            }
            else
            {
                std::uint64_t open = 0;
                const float rows = static_cast<float>(stored) * ImGui::GetTextLineHeightWithSpacing() + S(4.0f);

                if (ImGui::BeginChild("##stored_list", ImVec2(0.0f, std::min(rows, S(260.0f))), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
                {
                    for (const Unboxer::Item& inside : items)
                    {
                        if (inside.casket != item->id)
                            continue;

                        const std::string name = inside.customName.empty() ? Unboxer::DisplayName(inside) :
                            "\"" + inside.customName + "\"  " + Unboxer::DisplayName(inside);
                        char rowId[48];
                        snprintf(rowId, sizeof(rowId), "##stored_%llu", static_cast<unsigned long long>(inside.id));

                        ImGui::PushStyleColor(ImGuiCol_Text, ItemColor(inside));
                        if (ImGui::Selectable((name + rowId).c_str(), false))
                            open = inside.id;
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndChild();

                if (open != 0)
                    g_itemsFocus = open;
            }
        }

        // its buttons (the same as its right-click menu)
        UI::Gap(6.0f);
        const bool equippable = Unboxer::CanEquip(*item);
        const bool single = Cases::KindOf(item->definition) == Cases::DropKind::MusicKit || Cases::KindOf(item->definition) == Cases::DropKind::Pin ||
            item->definition == Unboxer::DEF_PET ||
            item->definition == Cases::DEF_SPRAY_PAINT || item->definition == Unboxer::DEF_C4 || (info && info->isAgent);
        const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

        if (equippable && single)
        {
            if (UI::Button(item->equipped ? "Take off##inv_off" : "Equip##inv_equip", ImVec2(-1.0f, S(34.0f)), item->equipped ? UI::Kind::Secondary : UI::Kind::Primary))
                Unboxer::Equip(item->id, item->equipped ? 0 : Unboxer::EQUIP_BOTH);
        }
        else if (equippable)
        {
            if (UI::Button("Equip on T##inv_t", ImVec2(half, S(34.0f)), item->equipped == Unboxer::EQUIP_T ? UI::Kind::Primary : UI::Kind::Secondary))
                Unboxer::Equip(item->id, Unboxer::EQUIP_T);
            ImGui::SameLine();
            if (UI::Button("Equip on CT##inv_ct", ImVec2(-1.0f, S(34.0f)), item->equipped == Unboxer::EQUIP_CT ? UI::Kind::Primary : UI::Kind::Secondary))
                Unboxer::Equip(item->id, Unboxer::EQUIP_CT);

            if (UI::Button("Both sides##inv_both", ImVec2(half, S(34.0f)), item->equipped == Unboxer::EQUIP_BOTH ? UI::Kind::Primary : UI::Kind::Secondary))
                Unboxer::Equip(item->id, Unboxer::EQUIP_BOTH);
            ImGui::SameLine();
            if (UI::Button("Take off##inv_takeoff", ImVec2(-1.0f, S(34.0f)), UI::Kind::Secondary, item->equipped != 0))
                Unboxer::Equip(item->id, 0);
        }

        if (UI::Button("Remove from inventory##inv_remove", ImVec2(-1.0f, 0.0f), UI::Kind::Ghost))
        {
            Unboxer::Remove(item->id);
            std::erase(g_itemsSelected, item->id);
            g_itemsFocus = 0;
        }

        UI::EndPanel();
    }
}

void KeepAddItemsPreview(bool onScreen)
{
    if (!onScreen && g_addPreviewActive)
    {
        ModelPreview::Close();
        g_addPreviewActive = false;
    }
}

void RenderAddItemsPage()
{
    if (!Cases::Ready())
    {
        UI::Note("Reading the game's items...");
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float side = std::clamp(available.x * 0.36f, S(320.0f), S(460.0f));
    const float list = available.x - side - ImGui::GetStyle().ItemSpacing.x;

    if (ImGui::BeginChild("##add_left", ImVec2(list, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
    {
        if (UI::BeginPanel("##add_list", ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
            RenderAddList();
        UI::EndPanel();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("##add_right", ImVec2(0.0f, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        RenderAddOptions();
    ImGui::EndChild();
}

namespace
{
    std::chrono::steady_clock::time_point g_gamePricesNext{};   // PublishGamePrices: the next time it's worked out
    std::string g_gamePricesLast;                               // ... and what it handed over last (changes only)
}

void PublishGamePrices()
{
    // each on its own: the inspect popup's price (cs2Prices), the inventory's total (cs2PriceTotal)
    const bool itemPrices = Config::menu.cs2Prices;
    const bool totalPrice = Config::menu.cs2PriceTotal;

    if (!itemPrices && !totalPrice)
        return;

    const auto now = std::chrono::steady_clock::now();

    if (now < g_gamePricesNext)
        return;

    g_gamePricesNext = now + std::chrono::seconds(2);

    // every Lootline item with a price (the menu's: the market's, else the nearest real item's, else the estimate "~")
    Unboxer::GamePrices out;
    double total = 0.0;

    for (const Unboxer::Item& item : Unboxer::Items())
    {
        bool estimated = false;
        const double price = ItemPrice(item, estimated);

        if (price <= 0.0)
            continue;

        total += price;

        if (itemPrices)
            out.items.emplace_back(item.id, (estimated ? "~" : "") + Money(price));
    }

    out.total = totalPrice && total > 0.0 ? Money(total) : std::string();   // "" = the inventory shows none
    out.itemTitle = tr("Lootline price: ");
    out.totalTitle = tr("Lootline items: ");

    std::string key = out.total + '\n' + out.itemTitle + '\n' + out.totalTitle;

    for (const auto& [id, text] : out.items)
    {
        key += '\n';
        key += std::to_string(id);
        key += text;
    }

    if (key == g_gamePricesLast)
        return;

    g_gamePricesLast = std::move(key);
    Unboxer::SetGamePrices(std::move(out));
}
