#pragma once

#include <atomic>

#include "config/config.h"
#include "hooks/hooks.h"
#include "interfaces/interfaces.h"
#include "renderer/renderer.h"
#include "utils/schema/schema.h"

class ChadWare2
{
public:
    // The game's first frame (render thread): the menu's settings and ImGui. Quick.
    void initRender(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView);

    // Its own thread, started right after initRender: everything else - the signature scans, the item schema, the case
    // list, the hooks. It used to run inside the game's first frame, which froze the game for a moment.
    void initGame(ID3D11Device* pDevice, ID3D11DeviceContext* pContext);

    // Render thread, every frame: true once initGame is done. The last step (the menu's pictures) runs here, on the
    // render thread like before.
    bool ready(ID3D11Device* pDevice);

    // initGame is done (any thread)
    [[nodiscard]] bool gameReady() const { return m_gameReady.load(std::memory_order_acquire); }

    Schema schema;
    Renderer renderer;
    H::Hooks hooks;
    I::Interfaces interfaces;

private:
    std::atomic_bool m_gameReady{ false };
    bool m_renderReady = false;   // render thread only
};

extern ChadWare2 chadware;
