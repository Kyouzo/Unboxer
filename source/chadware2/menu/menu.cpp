#include "menu.h"

#include <cmath>

#include "unboxer/unboxer_menu.h"
#include "settings/settings.h"
#include "misc/misc.h"
#include "../features/movement/movement.h"
#include "../config/config.h"
#include "../config/configmanager.h"
#include "../features/skinchanger/stattrak_tracker.h"
#include "../features/unboxer/local_spray.h"
#include "../features/unboxer/prices.h"
#include "../features/unboxer/unboxer.h"
#include "skinchanger/images/skin_images.h"
#include "notify.h"

namespace
{
    enum Tab : int
    {
        TabCases = 0,
        TabAddItems,
        TabInventory,
        TabUpgrader,
        TabMisc,
        TabSettings,
    };
}

Menu::Menu() : activeTab(TabCases), showMenu(false) {}

int* MenuState::KeyFor(int target)
{
    switch (target)
    {
    case KeyMenu: return &Config::menu.menuKey;
    case KeyBhop: return &Config::menu.bhopKey;
    case KeyFov: return &Config::menu.fovKey;
    case KeyReapply: return &Config::menu.reapplyKey;
    default: return nullptr;
    }
}

const char* MenuState::KeyName(int vk)
{
    static char name[64];

    switch (vk)
    {
    case 0: return tr("none");
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_MBUTTON:  return "Mouse 3";
    default: break;
    }

    UINT scanCode = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scanCode << 16);

    switch (vk) // extended keys need bit 24 or GetKeyNameText returns the numpad name
    {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
    case VK_DIVIDE: case VK_NUMLOCK:
        lParam |= 1 << 24;
        break;
    default:
        break;
    }

    if (GetKeyNameTextA(lParam, name, sizeof(name)) > 0)
        return name;

    snprintf(name, sizeof(name), "Key 0x%02X", vk);
    return name;
}

void MenuState::KeyButton(const char* id, int target)
{
    const int* key = KeyFor(target);
    const bool capturing = capturingKey.load() == target;
    char label[128];
    snprintf(label, sizeof(label), "%s: %s##%s", tr(target == KeyMenu ? "Menu key" : "Key"), capturing ? tr("press a key...") :
        KeyName(key ? *key : 0), id);

    if (UI::Button(label, ImVec2(-1.0f, 0.0f), capturing ? UI::Kind::Primary : UI::Kind::Secondary))
        capturingKey.store(capturing ? KeyNone : target);
}

bool MenuState::KeyToggle(const char* label, bool* value, int target)
{
    const int* key = KeyFor(target);
    const bool capturing = capturingKey.load() == target;
    bool keyClicked = false;
    const bool toggled = UI::ToggleKey(label, value, capturing ? tr("press a key...") : KeyName(key ? *key : 0), capturing, &keyClicked);

    if (keyClicked)
        capturingKey.store(capturing ? KeyNone : target);

    return toggled;
}

void MenuState::KeyChip(const char* id, int target, float width)
{
    const int* key = KeyFor(target);
    const bool capturing = capturingKey.load() == target;
    char label[128];
    snprintf(label, sizeof(label), "%s##%s", capturing ? tr("press a key...") : KeyName(key ? *key : 0), id);

    if (UI::Button(label, ImVec2(width, 0.0f), capturing ? UI::Kind::Primary : UI::Kind::Secondary))
        capturingKey.store(capturing ? KeyNone : target);

    if (ImGui::IsItemHovered() && !capturing)
        ImGui::SetTooltip("%s", tr("Click, then press a key. Esc cancels, Backspace clears."));
}

void Menu::init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView*)
{
    gameWindow = window;

    // Menu settings (language, scale, key, display mode...) are restored on every inject.
    internal_config::ConfigManager::LoadMenuSettings();
    g_Language = Config::menu.language;
    StatTrakTracker::Load();   // real StatTrak kill counts of equipped skins (stattrak.json)

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(pDevice, pContext);

    UI::LoadFonts();
    UI::ApplyStyle();
}

void Menu::CenterCursor()
{
    if (!gameWindow)
        return;

    RECT clientRect{};
    if (!GetClientRect(gameWindow, &clientRect))
        return;

    POINT center{ (clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2 };
    if (!ClientToScreen(gameWindow, &center))
        return;

    SetCursorPos(center.x, center.y);
}

void Menu::render()
{
    SkinImages::BeginFrame();   // new pictures: a few ms of loading a frame, never a freeze
    g_Language = Config::menu.language;
    UI::ShowTips = Config::menu.showTips;

    // the menu's colour (Settings -> Menu): the picked one, or cycling through the colours (~12 s a round)
    if (Config::menu.accentRgb)
    {
        const float hue = static_cast<float>(GetTickCount64() % 12000ull) / 12000.0f;
        float r = 0.0f, g = 0.0f, b = 0.0f;
        ImGui::ColorConvertHSVtoRGB(hue, 0.62f, 0.96f, r, g, b);
        UI::SetAccent(ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f)));
    }
    else
    {
        const int rgb = Config::menu.accentColor;
        UI::SetAccent(IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255));
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;

    // Alt-tabbed / minimized: the game window shrinks to (almost) nothing. Draw nothing and leave the layout alone.
    const bool minimized = display.x < 320.0f || display.y < 240.0f;

    const float autoScale = std::clamp(display.y / 1080.0f, 0.6f, 2.0f);   // Auto: follow the game resolution
    const float baseScale = Config::menu.scaleMode == 0 ? autoScale : 1.0f;
    const float scale = std::clamp(baseScale * Config::menu.uiScale, 0.4f, 3.0f);

    if (!minimized)
    {
        UiScale::Update(scale);
        ImGui::GetStyle().FontScaleMain = scale * Config::menu.fontScale;
    }

    // Case list (retried until the game has it), ★ pool for random drops — menu open or not.
    Unboxer::TickRenderThread();
    Prices::TickUpdate();   // Settings -> Update prices: the saved price list, once it's in Downloads

    // Equipped items → the skin changer's loadout (only when the equipment changed), handed to the game thread
    // (only when something changed: a hash). StatTrak kills are saved now and then.
    Unboxer::SyncLoadout();
    // the hotkeys (bunnyhop / FOV / reapply) + the on-screen speed, menu open or not
    if (Movement::RenderThreadTick())
    {
        Unboxer::ReapplyInMatch();
        Notify::Push(tr("Skins reapplied"));
    }
    Unboxer::SetChatDrops(Config::menu.chatDrops);
    Unboxer::SetReapplyOnSpawn(Config::menu.reapplyOnSpawn, Config::menu.reapplyTimes);
    Unboxer::SetShuffleEveryRound(Config::menu.shuffleEveryRound);
    Unboxer::SetScoreboardPin(Config::menu.scoreboardPin);
    Unboxer::SetArmoryEnabled(Config::menu.armoryLootline);
    Unboxer::SetBuyKeys(Config::menu.buyKeysLootline);
    Unboxer::SetGamePricesEnabled(Config::menu.cs2Prices || Config::menu.cs2PriceTotal);
    PublishGamePrices();   // Lootline's prices in CS2's inspect popup / inventory (every 2 s, changes only)
    LocalSpray::SetOptions(Config::menu.localSpray);
    Prices::SetCurrency(Config::menu.currency);
    Config::PublishSkins();
    StatTrakTracker::SaveIfDirty();

    // Auto-save menu settings shortly after the last change (not on every slider tick).
    if (const unsigned long long dirty = MenuState::dirtySinceMs.load(); dirty != 0 && GetTickCount64() - dirty > 600)
    {
        MenuState::dirtySinceMs.store(0);
        internal_config::ConfigManager::SaveMenuSettings();
    }

    // the 3D preview (Add items, Inventory) only lives while one of those tabs is on screen
    KeepAddItemsPreview(!minimized && showMenu && (activeTab == TabAddItems || activeTab == TabInventory));

    if (minimized)
        return;

    Notify::Render();

    if (!showMenu)
    {
        openedAt = -1.0;
        return;
    }

    // Fade + a small rise on open.
    if (openedAt < 0.0)
        openedAt = ImGui::GetTime();

    float fade = 1.0f;
    if (Config::menu.animations)
    {
        const float duration = std::max(0.05f, Config::menu.animSpeed);
        fade = std::clamp(static_cast<float>((ImGui::GetTime() - openedAt) / duration), 0.0f, 1.0f);
        fade = 1.0f - (1.0f - fade) * (1.0f - fade);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

    // Size = the configured size x UI scale, always kept inside the visible display (4:3 / small resolutions).
    const ImVec2 maxSize(std::max(1.0f, display.x * 0.98f), std::max(1.0f, display.y * 0.98f));
    const ImVec2 minSize(std::min(S(760.0f), maxSize.x), std::min(S(480.0f), maxSize.y));
    const ImVec2 wantSize(
        std::clamp(Config::menu.width * scale, minSize.x, maxSize.x),
        std::clamp(Config::menu.height * scale, minSize.y, maxSize.y));

    const bool sizeFromConfig = MenuState::resetSizePending.load()
        || appliedWidth != Config::menu.width || appliedHeight != Config::menu.height || appliedScale != scale;

    if (sizeFromConfig)
        ImGui::SetNextWindowSize(wantSize, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize);

    if (firstShow || MenuState::centerPending.load())
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Lootline###UnboxerMenu", &showMenu, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (centerCursorPending)
    {
        CenterCursor();
        centerCursorPending = false;
    }

    // Size bookkeeping: remember what we applied, and adopt a drag-resize by the user as the new menu size.
    const ImVec2 windowSize = ImGui::GetWindowSize();
    MenuState::windowCenter = ImVec2(ImGui::GetWindowPos().x + windowSize.x * 0.5f, ImGui::GetWindowPos().y + windowSize.y * 0.5f);
    if (sizeFromConfig)
    {
        appliedWidth = Config::menu.width;
        appliedHeight = Config::menu.height;
        appliedScale = scale;
    }
    else if (lastWindowSize.x > 0.0f &&
        (std::fabs(windowSize.x - lastWindowSize.x) > 0.5f || std::fabs(windowSize.y - lastWindowSize.y) > 0.5f))
    {
        Config::menu.width = std::clamp(windowSize.x / scale, 700.0f, 1800.0f);
        Config::menu.height = std::clamp(windowSize.y / scale, 420.0f, 1200.0f);
        appliedWidth = Config::menu.width;
        appliedHeight = Config::menu.height;
        MenuState::MarkDirty();
    }
    lastWindowSize = windowSize;
    MenuState::resetSizePending.store(false);
    MenuState::centerPending.store(false);
    firstShow = false;

    const float headerHeight = S(60.0f);
    RenderHeader(headerHeight);

    // ── the page ──
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + headerHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(16.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, S(10.0f, 10.0f));

    if (ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground))
    {
        // the item cards' size, each tab with cards its own (Config::menu.cardPicture / cardText [Cases, Add items,
        // Inventory, Upgrader]): the two sliders on top of the tab, then its cards drawn at that size
        const int cardTab = activeTab == TabCases ? 0 : activeTab == TabAddItems ? 1 : activeTab == TabInventory ? 2 : activeTab == TabUpgrader ? 3 : -1;

        if (cardTab >= 0 && UI::CardSizeRow("##card_size", &Config::menu.cardPicture[cardTab], &Config::menu.cardText[cardTab]))
            MenuState::MarkDirty();

        UI::CardPictureScale = cardTab >= 0 ? Config::menu.cardPicture[cardTab] : 1.0f;
        UI::CardTextScale = cardTab >= 0 ? Config::menu.cardText[cardTab] : 1.0f;

        switch (activeTab)
        {
        case TabCases:
            RenderCasesPage();
            break;
        case TabAddItems:
            RenderAddItemsPage();
            break;
        case TabInventory:
            RenderMyItemsPage();
            break;
        case TabUpgrader:
            RenderUpgraderPage();
            break;
        case TabMisc:
            RenderMiscPage();
            break;
        case TabSettings:
            RenderSettingsEditor();
            break;
        default:
            activeTab = TabCases;
            break;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    ImGui::End();
    ImGui::PopStyleVar();
}

// Logo, name, the tabs, what's working, close.
void Menu::RenderHeader(float height)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    const float rounding = ImGui::GetStyle().WindowRounding;

    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), UI::Col::Header, rounding, ImDrawFlags_RoundCornersTop);
    draw->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f), UI::Col::Border);

    // logo + wordmark
    const float logoSize = S(32.0f);
    const ImVec2 logoPos(origin.x + S(18.0f), origin.y + (height - logoSize) * 0.5f);
    UI::Logo(draw, logoPos, logoSize);

    const float nameSize = ImGui::GetFontSize() * 1.35f;
    const ImVec2 namePos(logoPos.x + logoSize + S(12.0f), origin.y + height * 0.5f - nameSize * 0.62f);
    draw->AddText(UI::FontHeading, nameSize, namePos, UI::Col::Text, "LOOTLINE");
    const float nameWidth = UI::FontHeading->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, "LOOTLINE").x;
    draw->AddText(UI::FontText, ImGui::GetFontSize() * 0.72f, ImVec2(namePos.x + S(1.0f), namePos.y + nameSize * 1.02f), UI::Col::TextFaint, tr("inventory & cases"));

    // tabs
    ImGui::SetCursorScreenPos(ImVec2(namePos.x + nameWidth + S(34.0f), origin.y));

    struct TabInfo { const char* label; ImWchar icon; int id; };
    static constexpr TabInfo tabs[] = {
        { "Cases##tab", ICON_CASES, TabCases },
        { "Add items##tab", ICON_ADD_ITEMS, TabAddItems },
        { "Inventory##tab", ICON_INVENTORY, TabInventory },
        { "Upgrader##tab", ICON_UPGRADER, TabUpgrader },
        { "Misc##tab", ICON_MISC, TabMisc },
        { "Settings##tab", ICON_SETTINGS, TabSettings },
    };

    for (const TabInfo& tab : tabs)
    {
        if (UI::TopTab(tab.label, tab.icon, activeTab == tab.id, height))
            activeTab = tab.id;
        ImGui::SameLine(0.0f, 0.0f);
    }

    // right side: status + close
    const Unboxer::Status status = Unboxer::GetStatus();
    const bool ready = status.resolved && status.panoramaReady && status.inventoryReady && status.casesReady;
    const char* statusText = !status.resolved || !status.panoramaReady ? "Game update broke something" :
        !status.inventoryReady ? "Waiting for inventory" : !status.casesReady ? "Reading cases" : "Ready";

    const float closeSize = S(34.0f);
    const float statusWidth = ImGui::CalcTextSize(tr(statusText)).x + S(24.0f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - closeSize - S(14.0f) - statusWidth - S(12.0f), origin.y + (height - ImGui::GetTextLineHeight()) * 0.5f));
    UI::Status(ready, statusText, status.error.empty() ? nullptr : status.error.c_str());

    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - closeSize - S(14.0f), origin.y + (height - closeSize) * 0.5f));
    if (UI::IconButton("##close_menu", ICON_CLOSE, closeSize, "Close (Home)"))
        showMenu = false;
}

void Menu::toggleMenu()
{
    showMenu = !showMenu;
    centerCursorPending = showMenu;
}
