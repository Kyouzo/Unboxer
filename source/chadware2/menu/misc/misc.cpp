#include "misc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../menu.h"
#include "../ui/ui.h"
#include "../ui_scale.h"
#include "../../config/config.h"
#include "../../features/movement/movement.h"
#include "../../features/skinchanger/skinchanger.h"

namespace
{
    // translated, wrapped, in a colour (warnings)
    void Colored(ImU32 color, const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(tr(text));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    void StatRow(const char* label)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::TextDim);
        ImGui::TextUnformatted(tr(label));
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
    }

    void Dim(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Col::TextFaint);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    // Gloves gone? Another player model for a frame makes the game rebuild them (SkinChanger::RefreshPlayerModel).
    // Selling a defuse kit does this by itself; the button is for anything else that ever takes them. (From Chadware.)
    void RenderGloves(const Movement::Stats& stats)
    {
        if (UI::BeginPanel("##misc_gloves", ImVec2(0.0f, 0.0f), "Gloves"))
        {
            if (UI::Button("Refresh Gloves##refresh_gloves", ImVec2(-1.0f, 0.0f), UI::Kind::Secondary, stats.alive))
                SkinChanger::RefreshPlayerModel();

            UI::Hint("Gloves gone? This brings them back. After you sell a defuse kit it happens by itself.");
        }
        UI::EndPanel();
    }

    void RenderBhop()
    {
        if (UI::BeginPanel("##misc_bhop", ImVec2(0.0f, 0.0f), "Bunnyhop"))
        {
            if (MenuState::KeyToggle("Bunnyhop##bhop", &Config::menu.bhop, MenuState::KeyBhop))
                MenuState::MarkDirty();

            UI::Hint("Hold SPACE to hop. Only while the menu is closed.");

            if (Movement::JumpButton() == Movement::ButtonLookup::NotFound)
                Colored(UI::Col::Danger, "Couldn't find the game's jump button - a CS2 update changed it.");
        }
        UI::EndPanel();
    }

    void RenderFov(const Movement::Stats& stats)
    {
        if (UI::BeginPanel("##misc_fov", ImVec2(0.0f, 0.0f), "FOV Changer"))
        {
            if (MenuState::KeyToggle("FOV changer##fov_changer", &Config::menu.fovChanger, MenuState::KeyFov))
                MenuState::MarkDirty();

            char value[16];
            snprintf(value, sizeof(value), "%.0f", Config::menu.fov);

            ImGui::BeginDisabled(!Config::menu.fovChanger);

            if (UI::Slider("##fov_value", "FOV", &Config::menu.fov, 60.0f, 150.0f, value))
            {
                Config::menu.fov = std::round(std::clamp(Config::menu.fov, 60.0f, 150.0f));
                MenuState::MarkDirty();
            }

            if (UI::Button("Reset to 90##fov_reset", ImVec2(-1.0f, 0.0f), UI::Kind::Ghost, Config::menu.fovChanger))
            {
                Config::menu.fov = 90.0f;
                MenuState::MarkDirty();
            }

            ImGui::EndDisabled();

            if (Config::menu.fovChanger && stats.alive && stats.scoped)
                Colored(UI::Col::Warning, "Paused while you're scoped.");
            else
                UI::Hint("Your camera FOV. The game's default is 90. Pauses while you're scoped.");
        }
        UI::EndPanel();
    }

    void RenderSpeed(const Movement::Stats& stats)
    {
        if (UI::BeginPanel("##misc_speed", ImVec2(0.0f, 0.0f), "Speed Meter"))
        {
            if (UI::Toggle("Show speed on screen##speed_meter", &Config::menu.speedHud))
                MenuState::MarkDirty();

            UI::Gap(4.0f);

            // the speed, big, and the top speed on the right
            char speed[16];
            snprintf(speed, sizeof(speed), stats.alive ? "%.0f" : "--", stats.speed);

            const float lineTop = ImGui::GetCursorPosY();
            ImGui::PushFont(UI::FontHeading ? UI::FontHeading : ImGui::GetFont(), ImGui::GetStyle().FontSizeBase * 2.2f);
            ImGui::TextUnformatted(speed);
            const float bigHeight = ImGui::GetItemRectSize().y;
            ImGui::PopFont();

            ImGui::SameLine();
            ImGui::SetCursorPosY(lineTop + bigHeight - ImGui::GetTextLineHeight() - S(2.0f));
            Dim(stats.alive ? "u/s" : tr("not in game"));

            char top[32];
            snprintf(top, sizeof(top), "%s %.0f", tr("top"), stats.topSpeed);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(top).x));
            Dim(top);
            ImGui::SetCursorPosY(lineTop + bigHeight + S(4.0f));

            // bar: 0 .. 450 u/s
            const float fraction = stats.alive ? std::clamp(stats.speed / 450.0f, 0.0f, 1.0f) : 0.0f;
            const ImVec2 barMin = ImGui::GetCursorScreenPos();
            const ImVec2 barMax(barMin.x + ImGui::GetContentRegionAvail().x, barMin.y + S(4.0f));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(barMin, barMax, UI::Col::Raised, S(2.0f));
            draw->AddRectFilled(barMin, ImVec2(barMin.x + (barMax.x - barMin.x) * fraction, barMax.y), UI::Col::Accent, S(2.0f));
            ImGui::Dummy(ImVec2(barMax.x - barMin.x, S(4.0f)));
            UI::Gap(6.0f);

            if (ImGui::BeginTable("##speed_stats", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch, 1.6f);

                StatRow("Last hop");
                if (stats.haveHop)
                {
                    ImGui::Text("%.0f", stats.lastTakeoff);
                    ImGui::SameLine();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(stats.lastGain >= 0.0f ? UI::Col::Success : UI::Col::Danger), "%+.0f", stats.lastGain);
                    ImGui::SameLine();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(stats.lastPerfect ? UI::Col::Success : UI::Col::Warning), "%s",
                        stats.lastPerfect ? tr("PERFECT") : tr("late"));
                }
                else
                {
                    Dim("--");
                }

                StatRow("On ground");
                if (stats.haveHop)
                    ImGui::Text("%.0f ms", stats.lastGroundMs);
                else
                    Dim("--");

                StatRow("Hops");
                ImGui::Text("%d", stats.hops);

                StatRow("Perfect");
                ImGui::Text("%d", stats.perfect);
                ImGui::SameLine();
                char share[16];
                snprintf(share, sizeof(share), "(%d%%)", stats.hops ? stats.perfect * 100 / stats.hops : 0);
                Dim(share);

                StatRow("Streak");
                ImGui::Text("%d", stats.streak);
                ImGui::SameLine();
                char best[32];
                snprintf(best, sizeof(best), "%s %d", tr("best"), stats.bestStreak);
                Dim(best);

                ImGui::EndTable();
            }

            UI::Gap(4.0f);

            if (UI::Button("Reset stats##speed_reset", ImVec2(-1.0f, 0.0f), UI::Kind::Secondary))
                Movement::ResetStats();

            UI::Hint("PERFECT = you jumped the tick you landed and kept your speed. Counts hops you do by hand too.");
        }
        UI::EndPanel();
    }

    void RenderSniperCrosshair()
    {
        if (UI::BeginPanel("##misc_sniper_crosshair", ImVec2(0.0f, 0.0f), "Sniper Crosshair"))
        {
            if (UI::Toggle("Your crosshair on snipers##sniper_crosshair", &Config::menu.sniperCrosshair))
                MenuState::MarkDirty();

            if (!Movement::SniperCrosshairAvailable())
                Colored(UI::Col::Warning, "Not available: its signature wasn't found (Settings -> Check signatures).");
            else
                UI::Hint("The AWP, SSG 08, SCAR-20 and G3SG1 show the crosshair you set in CS2 while you're not scoped.");
        }
        UI::EndPanel();
    }
}

void RenderMiscPage()
{
    const Movement::Stats stats = Movement::GetStats();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float half = std::floor((available.x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

    if (ImGui::BeginChild("##misc_left", ImVec2(half, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
    {
        RenderBhop();
        RenderFov(stats);
        RenderGloves(stats);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("##misc_right", ImVec2(0.0f, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
    {
        RenderSpeed(stats);
        RenderSniperCrosshair();
    }
    ImGui::EndChild();
}
