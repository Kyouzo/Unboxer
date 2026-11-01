#include <atomic>
#include <cmath>
#include <mutex>
#include <cstring>
#include <new>
#include <dxgi1_5.h>
#include <windowsx.h>

#include "includes.h"
#include "chadware2/chadware2.h"
#include "chadware2/utils/memory/vfunc/vfunc.h"
#include "chadware2/menu/skinchanger/images/skin_images.h"
#include "chadware2/features/model_preview/model_preview.h"
#include "chadware2/features/movement/movement.h"
#include "chadware2/features/skinchanger/agent_voice_changer.h"
#include "chadware2/features/skinchanger/knife_animation.h"
#include "chadware2/features/unboxer/local_spray.h"
#include "chadware2/features/unboxer/unboxer.h"
#include "chadware2/utils/offsets/offsets.h"

#include "../external/kiero/minhook/include/MinHook.h"

#include <d3dcompiler.h>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ChadWare2 chadware;

bool init = false;

std::atomic_bool g_Unloading = false;
std::atomic_bool g_ImGuiReady = false;
std::atomic_uint32_t g_PresentCalls = 0;
std::atomic_uint32_t g_WndProcCalls = 0;
std::recursive_mutex g_ImGuiMutex;

// The start-up work that used to freeze the game's first frame (signature scans, item schema, case list, hooks) runs on
// this thread (ChadWare2::initGame). Unloading waits for it.
std::atomic<HANDLE> g_GameInitThread = nullptr;

// Track in-flight callbacks so the DLL is not unloaded while one is still executing.
struct CallbackGuard
{
    std::atomic_uint32_t& counter;

    CallbackGuard(std::atomic_uint32_t& c) : counter(c)
    {
        counter.fetch_add(1, std::memory_order_acq_rel);
    }

    ~CallbackGuard()
    {
        counter.fetch_sub(1, std::memory_order_acq_rel);
    }
};

Present oPresent = nullptr;
ResizeBuffers oResizeBuffers = nullptr;

HWND window = NULL;
WNDPROC oWndProc = nullptr;
ID3D11Device* pDevice = NULL;
ID3D11DeviceContext* pContext = NULL;
ID3D11RenderTargetView* mainRenderTargetView = nullptr;

// Size of the swapchain backbuffer the game renders into (updated whenever the render target is created).
std::atomic_uint32_t g_BackbufferWidth = 0;
std::atomic_uint32_t g_BackbufferHeight = 0;

// "Crisp" stretched mode lays ImGui out in backbuffer pixels, so window-space mouse coordinates are remapped.
std::atomic<float> g_MouseScaleX = 1.0f;
std::atomic<float> g_MouseScaleY = 1.0f;


// ============================================================================
// WNDPROC
// ============================================================================

static bool IsMouseInputMessage(UINT uMsg)
{
    switch (uMsg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:

    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_MOUSEMOVE:

    case WM_INPUT:
        return true;

    default:
        return false;
    }
}

static bool IsKeyboardInputMessage(UINT uMsg)
{
    switch (uMsg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:

    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:

    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_UNICHAR:
        return true;

    default:
        return false;
    }
}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CallbackGuard guard(g_WndProcCalls);

    WNDPROC originalWndProc = oWndProc;

    // The window is subclassed with SetWindowLongPtrW, so pass messages on with the W (Unicode) calls —
    // the A versions translate WM_CHAR to ANSI and break typing Russian into search boxes.
    if (g_Unloading.load(std::memory_order_acquire))
    {
        if (originalWndProc)
            return CallWindowProcW(originalWndProc, hWnd, uMsg, wParam, lParam);

        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    // Settings / Misc "Key" buttons: the next key press becomes that key (Esc cancels, Page Down is reserved for unload,
    // Backspace / Delete clear a hotkey). The hotkeys can be Mouse 3 / 4 / 5 too.
    if (const int target = MenuState::capturingKey.load(); target != MenuState::KeyNone)
    {
        int vk = 0;

        if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN)
            vk = static_cast<int>(wParam);
        else if (target != MenuState::KeyMenu && uMsg == WM_MBUTTONDOWN)
            vk = VK_MBUTTON;
        else if (target != MenuState::KeyMenu && uMsg == WM_XBUTTONDOWN)
            vk = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;

        if (vk)
        {
            if (vk != VK_ESCAPE && vk != VK_NEXT && vk > 0 && vk < 256)
            {
                if (int* key = MenuState::KeyFor(target))
                {
                    *key = target != MenuState::KeyMenu && (vk == VK_BACK || vk == VK_DELETE) ? 0 : vk;
                    MenuState::MarkDirty();
                }
            }

            MenuState::capturingKey.store(MenuState::KeyNone);
            return uMsg == WM_XBUTTONDOWN ? TRUE : 0;
        }
    }

    // Handle the menu key before input suppression so the menu can always be toggled. WM_SYSKEYDOWN too: F10 and
    // Alt arrive that way, and Settings -> Menu Key lets you bind them.
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && !(lParam & (1 << 30)) && static_cast<int>(wParam) == Config::menu.menuKey)
    {
        if (!chadware.gameReady())   // still starting up (a second or two after injecting): no menu yet
            return 0;

        chadware.renderer.menu.toggleMenu();

        if (I::InputSystem)
        {
            const bool isMenuOpen = chadware.renderer.menu.showMenu;

            if (isMenuOpen)
            {
                M::CallVFunc<int, 76U>(I::InputSystem, false);
            }
            else if (I::EngineClient && I::EngineClient->in_game())
            {
                M::CallVFunc<int, 76U>(I::InputSystem, true);
            }
        }

        return 0;
    }

    if (chadware.renderer.menu.showMenu && g_ImGuiReady.load(std::memory_order_acquire) && chadware.gameReady())
    {
        // Give ImGui the input first so text fields, shortcuts, mouse input and
        // scrolling still work while the menu is open.
        {
            std::lock_guard<std::recursive_mutex> lock(g_ImGuiMutex);

            // Crisp stretched mode: ImGui is laid out in backbuffer pixels, the mouse arrives in window pixels.
            LPARAM imguiLParam = lParam;
            const float mouseScaleX = g_MouseScaleX.load();
            const float mouseScaleY = g_MouseScaleY.load();

            if (uMsg == WM_MOUSEMOVE && (mouseScaleX != 1.0f || mouseScaleY != 1.0f))
            {
                const int x = static_cast<int>(GET_X_LPARAM(lParam) * mouseScaleX);
                const int y = static_cast<int>(GET_Y_LPARAM(lParam) * mouseScaleY);
                imguiLParam = MAKELPARAM(x, y);
            }

            if (ImGui::GetCurrentContext())
                ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, imguiLParam);
        }

        // Block the same mouse/keyboard messages from reaching the game after
        // ImGui has consumed them. WM_CHAR is required for InputText typing.
        if (IsMouseInputMessage(uMsg) || IsKeyboardInputMessage(uMsg))
            return 0;
    }

    if (originalWndProc)
        return CallWindowProcW(originalWndProc, hWnd, uMsg, wParam, lParam);

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}


// ============================================================================
// SHADOW VTABLE / PRESENT BOOTSTRAP
// ============================================================================

Present g_BootstrapOriginalPresent = nullptr;
void* g_BootstrapPresentTarget = nullptr;
void* g_ResizeBuffersTarget = nullptr;

std::atomic_bool g_BootstrapEnabled = false;
std::atomic_bool g_ResizeBuffersEnabled = false;
std::atomic_bool g_ShadowInstalled = false;
std::atomic_bool g_SwapChainTransition = false;
std::atomic_uint32_t g_BootstrapPresentCalls = 0;
std::atomic_uint32_t g_ResizeBuffersCalls = 0;

IDXGISwapChain* g_ShadowSwapChain = nullptr;
void** g_OriginalSwapChainVTable = nullptr;
void** g_ShadowSwapChainVTable = nullptr;
std::size_t g_ShadowSwapChainVTableSize = 0;

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
HRESULT __stdcall hkBootstrapPresentFn(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

bool IsProcessSwapChain(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain)
        return false;

    DXGI_SWAP_CHAIN_DESC desc{};

    if (FAILED(pSwapChain->GetDesc(&desc)) || !desc.OutputWindow)
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &processId);

    return processId == GetCurrentProcessId();
}

bool IsGameWindowSwapChain(IDXGISwapChain* pSwapChain)
{
    if (!IsProcessSwapChain(pSwapChain))
        return false;

    DXGI_SWAP_CHAIN_DESC desc{};

    if (FAILED(pSwapChain->GetDesc(&desc)) || !desc.OutputWindow)
        return false;

    // Before ImGui has initialized, process ownership is the best filter we
    // have. Afterwards, require the same HWND as the swapchain we render to.
    return window == nullptr || desc.OutputWindow == window;
}

std::size_t GetSwapChainVTableSize(IDXGISwapChain* pSwapChain)
{
    IDXGISwapChain4* swapChain4 = nullptr;

    if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void**>(&swapChain4))))
    {
        swapChain4->Release();
        return 41;
    }

    IDXGISwapChain3* swapChain3 = nullptr;

    if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3))))
    {
        swapChain3->Release();
        return 40;
    }

    IDXGISwapChain2* swapChain2 = nullptr;

    if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&swapChain2))))
    {
        swapChain2->Release();
        return 36;
    }

    IDXGISwapChain1* swapChain1 = nullptr;

    if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))))
    {
        swapChain1->Release();
        return 29;
    }

    return 18;
}

bool InstallShadowSwapChain(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain)
        return false;

    if (g_ShadowInstalled.load(std::memory_order_acquire))
        return g_ShadowSwapChain == pSwapChain;

    void** originalVTable = *reinterpret_cast<void***>(pSwapChain);

    if (!originalVTable)
        return false;

    const std::size_t vtableSize = GetSwapChainVTableSize(pSwapChain);

    if (vtableSize <= 8)
        return false;

    void** shadowVTable = new (std::nothrow) void* [vtableSize];

    if (!shadowVTable)
        return false;

    std::memcpy(shadowVTable, originalVTable, vtableSize * sizeof(void*));

    Present originalPresent = reinterpret_cast<Present>(originalVTable[8]);

    if (!originalPresent)
    {
        delete[] shadowVTable;
        return false;
    }

    // Only Present is shadow-vtable hooked. ResizeBuffers uses its own MinHook detour 
    shadowVTable[8] = reinterpret_cast<void*>(hkPresent);

    g_ShadowSwapChain = pSwapChain;
    g_OriginalSwapChainVTable = originalVTable;
    g_ShadowSwapChainVTable = shadowVTable;
    g_ShadowSwapChainVTableSize = vtableSize;
    oPresent = originalPresent;

    std::atomic_thread_fence(std::memory_order_release);

    void* previousVTable = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(pSwapChain), shadowVTable, originalVTable);

    if (previousVTable != originalVTable)
    {
        g_ShadowSwapChain = nullptr;
        g_OriginalSwapChainVTable = nullptr;
        g_ShadowSwapChainVTable = nullptr;
        g_ShadowSwapChainVTableSize = 0;
        oPresent = nullptr;
        delete[] shadowVTable;
        return false;
    }

    // Keep the COM object alive for as long as its vptr points at our
    // private table. This makes a later restore safe even if CS2 has already
    // stopped using this swapchain.
    pSwapChain->AddRef();

    g_ShadowInstalled.store(true, std::memory_order_release);


    return true;
}

bool RestoreShadowSwapChain()
{
    if (!g_ShadowInstalled.load(std::memory_order_acquire))
        return true;

    IDXGISwapChain* swapChain = g_ShadowSwapChain;
    void** originalVTable = g_OriginalSwapChainVTable;
    void** shadowVTable = g_ShadowSwapChainVTable;

    if (!swapChain || !originalVTable || !shadowVTable)
        return false;

    void* previousVTable = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(swapChain), originalVTable, shadowVTable);

    if (previousVTable != shadowVTable)
    {
        return false;
    }

    g_ShadowInstalled.store(false, std::memory_order_release);

    // Balances the AddRef taken by InstallShadowSwapChain().
    swapChain->Release();


    return true;
}

HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    Present originalPresent = g_BootstrapEnabled.load(std::memory_order_acquire) ? g_BootstrapOriginalPresent : oPresent;

    if (!originalPresent)
        return DXGI_ERROR_INVALID_CALL;

    return originalPresent(pSwapChain, SyncInterval, Flags);
}

LRESULT CALLBACK BootstrapWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================================
// HELPERS
// ============================================================================

bool GetD3D11HookTargets(void** presentTarget, void** resizeBuffersTarget)
{
    if (!presentTarget || !resizeBuffersTarget)
        return false;

    *presentTarget = nullptr;
    *resizeBuffersTarget = nullptr;

    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BootstrapWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"Unboxer_D3D11_Bootstrap";

    ATOM classAtom = RegisterClassExW(&wc);

    if (!classAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    HWND dummyWindow = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, instance, nullptr);

    if (!dummyWindow)
    {
        if (classAtom)
            UnregisterClassW(wc.lpszClassName, instance);

        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = dummyWindow;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11Device* dummyDevice = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;
    D3D_FEATURE_LEVEL selectedFeatureLevel{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &swapChainDesc, &dummySwapChain, &dummyDevice, &selectedFeatureLevel, &dummyContext);

    if (SUCCEEDED(hr) && dummySwapChain)
    {
        void** vtable = *reinterpret_cast<void***>(dummySwapChain);

        if (vtable)
        {
            *presentTarget = vtable[8];
            *resizeBuffersTarget = vtable[13];
        }
    }

    if (dummyContext)
        dummyContext->Release();

    if (dummyDevice)
        dummyDevice->Release();

    if (dummySwapChain)
        dummySwapChain->Release();

    DestroyWindow(dummyWindow);

    if (classAtom)
        UnregisterClassW(wc.lpszClassName, instance);

    return *presentTarget != nullptr && *resizeBuffersTarget != nullptr;
}

bool InstallPresentBootstrap()
{
    if (g_BootstrapPresentTarget && g_ResizeBuffersTarget)
        return true;

    void* presentTarget = nullptr;
    void* resizeBuffersTarget = nullptr;

    if (!GetD3D11HookTargets(&presentTarget, &resizeBuffersTarget))
        return false;

    MH_STATUS status = MH_Initialize();

    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    status = MH_CreateHook(presentTarget, reinterpret_cast<void*>(hkBootstrapPresentFn), reinterpret_cast<void**>(&g_BootstrapOriginalPresent));

    if (status != MH_OK)
        return false;

    status = MH_CreateHook(resizeBuffersTarget, reinterpret_cast<void*>(hkResizeBuffers), reinterpret_cast<void**>(&oResizeBuffers));

    if (status != MH_OK)
    {
        MH_RemoveHook(presentTarget);
        g_BootstrapOriginalPresent = nullptr;
        return false;
    }

    g_BootstrapPresentTarget = presentTarget;
    g_ResizeBuffersTarget = resizeBuffersTarget;

    status = MH_EnableHook(resizeBuffersTarget);

    if (status != MH_OK && status != MH_ERROR_ENABLED)
    {
        MH_RemoveHook(resizeBuffersTarget);
        MH_RemoveHook(presentTarget);
        g_BootstrapPresentTarget = nullptr;
        g_ResizeBuffersTarget = nullptr;
        g_BootstrapOriginalPresent = nullptr;
        oResizeBuffers = nullptr;
        return false;
    }

    g_ResizeBuffersEnabled.store(true, std::memory_order_release);

    status = MH_EnableHook(presentTarget);

    if (status != MH_OK && status != MH_ERROR_ENABLED)
    {
        MH_DisableHook(resizeBuffersTarget);
        g_ResizeBuffersEnabled.store(false, std::memory_order_release);
        MH_RemoveHook(resizeBuffersTarget);
        MH_RemoveHook(presentTarget);
        g_BootstrapPresentTarget = nullptr;
        g_ResizeBuffersTarget = nullptr;
        g_BootstrapOriginalPresent = nullptr;
        oResizeBuffers = nullptr;
        return false;
    }

    g_BootstrapEnabled.store(true, std::memory_order_release);


    return true;
}

bool DisablePresentBootstrap()
{
    if (!g_BootstrapPresentTarget || !g_BootstrapEnabled.load(std::memory_order_acquire))
        return true;

    MH_STATUS status = MH_DisableHook(g_BootstrapPresentTarget);

    if (status != MH_OK && status != MH_ERROR_DISABLED)
        return false;

    g_BootstrapEnabled.store(false, std::memory_order_release);


    return true;
}

bool DisableResizeBuffersHook()
{
    if (!g_ResizeBuffersTarget || !g_ResizeBuffersEnabled.load(std::memory_order_acquire))
        return true;

    MH_STATUS status = MH_DisableHook(g_ResizeBuffersTarget);

    if (status != MH_OK && status != MH_ERROR_DISABLED)
        return false;

    g_ResizeBuffersEnabled.store(false, std::memory_order_release);


    return true;
}

void CleanupRenderTarget()
{
    if (mainRenderTargetView)
    {
        mainRenderTargetView->Release();
        mainRenderTargetView = nullptr;
    }
}

bool CreateRenderTarget(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain || !pDevice)
        return false;

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));

    if (FAILED(hr))
        return false;

    D3D11_TEXTURE2D_DESC backBufferDesc{};
    backBuffer->GetDesc(&backBufferDesc);
    g_BackbufferWidth.store(backBufferDesc.Width);
    g_BackbufferHeight.store(backBufferDesc.Height);

    hr = pDevice->CreateRenderTargetView(backBuffer, nullptr, &mainRenderTargetView);
    backBuffer->Release();

    return SUCCEEDED(hr);
}

// Borderless "stretched" resolutions (e.g. 1440x1080 on a 1920x1080 window): the game renders into a smaller
// backbuffer that the compositor stretches over the window. ImGui sizes itself from the WINDOW, so by default the
// menu is cut off past the backbuffer width and the mouse doesn't line up with what's drawn. Applied after
// ImGui_ImplWin32_NewFrame (which resets DisplaySize from the window every frame). See Settings → Display.
void ApplyStretchedDisplayMode()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    g_MouseScaleX.store(1.0f);
    g_MouseScaleY.store(1.0f);

    RECT client{};
    if (!window || !GetClientRect(window, &client))
        return;

    const float windowW = static_cast<float>(client.right - client.left);
    const float windowH = static_cast<float>(client.bottom - client.top);
    const float backW = static_cast<float>(g_BackbufferWidth.load());
    const float backH = static_cast<float>(g_BackbufferHeight.load());

    MenuState::windowW.store(static_cast<int>(windowW));
    MenuState::windowH.store(static_cast<int>(windowH));
    MenuState::backbufferW.store(static_cast<int>(backW));
    MenuState::backbufferH.store(static_cast<int>(backH));

    if (windowW < 1.0f || windowH < 1.0f || backW < 1.0f || backH < 1.0f)
        return;

    if (std::fabs(windowW - backW) < 1.0f && std::fabs(windowH - backH) < 1.0f)
        return; // not stretched: nothing to do

    switch (Config::menu.stretchMode)
    {
    case 0:
        // Normal look: lay the menu out in window pixels and draw it squeezed into the backbuffer; the stretch then
        // restores normal proportions. Mouse coordinates already match. (ImGui's DX11 backend scales the viewport
        // and clip rects by the framebuffer scale; the imgui.cpp density patch keeps the text sharp.)
        io.DisplaySize = ImVec2(windowW, windowH);
        io.DisplayFramebufferScale = ImVec2(backW / windowW, backH / windowH);
        break;

    case 1:
        // Crisp: lay the menu out in backbuffer pixels (sharpest text, stretched wide like the game) and remap
        // the window-space mouse into backbuffer space in the WndProc.
        io.DisplaySize = ImVec2(backW, backH);
        g_MouseScaleX.store(backW / windowW);
        g_MouseScaleY.store(backH / windowH);
        break;

    default:
        break; // Off: original Chadware behaviour
    }
}

HRESULT __stdcall hkBootstrapPresentFn(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    CallbackGuard guard(g_BootstrapPresentCalls);

    if (!g_Unloading.load(std::memory_order_acquire) && IsProcessSwapChain(pSwapChain) && !g_ShadowInstalled.load(std::memory_order_acquire))
        InstallShadowSwapChain(pSwapChain);

    if (!g_BootstrapOriginalPresent)
        return DXGI_ERROR_INVALID_CALL;

    return g_BootstrapOriginalPresent(pSwapChain, SyncInterval, Flags);
}

extern "C" void* __guard_check_icall_fptr;
extern "C" void* __guard_dispatch_icall_fptr;
extern "C" void* __guard_xfg_check_icall_fptr;
extern "C" void* __guard_xfg_dispatch_icall_fptr;

extern "C" decltype(&D3DCompile) __imp_D3DCompile;

// ============================================================================
// PRESENT HOOK
// ============================================================================

DWORD WINAPI GameInitThread(LPVOID)
{
    chadware.initGame(pDevice, pContext);
    return 0;
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    CallbackGuard guard(g_PresentCalls);

    if (g_Unloading.load(std::memory_order_acquire))
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);

    if (g_SwapChainTransition.load(std::memory_order_acquire))
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);

    if (!init)
    {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pDevice))))
        {
            pDevice->GetImmediateContext(&pContext);

            DXGI_SWAP_CHAIN_DESC sd{};
            pSwapChain->GetDesc(&sd);
            window = sd.OutputWindow;

            if (!CreateRenderTarget(pSwapChain))
            {
                if (pContext)
                {
                    pContext->Release();
                    pContext = nullptr;
                }

                if (pDevice)
                {
                    pDevice->Release();
                    pDevice = nullptr;
                }

                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }

            oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

            chadware.initRender(window, pDevice, pContext, mainRenderTargetView);

            g_ImGuiReady.store(true, std::memory_order_release);
            init = true;

            // the rest of the start-up on its own thread: the game keeps drawing frames meanwhile
            HANDLE thread = CreateThread(nullptr, 0, GameInitThread, nullptr, 0, nullptr);

            if (thread)
                g_GameInitThread.store(thread, std::memory_order_release);
            else
                chadware.initGame(pDevice, pContext);   // no thread: the old way (a short freeze)

        }
        else
        {
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
    }

    if (!mainRenderTargetView)
    {
        if (!CreateRenderTarget(pSwapChain))
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_ImGuiMutex);

        if (
            g_ImGuiReady.load(std::memory_order_acquire) &&
            ImGui::GetCurrentContext())
        {

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ApplyStretchedDisplayMode();
            ImGui::NewFrame();

            if (chadware.ready(pDevice))
            {
                // D3D11-only preview work (Chadware's order): it may snapshot / promote SRVs, so before the menu creates
                // draw commands that reference them. Panorama / V8 is never touched from Present.
                ModelPreview::TickRenderThread();

                chadware.renderer.menu.render();
            }
            else
            {
                ImGui::GetForegroundDrawList()->AddText(ImVec2(14.0f, 14.0f), IM_COL32(255, 255, 255, 210), "Lootline is loading...");
            }

            ImGui::Render();

            if (mainRenderTargetView && pContext)
            {
                pContext->OMSetRenderTargets(
                    1,
                    &mainRenderTargetView,
                    nullptr
                );

                ImGui_ImplDX11_RenderDrawData(
                    ImGui::GetDrawData()
                );
            }
        }
    }

    return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
}

// ============================================================================
// RESIZEBUFFERS HOOK
// ============================================================================

HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    CallbackGuard guard(g_ResizeBuffersCalls);

    if (!oResizeBuffers)
        return DXGI_ERROR_INVALID_CALL;

    if (g_Unloading.load(std::memory_order_acquire))
        return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    const bool gameWindowSwapChain = IsGameWindowSwapChain(pSwapChain);
    const bool currentSwapChain = pSwapChain == g_ShadowSwapChain;


    // Ignore swapchains that do not belong to the CS2 render window.
    if (!gameWindowSwapChain)
        return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    g_SwapChainTransition.store(true, std::memory_order_release);

    // If ResizeBuffers arrived on a different swapchain for the same game
    // window, CS2 has replaced the object. Move the per-object Present shadow
    // hook before allowing rendering to resume.
    if (!currentSwapChain && g_ShadowInstalled.load(std::memory_order_acquire))
    {

        CleanupRenderTarget();

        // Restore the old object's original vptr. InstallShadowSwapChain held
        // an AddRef, so the object is guaranteed to remain valid here.
        if (!RestoreShadowSwapChain())
        {
            g_SwapChainTransition.store(false, std::memory_order_release);
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }

        // The old private table is intentionally not freed here. A Present
        // call may already have dispatched through it. It is now unreferenced
        // by the old COM object and can safely be leaked until process exit.
        g_ShadowSwapChain = nullptr;
        g_OriginalSwapChainVTable = nullptr;
        g_ShadowSwapChainVTable = nullptr;
        g_ShadowSwapChainVTableSize = 0;
        oPresent = nullptr;

        if (!InstallShadowSwapChain(pSwapChain))
        {
            g_SwapChainTransition.store(false, std::memory_order_release);
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }

    }
    else
    {
        CleanupRenderTarget();
    }

    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(hr) && pSwapChain == g_ShadowSwapChain)
    {
        bool created = CreateRenderTarget(pSwapChain);

    }

    g_SwapChainTransition.store(false, std::memory_order_release);
    return hr;
}


// ============================================================================
// MAIN THREAD
// ============================================================================

DWORD WINAPI MainThread(LPVOID lpReserved)
{

    bool bootstrapInstalled = false;

    // Page Down unloads (Page Up is Chadware's, so both can be loaded at once)
    while (!GetAsyncKeyState(VK_NEXT) && !MenuState::unloadRequested.load(std::memory_order_acquire))
    {

        if (!bootstrapInstalled)
        {
            bootstrapInstalled = InstallPresentBootstrap();

        }

        if (bootstrapInstalled && g_ShadowInstalled.load(std::memory_order_acquire) && g_BootstrapEnabled.load(std::memory_order_acquire))
            DisablePresentBootstrap();

        AgentVoice::Prepare();   // agent voice lines: builds the voice tables off the game's threads (Chadware's way)
        Sleep(10);
    }

    // Unloaded while still starting up: let the start-up thread finish first (it installs the hooks undone below).
    if (const HANDLE thread = g_GameInitThread.exchange(nullptr, std::memory_order_acq_rel))
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }

    // Lootline graffiti: the main thread takes its decals off the walls while its hooks still run
    LocalSpray::Shutdown(500);

    // added items still NEW: acknowledged locally while the acknowledge hook still runs (never by CS2 itself, which
    // would ask Valve about a Lootline id)
    Unboxer::AcknowledgeBeforeUnload(1000);

    // from here the knife fix and the voice lines leave the game alone (their hooks just pass through)
    KnifeAnimation::Shutdown();
    AgentVoice::Shutdown();

    Offsets::Shutdown();   // a running "Check Signatures" must finish before the DLL goes away

    if (I::InputSystem)
    {
        if (chadware.renderer.menu.showMenu) {
            chadware.renderer.menu.showMenu = false;
            if (I::EngineClient && I::EngineClient->in_game())
                M::CallVFunc<int, 76U>(I::InputSystem, true);
        }
    }


    g_Unloading.store(true, std::memory_order_release);
    g_ImGuiReady.store(false, std::memory_order_release);

    if (!DisablePresentBootstrap())
    {
        g_Unloading.store(false, std::memory_order_release);
        g_ImGuiReady.store(init, std::memory_order_release);
        return 0;
    }

    if (!DisableResizeBuffersHook())
    {
        g_Unloading.store(false, std::memory_order_release);
        g_ImGuiReady.store(init, std::memory_order_release);
        return 0;
    }

    if (!RestoreShadowSwapChain())
    {
        g_Unloading.store(false, std::memory_order_release);
        g_ImGuiReady.store(init, std::memory_order_release);
        return 0;
    }

    if (window != nullptr && oWndProc != nullptr)
    {
        WNDPROC currentWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));

        if (currentWndProc != WndProc)
        {
            return 0;
        }

        SetLastError(ERROR_SUCCESS);

        LONG_PTR wndProcResult = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
        DWORD wndProcError = GetLastError();

        if (wndProcResult == 0 && wndProcError != ERROR_SUCCESS)
        {
            return 0;
        }

    }


    int stableZeroCount = 0;

    while (stableZeroCount < 100)
    {
        const uint32_t bootstrapCalls = g_BootstrapPresentCalls.load(std::memory_order_acquire);
        const uint32_t presentCalls = g_PresentCalls.load(std::memory_order_acquire);
        const uint32_t resizeCalls = g_ResizeBuffersCalls.load(std::memory_order_acquire);
        const uint32_t wndProcCalls = g_WndProcCalls.load(std::memory_order_acquire);

        if (bootstrapCalls == 0 && presentCalls == 0 && resizeCalls == 0 && wndProcCalls == 0)
            ++stableZeroCount;
        else
            stableZeroCount = 0;

        Sleep(1);
    }

    // A first frame that was already starting Lootline up when Page Down came (no frame can start it any more now)
    if (const HANDLE thread = g_GameInitThread.exchange(nullptr, std::memory_order_acq_rel))
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        KnifeAnimation::Shutdown();
        AgentVoice::Shutdown();
    }


    // Misc tab: let go of +jump and give the game its FOV back while the hooks are still running.
    Movement::Shutdown();

    // Stop every game detour (UseTool; panorama RunFrame, FrameStageNotify and the mouse functions are vtable slots,
    // put back here), then wait until no game thread is still inside one. Only then free anything.
    MH_DisableHook(MH_ALL_HOOKS);

    H::Hooks hookManager;
    hookManager.shutdown();

    H::WaitForHookCalls(5000);

    ModelPreview::Shutdown();
    Unboxer::Shutdown();
    SkinImages::Shutdown();

    const MH_STATUS minHookShutdown = MH_Uninitialize();
    (void)minHookShutdown;


    if (init)
    {
        ImGui_ImplDX11_Shutdown();

        ImGui_ImplWin32_Shutdown();

        if (ImGui::GetCurrentContext() != nullptr)
        {
            ImGui::DestroyContext();
        }

        init = false;
    }

    CleanupRenderTarget();

    if (pContext)
    {
        pContext->Release();
        pContext = nullptr;
    }

    if (pDevice)
    {
        pDevice->Release();
        pDevice = nullptr;
    }

    oWndProc = nullptr;
    window = nullptr;

    if (g_ShadowSwapChainVTable)
    {
        delete[] g_ShadowSwapChainVTable;
        g_ShadowSwapChainVTable = nullptr;
    }

    g_ShadowSwapChain = nullptr;
    g_OriginalSwapChainVTable = nullptr;
    g_ShadowSwapChainVTableSize = 0;
    oPresent = nullptr;
    oResizeBuffers = nullptr;
    g_ResizeBuffersTarget = nullptr;

    Sleep(10);


    FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpReserved), EXIT_SUCCESS);
    return 0;
}


// ============================================================================
// DLL MAIN
// ============================================================================

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hMod);

        HANDLE thread = CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);

        if (thread)
            CloseHandle(thread);
    }

    return TRUE;
}