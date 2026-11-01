#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "config.h"
#include "../../../external/json/json.hpp"

namespace internal_config
{
    class ConfigManager
    {
    public:
        using json = nlohmann::json;

        // UTF-8 text (ImGui input, config names) <-> file paths. std::filesystem::path(std::string) reads the
        // string in the Windows ANSI code page, so a config named in Russian got a garbled file name.
        static std::filesystem::path Utf8Path(const std::string& text)
        {
            return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
        }

        static std::string PathUtf8(const std::filesystem::path& path)
        {
            const std::u8string text = path.u8string();
            return std::string(reinterpret_cast<const char*>(text.data()), text.size());
        }

        static std::filesystem::path GetConfigDirectory()
        {
            // wide: a user folder with non-English letters doesn't survive the ANSI code page
            wchar_t* userProfile = nullptr;
            size_t len = 0;
            const errno_t err = _wdupenv_s(&userProfile, &len, L"USERPROFILE");

            std::filesystem::path folder;

            if (err != 0 || !userProfile || len == 0)
            {
                folder = ".lootline";
            }
            else
            {
                folder = userProfile;

                // renamed from Unboxer: its folder (items, stats, settings, prices) moves over once
                std::error_code moveError;
                const std::filesystem::path previous = folder / ".unboxer";
                folder /= ".lootline";

                if (!std::filesystem::exists(folder, moveError) && std::filesystem::exists(previous, moveError))
                    std::filesystem::rename(previous, folder, moveError);
            }

            free(userProfile);

            folder /= "configs";

            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            return folder;
        }

        static bool Save(const std::string& configName)
        {
            if (configName.empty())
                return false;

            json root;
            root["skins"] = Serialize(Config::skins);
            root["ui"] = json{ { "theme", Config::ui.themePreset } };

            std::ofstream file(GetConfigPath(configName));
            if (!file)
                return false;

            file << root.dump(4, ' ', false, json::error_handler_t::replace);
            return true;
        }

        static bool Load(const std::string& configName)
        {
            std::ifstream file(GetConfigPath(configName));
            if (!file)
                return false;

            // Read into copies and only commit a config that loaded completely. json::value() throws when a key
            // holds the wrong type (hand-edited file, other version) — uncaught, that closed the game.
            try
            {
                json root;
                file >> root;

                Config::skins_t skins = Config::skins;
                Config::ui_t ui = Config::ui;

                if (const auto found = root.find("skins"); found != root.end())
                {
                    if (found->is_object() && found->contains("config"))
                        Deserialize((*found)["config"], skins);
                    else
                        Deserialize(*found, skins);
                }
                else if (const auto legacy = root.find("skins.config"); legacy != root.end())
                {
                    Deserialize(*legacy, skins);
                }

                if (const auto found = root.find("ui"); found != root.end() && found->is_object())
                    ui.themePreset = found->value("theme", ui.themePreset);
                else if (const auto legacyTheme = root.find("ui.theme"); legacyTheme != root.end() && legacyTheme->is_string())
                    ui.themePreset = legacyTheme->get<std::string>();

                Config::skins = std::move(skins);
                Config::ui = std::move(ui);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        // ── Menu / interface settings (global, separate from skin configs) ──────────────────────────
        static std::filesystem::path GetMenuSettingsPath()
        {
            return GetConfigDirectory().parent_path() / "menu.json";
        }

        static bool SaveMenuSettings()
        {
            const Config::menu_t& m = Config::menu;
            json root = {
                { "language", m.language },
                { "scale_mode", m.scaleMode },
                { "ui_scale", m.uiScale },
                { "font_scale", m.fontScale },
                { "width", m.width },
                { "height", m.height },
                { "animations", m.animations },
                { "anim_speed", m.animSpeed },
                { "menu_key", m.menuKey },
                { "stretch_mode", m.stretchMode },
                { "random_guns", m.randomGuns },
                { "random_stickers", m.randomStickers },
                { "random_charms", m.randomCharms },
                { "random_knife", m.randomKnife },
                { "random_gloves", m.randomGloves },
                { "random_agent", m.randomAgent },
                { "random_music_kit", m.randomMusicKit },
                { "random_every_round", m.randomEveryRound },
                { "auto_load_config", m.autoLoadConfig },
                { "notifications", m.notifications },
                { "skin_sort", m.skinSort },
                { "viewmodel_enabled", m.viewmodelEnabled },
                { "viewmodel_fov", m.viewmodelFov },
                { "viewmodel_x", m.viewmodelX },
                { "viewmodel_y", m.viewmodelY },
                { "viewmodel_z", m.viewmodelZ },
                { "bhop", m.bhop },
                { "fov_changer", m.fovChanger },
                { "bhop_key", m.bhopKey },
                { "fov_key", m.fovKey },
                { "reapply_key", m.reapplyKey },
                { "fov", m.fov },
                { "speed_meter", m.speedHud },
                { "sniper_crosshair", m.sniperCrosshair },
                { "show_tips", m.showTips },
                { "reapply_on_spawn", m.reapplyOnSpawn },
                { "shuffle_every_round", m.shuffleEveryRound },
                { "local_spray", m.localSpray },
                { "scoreboard_pin", m.scoreboardPin },
                { "armory_lootline", m.armoryLootline },
                { "accent_color", m.accentColor },
                { "accent_rgb", m.accentRgb },
                { "currency", m.currency },
                { "cs2_prices", m.cs2Prices },
                { "cs2_price_total", m.cs2PriceTotal },
                { "buy_keys_lootline", m.buyKeysLootline },
                { "chat_drops", m.chatDrops },
            };

            json recent = json::array();
            for (const Config::recent_skin_t& entry : m.recentSkins)
                recent.push_back({ { "definition_index", entry.definitionIndex }, { "paint_kit", entry.paintKit } });

            root["recent_skins"] = recent;
            root["reapply_times"] = m.reapplyTimes;
            root["card_pictures"] = m.cardPicture;
            root["card_text"] = m.cardText;

            std::ofstream file(GetMenuSettingsPath(), std::ios::trunc);
            if (!file)
                return false;

            file << root.dump(4, ' ', false, json::error_handler_t::replace);
            return true;
        }

        static bool LoadMenuSettings()
        {
            std::ifstream file(GetMenuSettingsPath());
            if (!file)
                return false;

            json root;
            try
            {
                file >> root;
            }
            catch (...)
            {
                return false;
            }

            if (!root.is_object())
                return false;

            // A value of the wrong type throws: keep the settings as they were then (read into a copy).
            try
            {
                Config::menu_t loaded = Config::menu;
                ReadMenuSettings(root, loaded);
                Config::menu = std::move(loaded);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        static void ReadMenuSettings(const json& root, Config::menu_t& m)
        {
            const Config::menu_t d{};
            m.language    = std::clamp(root.value("language", d.language), 0, 1);
            m.scaleMode   = std::clamp(root.value("scale_mode", d.scaleMode), 0, 1);
            m.uiScale     = std::clamp(root.value("ui_scale", d.uiScale), 0.5f, 2.0f);
            m.fontScale   = std::clamp(root.value("font_scale", d.fontScale), 0.7f, 1.5f);
            m.width       = std::clamp(root.value("width", d.width), 640.0f, 1800.0f);
            m.height      = std::clamp(root.value("height", d.height), 380.0f, 1200.0f);
            m.animations  = root.value("animations", d.animations);
            m.animSpeed   = std::clamp(root.value("anim_speed", d.animSpeed), 0.05f, 1.0f);
            m.menuKey     = std::clamp(root.value("menu_key", d.menuKey), 1, 0xFE);
            m.stretchMode = std::clamp(root.value("stretch_mode", d.stretchMode), 0, 2);
            m.randomGuns       = root.value("random_guns", d.randomGuns);
            m.randomStickers   = root.value("random_stickers", d.randomStickers);
            m.randomCharms     = root.value("random_charms", d.randomCharms);
            m.randomKnife      = root.value("random_knife", d.randomKnife);
            m.randomGloves     = root.value("random_gloves", d.randomGloves);
            m.randomAgent      = root.value("random_agent", d.randomAgent);
            m.randomMusicKit   = root.value("random_music_kit", d.randomMusicKit);
            m.randomEveryRound = root.value("random_every_round", d.randomEveryRound);
            m.autoLoadConfig   = root.value("auto_load_config", d.autoLoadConfig);
            m.notifications    = root.value("notifications", d.notifications);
            m.skinSort         = std::clamp(root.value("skin_sort", d.skinSort), 0, 2);
            m.viewmodelEnabled = root.value("viewmodel_enabled", d.viewmodelEnabled);
            m.viewmodelFov     = std::clamp(root.value("viewmodel_fov", d.viewmodelFov), 40.0f, 130.0f);
            m.viewmodelX       = std::clamp(root.value("viewmodel_x", d.viewmodelX), -10.0f, 10.0f);
            m.viewmodelY       = std::clamp(root.value("viewmodel_y", d.viewmodelY), -10.0f, 10.0f);
            m.viewmodelZ       = std::clamp(root.value("viewmodel_z", d.viewmodelZ), -10.0f, 10.0f);
            m.bhop             = root.value("bhop", d.bhop);
            m.fovChanger       = root.value("fov_changer", d.fovChanger);
            m.bhopKey          = std::clamp(root.value("bhop_key", d.bhopKey), 0, 255);
            m.fovKey           = std::clamp(root.value("fov_key", d.fovKey), 0, 255);
            m.reapplyKey       = std::clamp(root.value("reapply_key", d.reapplyKey), 0, 255);
            m.fov              = std::clamp(root.value("fov", d.fov), 60.0f, 150.0f);
            m.speedHud         = root.value("speed_meter", d.speedHud);   // new key: Lootline's old menu.json had "speed_hud" on
            m.sniperCrosshair  = root.value("sniper_crosshair", d.sniperCrosshair);
            m.showTips         = root.value("show_tips", d.showTips);
            m.reapplyOnSpawn   = root.value("reapply_on_spawn", d.reapplyOnSpawn);
            m.shuffleEveryRound = root.value("shuffle_every_round", d.shuffleEveryRound);
            m.localSpray       = root.value("local_spray", d.localSpray);
            m.scoreboardPin    = root.value("scoreboard_pin", d.scoreboardPin);
            m.armoryLootline   = root.value("armory_lootline", d.armoryLootline);
            m.accentColor      = root.value("accent_color", d.accentColor) & 0xFFFFFF;
            m.accentRgb        = root.value("accent_rgb", d.accentRgb);
            m.currency         = std::max(0, root.value("currency", d.currency));
            m.cs2Prices        = root.value("cs2_prices", d.cs2Prices);
            m.cs2PriceTotal    = root.value("cs2_price_total", d.cs2PriceTotal);
            m.buyKeysLootline  = root.value("buy_keys_lootline", d.buyKeysLootline);
            m.chatDrops        = root.value("chat_drops", d.chatDrops);

            // the reapply times after a spawn: 0 - 60 s each, at most 8 (none = the switch alone does nothing)
            if (const auto times = root.find("reapply_times"); times != root.end() && times->is_array())
            {
                m.reapplyTimes.clear();

                for (const auto& time : *times)
                    if (time.is_number() && m.reapplyTimes.size() < 8)
                        m.reapplyTimes.push_back(std::clamp(time.get<float>(), 0.0f, 60.0f));
            }

            // the cards' picture / text size per tab (0.6 - 2)
            const auto readSizes = [&](const char* key, std::array<float, 4>& sizes)
            {
                if (const auto values = root.find(key); values != root.end() && values->is_array())
                    for (std::size_t i = 0; i < sizes.size() && i < values->size(); ++i)
                        if ((*values)[i].is_number())
                            sizes[i] = std::clamp((*values)[i].get<float>(), 0.6f, 2.0f);
            };

            readSizes("card_pictures", m.cardPicture);
            readSizes("card_text", m.cardText);

            m.recentSkins.clear();
            if (const auto recent = root.find("recent_skins"); recent != root.end() && recent->is_array())
            {
                for (const auto& entry : *recent)
                {
                    if (!entry.is_object() || m.recentSkins.size() >= 6)
                        continue;

                    Config::recent_skin_t item;
                    item.definitionIndex = entry.value("definition_index", static_cast<std::uint16_t>(0));
                    item.paintKit = entry.value("paint_kit", 0);

                    if (item.definitionIndex != 0 && item.paintKit > 0)
                        m.recentSkins.push_back(item);
                }
            }
        }

        // ── Share codes: the whole skin setup as one line of text ("CHAD1-" + base64 JSON) ──────────────
        static std::string ExportShareCode()
        {
            return std::string(SHARE_CODE_PREFIX) + Base64Encode(Serialize(Config::skins).dump(-1, ' ', false, json::error_handler_t::replace));
        }

        // Replaces the current skins with the ones in the code. False if it isn't a valid Chadware code.
        static bool ImportShareCode(std::string code)
        {
            code.erase(std::remove_if(code.begin(), code.end(), [](unsigned char c) { return std::isspace(c) != 0; }), code.end());

            if (code.rfind(SHARE_CODE_PREFIX, 0) != 0)
                return false;

            std::string text;

            if (!Base64Decode(code.substr(std::strlen(SHARE_CODE_PREFIX)), text))
                return false;

            // Someone else's code: anything can be in it. A wrong value type throws — refuse the code instead.
            try
            {
                const json root = json::parse(text);

                if (!root.is_object())
                    return false;

                Config::skins_t parsed{};
                Deserialize(root, parsed);
                Config::skins = std::move(parsed);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        static bool Delete(const std::string& configName)
        {
            std::error_code ec;
            return std::filesystem::remove(GetConfigPath(configName), ec) && !ec;
        }

        static std::vector<std::string> ListConfigs()
        {
            std::vector<std::string> result;
            const auto folder = GetConfigDirectory();

            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            {
                if (ec)
                    break;

                if (!entry.is_regular_file())
                    continue;

                const auto path = entry.path();
                if (path.extension() == ".json")
                    result.emplace_back(PathUtf8(path.stem()));   // UTF-8, like everything ImGui shows
            }

            std::sort(result.begin(), result.end());
            return result;
        }

    private:
        static constexpr const char* SHARE_CODE_PREFIX = "CHAD1-";

        static std::string Base64Encode(const std::string& input)
        {
            static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

            std::string out;
            out.reserve((input.size() + 2) / 3 * 4);

            std::uint32_t buffer = 0;
            int bits = 0;

            for (const unsigned char c : input)
            {
                buffer = (buffer << 8) | c;
                bits += 8;

                while (bits >= 6)
                {
                    bits -= 6;
                    out.push_back(table[(buffer >> bits) & 0x3F]);
                }
            }

            if (bits > 0)
                out.push_back(table[(buffer << (6 - bits)) & 0x3F]);

            while (out.size() % 4 != 0)
                out.push_back('=');

            return out;
        }

        static bool Base64Decode(const std::string& input, std::string& out)
        {
            out.clear();
            std::uint32_t buffer = 0;
            int bits = 0;

            for (const char c : input)
            {
                int value;

                if (c >= 'A' && c <= 'Z') value = c - 'A';
                else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
                else if (c >= '0' && c <= '9') value = c - '0' + 52;
                else if (c == '+') value = 62;
                else if (c == '/') value = 63;
                else if (c == '=') break;
                else return false;

                buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
                bits += 6;

                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
                }
            }

            return !out.empty();
        }

        static std::filesystem::path GetConfigPath(const std::string& name)
        {
            return GetConfigDirectory() / Utf8Path(name + ".json");
        }

        static json Serialize(const Config::skin_sticker_t& value)
        {
            return json{
                { "enabled", value.enabled },
                { "kit", value.kit },
                { "wear", value.wear },
                { "scale", value.scale },
                { "rotation", value.rotation },
                { "offset_x", value.offsetX },
                { "offset_y", value.offsetY }
            };
        }

        static void Deserialize(const json& value, Config::skin_sticker_t& target)
        {
            if (!value.is_object())
                return;

            target.enabled = value.value("enabled", target.enabled);
            target.kit = value.value("kit", target.kit);
            target.wear = value.value("wear", target.wear);
            target.scale = value.value("scale", target.scale);
            target.rotation = value.value("rotation", target.rotation);
            target.offsetX = std::clamp(value.value("offset_x", target.offsetX), -0.5f, 0.5f);
            target.offsetY = std::clamp(value.value("offset_y", target.offsetY), -0.5f, 0.5f);
        }

        static json Serialize(const Config::skin_keychain_t& value)
        {
            return json{
                { "enabled", value.enabled },
                { "id", value.id },
                { "seed", value.seed },
                { "sticker", value.sticker },
                { "offset_x", value.offsetX },
                { "offset_y", value.offsetY },
                { "offset_z", value.offsetZ }
            };
        }

        static void Deserialize(const json& value, Config::skin_keychain_t& target)
        {
            if (!value.is_object())
                return;

            target.enabled = value.value("enabled", target.enabled);
            target.id = value.value("id", target.id);
            target.seed = value.value("seed", target.seed);
            target.sticker = value.value("sticker", target.sticker);
            target.offsetX = value.value("offset_x", target.offsetX);
            target.offsetY = value.value("offset_y", target.offsetY);
            target.offsetZ = value.value("offset_z", target.offsetZ);
        }

        static json Serialize(const Config::item_skin_t& value)
        {
            json stickers = json::array();
            for (const auto& sticker : value.stickers)
                stickers.push_back(Serialize(sticker));

            return json{
                { "enabled", value.enabled },
                { "paint_kit", value.paintKit },
                { "seed", value.seed },
                { "wear", value.wear },
                { "stattrak_enabled", value.statTrakEnabled },
                { "stattrak_real", value.statTrakReal },
                { "stattrak_kills", value.statTrakKills },
                { "souvenir", value.souvenir },
                { "custom_name", value.customName },
                { "stickers", stickers },
                { "keychain", Serialize(value.keychain) }
            };
        }

        static void Deserialize(const json& value, Config::item_skin_t& target)
        {
            if (!value.is_object())
                return;

            target.enabled = value.value("enabled", target.enabled);
            target.statTrakReal = value.value("stattrak_real", target.statTrakReal);
            target.paintKit = value.value("paint_kit", target.paintKit);
            target.seed = value.value("seed", target.seed);
            target.wear = value.value("wear", target.wear);
            target.statTrakEnabled = value.value("stattrak_enabled", target.statTrakEnabled);
            target.statTrakKills = value.value("stattrak_kills", target.statTrakKills);
            target.souvenir = value.value("souvenir", target.souvenir);
            target.customName = value.value("custom_name", target.customName);

            if (const auto stickers = value.find("stickers"); stickers != value.end() && stickers->is_array())
            {
                const std::size_t count = (std::min)(stickers->size(), target.stickers.size());
                for (std::size_t i = 0; i < count; ++i)
                    Deserialize((*stickers)[i], target.stickers[i]);
            }

            if (const auto keychain = value.find("keychain"); keychain != value.end())
                Deserialize(*keychain, target.keychain);
        }

        // One side's loadout (no "enabled" / music kit - those belong to skins_t).
        static json Serialize(const Config::skin_changer_t& value)
        {
            json weapons = json::array();
            for (const auto& [definitionIndex, skin] : value.weapons)
                weapons.push_back({ { "definition_index", definitionIndex }, { "skin", Serialize(skin) } });

            json knives = json::array();
            for (const auto& [definitionIndex, skin] : value.knives)
                knives.push_back({ { "definition_index", definitionIndex }, { "skin", Serialize(skin) } });

            json gloves = json::array();
            for (const auto& [definitionIndex, skin] : value.gloves)
                gloves.push_back({ { "definition_index", definitionIndex }, { "skin", Serialize(skin) } });

            return json{
                { "weapons", weapons },
                { "knife_definition", value.knifeDefinition },
                { "knives", knives },
                { "glove_definition", value.gloveDefinition },
                { "gloves", gloves },
                { "agent_definition", value.agentDefinition }
            };
        }

        static void Deserialize(const json& value, Config::skin_changer_t& target)
        {
            if (!value.is_object())
                return;

            target.knifeDefinition = value.value("knife_definition", target.knifeDefinition);
            target.gloveDefinition = value.value("glove_definition", target.gloveDefinition);
            target.agentDefinition = value.value("agent_definition", target.agentDefinition);

            LoadSkinMap(value, "weapons", target.weapons, false);
            LoadSkinMap(value, "knives", target.knives, true);
            LoadSkinMap(value, "gloves", target.gloves, false);
        }

        // Layout: the T loadout sits at the top level (same keys as older single-loadout configs, so those
        // still load), the CT loadout under "ct".
        static json Serialize(const Config::skins_t& value)
        {
            json root = Serialize(value.loadouts[Config::TEAM_T]);
            root["enabled"] = value.enabled;
            root["separate_teams"] = true;   // older builds could share one loadout between both sides
            root["music_kit"] = value.musicKit;
            root["ct"] = Serialize(value.loadouts[Config::TEAM_CT]);
            return root;
        }

        static void Deserialize(const json& value, Config::skins_t& target)
        {
            if (!value.is_object())
                return;

            target.enabled = value.value("enabled", target.enabled);
            const bool hadSeparateTeams = value.value("separate_teams", true);
            target.musicKit = (std::max)(0, value.value("music_kit", target.musicKit));

            Config::skin_changer_t& t = target.loadouts[Config::TEAM_T];
            Config::skin_changer_t& ct = target.loadouts[Config::TEAM_CT];

            t = Config::skin_changer_t{};
            Deserialize(value, t);

            if (t.agentDefinition == 0)
            {
                const std::uint16_t legacyT = value.value("terrorist_agent_definition", static_cast<std::uint16_t>(0));
                const std::uint16_t legacyCt = value.value("counter_terrorist_agent_definition", static_cast<std::uint16_t>(0));
                t.agentDefinition = legacyT != 0 ? legacyT : legacyCt;
            }

            if (const auto ctValue = value.find("ct"); hadSeparateTeams && ctValue != value.end() && ctValue->is_object())
            {
                ct = Config::skin_changer_t{};
                Deserialize(*ctValue, ct);
            }
            else
            {
                // Older config (one loadout for both sides): CT starts as a copy of T. The single agent is kept on
                // both; the agent changer only applies it on the side it belongs to.
                const std::uint16_t ctAgent = ctValue != value.end() && ctValue->is_object()
                    ? ctValue->value("agent_definition", t.agentDefinition)
                    : t.agentDefinition;
                ct = t;
                ct.agentDefinition = ctAgent;
            }
        }

        static void LoadSkinMap(const json& root, const char* key, std::unordered_map<std::uint16_t, Config::item_skin_t>& output, bool knivesOnly)
        {
            output.clear();
            const auto list = root.find(key);
            if (list == root.end() || !list->is_array())
                return;

            for (const auto& entry : *list)
            {
                if (!entry.is_object())
                    continue;

                const std::uint16_t definitionIndex = entry.value("definition_index", static_cast<std::uint16_t>(0));
                if (definitionIndex == 0 || (knivesOnly && definitionIndex < 500))
                    continue;

                const auto skin = entry.find("skin");
                if (skin == entry.end())
                    continue;

                Config::item_skin_t parsed;
                Deserialize(*skin, parsed);
                output[definitionIndex] = std::move(parsed);
            }
        }
    };
}
