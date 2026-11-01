#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include "../menu.h"
#include "../../config/config.h"
#include "../../config/configmanager.h"
#include "../../features/unboxer/local_spray.h"
#include "../../features/unboxer/prices.h"
#include "../../features/unboxer/unboxer.h"
#include "../../utils/offsets/offsets.h"
#include "../../../patterns.h"

namespace
{
    // Human-readable name of a virtual key ("Home", "F5", "Mouse 4"...).
    // Sliders that resize the menu itself only apply when you let go (otherwise the window and the slider under the
    // mouse fight each other).
    bool SizeSlider(const char* id, const char* label, float* value, float minValue, float maxValue, const char* format)
    {
        static ImGuiID editingId = 0;
        static float editingValue = 0.0f;

        const ImGuiID thisId = ImGui::GetID(id);
        float shown = editingId == thisId ? editingValue : *value;

        char text[32];
        snprintf(text, sizeof(text), format, shown);
        UI::Slider(id, label, &shown, minValue, maxValue, text);

        if (ImGui::IsItemActive())
        {
            editingId = thisId;
            editingValue = shown;
            return false;
        }

        if (editingId == thisId)
        {
            editingId = 0;

            if (shown != *value)
            {
                *value = shown;
                return true;
            }
        }

        return false;
    }

    void Colored(ImU32 color, const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    void RenderLeft()
    {
        // ── language ──
        if (UI::BeginPanel("##set_language", ImVec2(0.0f, 0.0f), "Language"))
        {
            static const char* const languages[] = { "English", "Русский" };
            int language = Config::menu.language;

            if (UI::Segmented("##language", languages, &language))
            {
                Config::menu.language = language;
                MenuState::MarkDirty();
            }
        }
        UI::EndPanel();

        // ── size ──
        if (UI::BeginPanel("##set_size", ImVec2(0.0f, 0.0f), "Size"))
        {
            static const char* const scaleModes[] = { "Follow the game", "Fixed" };
            int scaleMode = Config::menu.scaleMode;

            if (UI::Segmented("##scale_mode", scaleModes, &scaleMode))
            {
                Config::menu.scaleMode = scaleMode;
                MenuState::MarkDirty();
            }

            if (SizeSlider("##menu_scale", "Menu scale", &Config::menu.uiScale, 0.5f, 2.0f, "%.2fx"))
                MenuState::MarkDirty();
            if (SizeSlider("##text_size", "Text size", &Config::menu.fontScale, 0.7f, 1.5f, "%.2fx"))
                MenuState::MarkDirty();

            UI::Hint("Applies when you let go. You can also drag the menu's corner.");

            const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

            if (UI::Button("Reset size##reset_size", ImVec2(half, 0.0f)))
            {
                const Config::menu_t defaults{};
                Config::menu.width = defaults.width;
                Config::menu.height = defaults.height;
                Config::menu.uiScale = defaults.uiScale;
                Config::menu.fontScale = defaults.fontScale;
                MenuState::resetSizePending.store(true);
                MenuState::centerPending.store(true);
                MenuState::MarkDirty();
            }

            ImGui::SameLine();

            if (UI::Button("Center##center_menu", ImVec2(half, 0.0f)))
                MenuState::centerPending.store(true);
        }
        UI::EndPanel();

        // ── display ──
        if (UI::BeginPanel("##set_display", ImVec2(0.0f, 0.0f), "Stretched resolution"))
        {
            static const char* const stretchModes[] = { "Normal", "Crisp", "Off" };
            int stretchMode = Config::menu.stretchMode;

            if (UI::Segmented("##stretch_mode", stretchModes, &stretchMode))
            {
                Config::menu.stretchMode = stretchMode;
                MenuState::MarkDirty();
            }

            UI::Hint(stretchMode == 0 ? "Normal: the menu keeps its proportions and the mouse lines up on stretched 4:3." :
                stretchMode == 1 ? "Crisp: sharpest text, but the menu is stretched wide like the game." :
                                   "Off: no correction (may be cut off on stretched resolutions).");
        }
        UI::EndPanel();

        // ── prices (the upgrader's) ──
        if (UI::BeginPanel("##set_prices", ImVec2(0.0f, 0.0f), "Prices"))
        {
            const Prices::Info info = Prices::GetInfo();
            const Prices::UpdateStatus update = Prices::GetUpdateStatus();
            char line[200];

            if (info.state == Prices::State::Ready)
                snprintf(line, sizeof(line), tr(info.fromFile ? "%d items, your list from %s" : "%d items, built-in list from %s"), info.count, info.date.c_str());
            else
                snprintf(line, sizeof(line), "%s", tr("No price list - estimates are used"));

            ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::TextDim);
            ImGui::TextUnformatted(line);
            ImGui::PopStyleColor();

            // the currency every price is shown in (the menu, CS2's terminal offers)
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tr("Currency"));
                ImGui::SameLine(S(96.0f));
                ImGui::SetNextItemWidth(-FLT_MIN);

                const int current = std::clamp(Config::menu.currency, 0, Prices::CurrencyCount() - 1);
                char preview[96];
                snprintf(preview, sizeof(preview), "%s  -  %s  (%s)", Prices::CurrencyCode(current), tr(Prices::CurrencyName(current)), Prices::Format(1.0).c_str());

                if (ImGui::BeginCombo("##currency", preview, ImGuiComboFlags_HeightLarge))
                {
                    for (int i = 0; i < Prices::CurrencyCount(); ++i)
                    {
                        char label[96];
                        snprintf(label, sizeof(label), "%s  -  %s", Prices::CurrencyCode(i), tr(Prices::CurrencyName(i)));

                        if (ImGui::Selectable(label, i == current))
                        {
                            Config::menu.currency = i;
                            Prices::SetCurrency(i);
                            MenuState::MarkDirty();
                        }
                    }

                    ImGui::EndCombo();
                }

                UI::Hint("Every price in Lootline and on CS2's terminal offers. The rates of the price list's day are built in (Lootline never goes online).");
            }

            if (UI::Toggle("Price when you inspect an item in CS2##cs2_prices", &Config::menu.cs2Prices))
                MenuState::MarkDirty();

            if (UI::Toggle("Total at the top of CS2's inventory##cs2_price_total", &Config::menu.cs2PriceTotal))
                MenuState::MarkDirty();

            UI::Hint("A Lootline item's price under its name when you inspect it in CS2, and what all your Lootline items are worth at the top of CS2's inventory - each on its own.");

            // CS2's own "buy a key" -> Lootline keys, and what that "spent"
            if (UI::Toggle("Buy keys with Lootline##buy_keys", &Config::menu.buyKeysLootline))
                MenuState::MarkDirty();

            UI::Hint("A case without a key: CS2's Buy in its popup gives you Lootline keys instead - free, nothing goes to Steam - and Unlock comes up like after a real purchase. While it's on, every key you buy in CS2 is a Lootline key.");

            // still being finished (v1.5.2): said right under the switch
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::Warning);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(tr("Work in progress. If Steam's own payment window ever opens, close it and don't confirm anything: Lootline didn't catch that purchase."));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            {
                const Unboxer::KeySpending spent = Unboxer::GetKeySpending();
                char line[160];
                snprintf(line, sizeof(line), tr("Spent on keys: %s (%d keys)"), Prices::Format(spent.dollars).c_str(), spent.keys);

                const float resetWidth = S(90.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(line);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("Keys bought with Lootline, at Valve's store price ($2.49 a key), in your currency."));

                ImGui::SameLine(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - resetWidth));
                if (UI::Button("Reset##key_spent_reset", ImVec2(resetWidth, 0.0f), UI::Kind::Ghost, spent.keys > 0))
                    Unboxer::ResetKeySpending();
            }

            if (update.state == Prices::UpdateState::Waiting)
            {
                UI::Note("Your browser shows CSFloat's price list: press Ctrl+S and save it (Downloads). Lootline picks it up by itself.");

                if (UI::Button("Cancel##prices_cancel", ImVec2(-1.0f, 0.0f), UI::Kind::Ghost))
                    Prices::CancelUpdate();
            }
            else
            {
                if (UI::Button("Update prices##prices_update", ImVec2(-1.0f, 0.0f), UI::Kind::Secondary))
                    Prices::BeginUpdate();

                if (update.state == Prices::UpdateState::Done)
                {
                    snprintf(line, sizeof(line), "%s: %d %s", tr(update.text.c_str()), update.count, tr("items"));
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::Success), "%s", line);
                }
                else if (update.state == Prices::UpdateState::Failed)
                {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::Danger), "%s", tr(update.text.c_str()));
                }

                UI::Hint("One click: CSFloat's price list opens in your browser, you save it, Lootline takes it. Lootline itself never goes online.");
            }
        }
        UI::EndPanel();
    }

    void RenderRight()
    {
        // ── menu ──
        if (UI::BeginPanel("##set_menu", ImVec2(0.0f, 0.0f), "Menu"))
        {
            MenuState::KeyButton("menu_key", MenuState::KeyMenu);

            if (UI::Toggle("Open animation##animations", &Config::menu.animations))
                MenuState::MarkDirty();

            if (UI::Toggle("Pop-up messages##notifications", &Config::menu.notifications))
                MenuState::MarkDirty();

            if (UI::Toggle("Show tips##show_tips", &Config::menu.showTips))
                MenuState::MarkDirty();

            // the menu's colour: the selected tab, buttons, switches, sliders (RGB picker, or cycling)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI::Col::TextDim), "%s", tr("Menu colour"));
            float rgb[3] = {
                static_cast<float>((Config::menu.accentColor >> 16) & 0xFF) / 255.0f,
                static_cast<float>((Config::menu.accentColor >> 8) & 0xFF) / 255.0f,
                static_cast<float>(Config::menu.accentColor & 0xFF) / 255.0f,
            };

            ImGui::BeginDisabled(Config::menu.accentRgb);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::ColorEdit3("##accent_color", rgb, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_Uint8))
            {
                const auto channel = [](float value) { return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
                Config::menu.accentColor = (channel(rgb[0]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[2]);
                MenuState::MarkDirty();
            }
            ImGui::EndDisabled();

            if (UI::Toggle("RGB - the colours go round##accent_rgb", &Config::menu.accentRgb))
                MenuState::MarkDirty();

            if (UI::Button("Back to gold##accent_reset", ImVec2(-1.0f, 0.0f), UI::Kind::Ghost,
                Config::menu.accentColor != 0xECB43C || Config::menu.accentRgb))
            {
                Config::menu.accentColor = 0xECB43C;
                Config::menu.accentRgb = false;
                MenuState::MarkDirty();
            }

            if (UI::Button("Open data folder##open_folder", ImVec2(-1.0f, 0.0f), UI::Kind::Ghost))
            {
                const std::filesystem::path folder = internal_config::ConfigManager::GetConfigDirectory().parent_path();
                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }

            UI::Hint("Your items, settings and the log are in %USERPROFILE%\\.lootline");
        }
        UI::EndPanel();

        // ── in a match ──
        if (UI::BeginPanel("##set_match", ImVec2(0.0f, 0.0f), "In a match"))
        {
            if (UI::Toggle("Drop message in chat##chat_drops", &Config::menu.chatDrops))
                MenuState::MarkDirty();

            UI::Hint("A case you open in a match puts \"<you> has opened a container and found: <item>\" in your chat when the reel stops. Only you see it.");

            if (UI::Toggle("Reapply skins when you spawn##reapply_spawn", &Config::menu.reapplyOnSpawn))
                MenuState::MarkDirty();

            // when: one reapply per time, in seconds after spawning (typed, +/-, deleted with x; at most 8)
            if (Config::menu.reapplyOnSpawn)
            {
                std::vector<float>& times = Config::menu.reapplyTimes;
                int remove = -1;

                if (!times.empty() && ImGui::BeginTable("##reapply_times", 2, ImGuiTableFlags_None))
                {
                    ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("delete", ImGuiTableColumnFlags_WidthFixed, S(34.0f));

                    for (int i = 0; i < static_cast<int>(times.size()); ++i)
                    {
                        ImGui::PushID(i);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();

                        char label[96];
                        snprintf(label, sizeof(label), tr("Time %d: seconds after spawning"), i + 1);

                        if (UI::FloatField("##reapply_time", label, &times[static_cast<std::size_t>(i)], 0.0f, 60.0f, 0.1f, "%.1f"))
                            MenuState::MarkDirty();

                        ImGui::TableNextColumn();

                        if (UI::Button("x##reapply_time_delete", ImVec2(S(34.0f), S(34.0f)), UI::Kind::Ghost))
                            remove = i;

                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", tr("Delete this time"));

                        ImGui::PopID();
                    }

                    ImGui::EndTable();
                }

                if (remove >= 0)
                {
                    times.erase(times.begin() + remove);
                    MenuState::MarkDirty();
                }

                if (times.empty())
                    UI::Note("No times: nothing is put on again when you spawn. Add one.");

                if (UI::Button("+ Add a time##reapply_time_add", ImVec2(-1.0f, 0.0f), UI::Kind::Secondary, times.size() < 8))
                {
                    times.push_back(times.empty() ? 0.6f : std::min(60.0f, times.back() + 1.0f));
                    MenuState::MarkDirty();
                }

                UI::Hint("Each time puts every skin on again (gloves too) that many seconds after you spawn - e.g. 0.6, 2 and 5 if a skin sometimes doesn't stick. Up to 8 times; x deletes one.");
            }

            // the button, and its key right next to it
            const float chip = S(96.0f);
            if (UI::Button("Reapply skins now##reapply", ImVec2(ImGui::GetContentRegionAvail().x - chip - ImGui::GetStyle().ItemSpacing.x, 0.0f), UI::Kind::Secondary))
                Unboxer::ReapplyInMatch();

            ImGui::SameLine();
            MenuState::KeyChip("reapply_key", MenuState::KeyReapply, chip);

            UI::Hint("What you equip in a match goes on right away, gloves too. This puts everything on again if something didn't.");

            if (UI::Toggle("Shuffle every round##shuffle_round", &Config::menu.shuffleEveryRound))
                MenuState::MarkDirty();

            UI::Hint("CS2's loadout shuffle picks one item per match. On: a new pick from your shuffled items every round.");

            if (UI::Toggle("Lootline pin on the scoreboard##scoreboard_pin", &Config::menu.scoreboardPin))
                MenuState::MarkDirty();

            UI::Hint("The pin, coin or medal you equip in Lootline shows next to your name on the scoreboard. Only you see it.");

            if (UI::Toggle("Graffiti only you see##local_spray", &Config::menu.localSpray))
                MenuState::MarkDirty();

            const int sprays = LocalSpray::Count();
            char removeLabel[96];
            snprintf(removeLabel, sizeof(removeLabel), "%s (%d)##spray_clear", tr("Remove my graffiti"), sprays);
            if (UI::Button(removeLabel, ImVec2(-1.0f, 0.0f), UI::Kind::Ghost, sprays > 0))
                LocalSpray::Clear();

            UI::Hint("Equip a Lootline graffiti and spray it with CS2's spray wheel: it goes on the wall like a real one, on your screen only - nothing goes to the server. One every 45 seconds like in CS2; leaving the match takes them off.");
        }
        UI::EndPanel();

        // (the Armory's settings are in Cases -> Armory)

        // ── game update ──
        if (UI::BeginPanel("##set_update", ImVec2(0.0f, 0.0f), "Game update"))
        {
            const bool running = Offsets::SignatureCheckRunning();

            if (UI::Button(running ? "Checking...##check_sigs" : "Check signatures##check_sigs", ImVec2(-1.0f, 0.0f), UI::Kind::Secondary, !running))
                Offsets::StartSignatureCheck();

            const std::vector<Offsets::SignatureResult> results = Offsets::SignatureResults();

            if (!results.empty() && !running)
            {
                int broken = 0;

                for (const Offsets::SignatureResult& result : results)
                {
                    if (result.found || result.rescued)   // rescued: the offsets dump or a fallback covers it
                        continue;

                    const Patterns::Entry* entry = nullptr;
                    for (const Patterns::Entry& candidate : Patterns::All)
                        if (!std::strcmp(candidate.name, result.name))
                            entry = &candidate;

                    ++broken;
                    char line[160];
                    snprintf(line, sizeof(line), "%s  -  %s", result.name, entry ? tr(entry->usedFor) : "");
                    Colored(UI::Col::Danger, line);
                }

                if (broken == 0)
                {
                    char line[64];
                    snprintf(line, sizeof(line), "%s (%d)", tr("All signatures found."), static_cast<int>(results.size()));
                    Colored(UI::Col::Success, line);
                }
                else
                {
                    UI::Note("After a CS2 update: put a new signature for each red one into Lootline\\offsets\\patterns.txt and run BUILD.bat.");
                }
            }
            else
            {
                UI::Hint("If Lootline stops working after a CS2 update, this shows which part broke.");
            }
        }
        UI::EndPanel();

        // ── unload ──
        if (UI::BeginPanel("##set_unload", ImVec2(0.0f, 0.0f), "Unload"))
        {
            UI::Hint("Page Down does the same. Your items stay until CS2 restarts, but opening them needs Lootline loaded.");

            if (UI::Button("Unload Lootline##unload", ImVec2(-1.0f, 0.0f), UI::Kind::Danger))
                ImGui::OpenPopup("##confirm_unload");

            if (ImGui::BeginPopup("##confirm_unload"))
            {
                ImGui::TextUnformatted(tr("Unload Lootline?"));
                UI::Gap(4.0f);

                if (UI::Button("Yes, unload##yes_unload", ImVec2(S(130.0f), 0.0f), UI::Kind::Danger))
                {
                    MenuState::unloadRequested.store(true, std::memory_order_release);
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();

                if (UI::Button("Cancel##cancel_unload", ImVec2(S(110.0f), 0.0f)))
                    ImGui::CloseCurrentPopup();

                ImGui::EndPopup();
            }
        }
        UI::EndPanel();
    }
}

void RenderSettingsEditor()
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float half = std::floor((available.x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

    if (ImGui::BeginChild("##settings_left", ImVec2(half, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        RenderLeft();
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("##settings_right", ImVec2(0.0f, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        RenderRight();
    ImGui::EndChild();
}
