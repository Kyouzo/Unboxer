#include "chadware2.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>

#include "features/model_preview/model_preview.h"
#include "features/skinchanger/econ_item_attribute_manager.h"

#include "features/skinchanger/item_schema.h"
#include "features/unboxer/prices.h"
#include "features/unboxer/unboxer.h"
#include "menu/skinchanger/images/skin_images.h"

namespace
{
    // how long each start-up step took (milliseconds), written to the Lootline log at the end
    class StepClock
    {
    public:
        StepClock()
        {
            QueryPerformanceFrequency(&m_frequency);
            QueryPerformanceCounter(&m_last);
        }

        void Step(const char* name)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double ms = m_frequency.QuadPart ? (now.QuadPart - m_last.QuadPart) * 1000.0 / m_frequency.QuadPart : 0.0;
            m_last = now;
            m_total += ms;

            const std::size_t used = std::strlen(m_text);
            if (used + 48 < sizeof(m_text))
                snprintf(m_text + used, sizeof(m_text) - used, "%s%s %.0f", used ? ", " : "", name, ms);
        }

        void Write(const char* what) const
        {
            char line[640];
            snprintf(line, sizeof(line), "%s: %.0f ms (%s)", what, m_total, m_text);
            Unboxer::LogLine(line);
        }

    private:
        LARGE_INTEGER m_frequency{};
        LARGE_INTEGER m_last{};
        double m_total = 0.0;
        char m_text[512]{};
    };
}

void ChadWare2::initRender(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView)
{
    renderer.menu.init(window, pDevice, pContext, mainRenderTargetView);
}

void ChadWare2::initGame(ID3D11Device* pDevice, ID3D11DeviceContext* pContext)
{
    StepClock clock;

    schema.init("client.dll", 0);
    clock.Step("schema");
    interfaces.init();
    clock.Step("interfaces");
    ModelPreview::Initialize(pDevice, pContext);   // the Add items tab's 3D preview (Chadware's)
    clock.Step("3d preview");
    // stickers in a match where CS2's sticker popup put them (their own slot's position, not slot 0's)
    EconItemAttributeManager::SetStickerSchemaFromSlot(true);
    EconItemAttributeManager::SetReplaceRealItem(true);   // a Lootline skin replaces the whole real item (stickers, charm...)
    Skins::g_ItemSchema.Initialize();
    clock.Step("item schema");
    Unboxer::Initialize();
    clock.Step("lootline");
    Prices::Start();   // the upgrader's prices: the list built into the DLL (never online)
    clock.Step("prices");

    // Last: panorama's RunFrame (main thread) starts calling Unboxer right away; everything it reads must be
    // built by then.
    hooks.init();
    clock.Step("hooks");
    clock.Write("start-up (in the background, the game keeps running)");

    m_gameReady.store(true, std::memory_order_release);

    // still this thread (unloading waits for it): the signature check, a CS2 popup if a CS2 update broke something
    Unboxer::CheckAfterUpdate();
}

bool ChadWare2::ready(ID3D11Device* pDevice)
{
    if (m_renderReady)
        return true;

    if (!m_gameReady.load(std::memory_order_acquire))
        return false;

    SkinImages::Initialize(pDevice);   // the render thread's COM, like it always was
    m_renderReady = true;
    return true;
}
