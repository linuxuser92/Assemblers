#include "D3D12Hook.h"
#include "Renderer.h"
#include "menu/Menu.h"

#include <MinHook.h>

D3D12Hook::Hook D3D12Hook::m_createFactory;
D3D12Hook::Hook D3D12Hook::m_createFactory1;
D3D12Hook::Hook D3D12Hook::m_createFactory2;
D3D12Hook::Hook D3D12Hook::m_createSwapChain;
D3D12Hook::Hook D3D12Hook::m_createSwapChainForHwnd;
D3D12Hook::Hook D3D12Hook::m_createSwapChainForCoreWindow;
D3D12Hook::Hook D3D12Hook::m_createSwapChainForComposition;
D3D12Hook::Hook D3D12Hook::m_present;
D3D12Hook::Hook D3D12Hook::m_executeCommandLists;

void* D3D12Hook::m_factoryVTable = nullptr;
void* D3D12Hook::m_swapChainVTable = nullptr;
void* D3D12Hook::m_lastSwapChain = nullptr;

Microsoft::WRL::ComPtr<ID3D12Device> D3D12Hook::m_device;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> D3D12Hook::m_queue;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> D3D12Hook::m_gameQueue;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> D3D12Hook::m_probeQueue;
bool D3D12Hook::m_installed = false;

namespace
{
    HWND CreateDummySwapChainWindow()
    {
        static const wchar_t* kClassName = L"BedrockMenuDummySwapChain";
        static ATOM atom = 0;
        if (!atom)
        {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = kClassName;
            atom = RegisterClassExW(&wc);
        }
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        return CreateWindowExW(0, kClassName, L"", 0, 0, 0, 1, 1,
                               nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    }
}

bool D3D12Hook::Hook::Create(void* t, void* d)
{
    target = t;
    detour = d;
    if (!target || !detour) return false;
    const MH_STATUS status = MH_CreateHook(target, detour, &original);
    if (status != MH_OK)
    {
        Log("MinHook: failed to hook %p (%s)", target, MH_StatusToString(status));
        return false;
    }
    return true;
}

bool D3D12Hook::Hook::Enable()
{
    if (!target || installed) return installed;
    const MH_STATUS status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("MinHook: failed to enable %p (%s)", target, MH_StatusToString(status));
        return false;
    }
    installed = true;
    return true;
}

void D3D12Hook::Hook::Disable()
{
    if (!target || !installed) return;
    MH_DisableHook(target);
    MH_RemoveHook(target);
    installed = false;
}

bool D3D12Hook::Install()
{
    if (m_installed) return true;
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("MinHook init failed: %s", MH_StatusToString(status));
        return false;
    }

    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi)
    {
        Log("D3D12Hook: dxgi.dll not found");
        return false;
    }

    // Hooking the export also redirects every IAT call to it, regardless of injection timing
    // (as long as the game creates its swap chain after the hook is active).
    struct ExportHook { const char* name; Hook* hook; void* detour; };
    ExportHook exports[] = {
        { "CreateDXGIFactory",  &m_createFactory,  &CreateDXGIFactoryHook },
        { "CreateDXGIFactory1", &m_createFactory1, &CreateDXGIFactory1Hook },
        { "CreateDXGIFactory2", &m_createFactory2, &CreateDXGIFactory2Hook },
    };
    for (auto& entry : exports)
    {
        void* address = reinterpret_cast<void*>(GetProcAddress(dxgi, entry.name));
        if (address && entry.hook->Create(address, entry.detour))
        {
            entry.hook->Enable();
            Log("Hooked %s at %p", entry.name, address);
        }
    }

    m_installed = true;

    // The late-hook fallback is deliberately deferred by the init thread (see main.cpp):
    // creating a second D3D12 device here can collide with the game's own initialization.

    Log("D3D12Hook installed.");
    return true;
}

bool D3D12Hook::InstallLatePresentHook()
{
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return false;

    using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto createFactory2 = reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
    if (!createFactory2) return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(createFactory2(0, IID_PPV_ARGS(&factory)))) return false;

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return false;

    HWND hwnd = CreateDummySwapChainWindow();
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain;
    const HRESULT hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc,
                                                        nullptr, nullptr, &swapChain);
    if (SUCCEEDED(hr) && swapChain)
    {
        HookSwapChain(swapChain.Get());
        Log("Late Present hook installed via dummy swap chain (hr=0x%08X)", hr);
    }
    else
    {
        Log("Dummy swap chain creation failed: 0x%08X", hr);
    }

    swapChain.Reset();
    queue.Reset();
    device.Reset();
    factory.Reset();
    DestroyWindow(hwnd);
    return m_present.installed;
}

bool D3D12Hook::HookFactory(IUnknown* factory)
{
    void** vtable = *reinterpret_cast<void***>(factory);
    if (!vtable || vtable == m_factoryVTable) return true;
    m_factoryVTable = vtable;

    // IDXGIFactory::CreateSwapChain              = 11
    // IDXGIFactory2::CreateSwapChainForHwnd      = 16
    // IDXGIFactory2::CreateSwapChainForCoreWindow= 17
    // IDXGIFactory2::CreateSwapChainForComposition = 25
    struct { Hook* hook; void* fn; UINT slot; const char* name; } entries[] = {
        { &m_createSwapChain,               &CreateSwapChainHook,               11, "CreateSwapChain" },
        { &m_createSwapChainForHwnd,        &CreateSwapChainForHwndHook,        16, "CreateSwapChainForHwnd" },
        { &m_createSwapChainForCoreWindow,  &CreateSwapChainForCoreWindowHook,  17, "CreateSwapChainForCoreWindow" },
        { &m_createSwapChainForComposition, &CreateSwapChainForCompositionHook, 25, "CreateSwapChainForComposition" },
    };

    for (auto& e : entries)
    {
        if (!e.hook->Create(vtable[e.slot], e.fn)) continue;
        e.hook->Enable();
        Log("Hooked IDXGIFactory vtable[%u] %s", e.slot, e.name);
    }
    return true;
}

bool D3D12Hook::HookSwapChain(IDXGISwapChain* swapChain)
{
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!vtable || !CanRead(vtable, 11 * sizeof(void*))) return false;
    if (vtable == m_swapChainVTable) return true;

    // IDXGISwapChain::Present = vtable slot 8. Validate before handing it to MinHook.
    void* presentFunction = vtable[8];
    if (!presentFunction || !CanRead(presentFunction, 16))
    {
        Log("HookSwapChain: invalid Present vtable entry %p", presentFunction);
        return false;
    }

    m_swapChainVTable = vtable;
    if (m_present.Create(presentFunction, &PresentHook))
    {
        m_present.Enable();
        Log("Hooked IDXGISwapChain::Present (vtable[8] = %p)", presentFunction);
    }
    return true;
}

bool D3D12Hook::EnsureExecuteHook(ID3D12CommandQueue* queue)
{
    if (m_executeCommandLists.installed || !queue) return m_executeCommandLists.installed;

    void** vtable = *reinterpret_cast<void***>(queue);
    if (!vtable || !CanRead(vtable, 11 * sizeof(void*))) return false;

    // D3D12 command queues share this vtable slot. Patching it once lets us observe
    // the game's queue without modifying any game-owned object.
    void* fn = vtable[10];
    if (!fn || !CanRead(fn, 16)) return false;

    if (!m_executeCommandLists.Create(fn, &ExecuteCommandListsHook)) return false;
    m_executeCommandLists.Enable();
    Log("Hooked ID3D12CommandQueue::ExecuteCommandLists (vtable[10] = %p)", fn);
    return true;
}

void D3D12Hook::CaptureDevice(ID3D12CommandQueue* queue)
{
    if (!queue) return;
    if (m_gameQueue && m_device) return;

    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;

    // The probe queue is ours; anything else belongs to the game.
    if (!m_gameQueue && queue != m_probeQueue.Get())
    {
        m_gameQueue = queue;
        Log("Captured game command queue");
        // The probe queue existed only to install the shared ExecuteCommandLists hook.
        m_probeQueue.Reset();
    }

    if (!m_device)
    {
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device)
            m_device = device;
    }
}

void D3D12Hook::OnSwapChainCreated(IUnknown* device, IDXGISwapChain** swapChain)
{
    if (!swapChain || !*swapChain) return;
    HookSwapChain(*swapChain);

    ComPtr<ID3D12CommandQueue> queue;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
    {
        CaptureDevice(queue.Get());
        EnsureExecuteHook(queue.Get());
    }
}

HRESULT STDMETHODCALLTYPE D3D12Hook::CreateDXGIFactoryHook(REFIID riid, void** ppFactory)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(REFIID, void**);
    const HRESULT result = reinterpret_cast<Fn>(m_createFactory.original)(riid, ppFactory);
    if (SUCCEEDED(result) && ppFactory && *ppFactory)
        HookFactory(static_cast<IUnknown*>(*ppFactory));
    return result;
}

HRESULT STDMETHODCALLTYPE D3D12Hook::CreateDXGIFactory1Hook(REFIID riid, void** ppFactory)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(REFIID, void**);
    const HRESULT result = reinterpret_cast<Fn>(m_createFactory1.original)(riid, ppFactory);
    if (SUCCEEDED(result) && ppFactory && *ppFactory)
        HookFactory(static_cast<IUnknown*>(*ppFactory));
    return result;
}

HRESULT STDMETHODCALLTYPE D3D12Hook::CreateDXGIFactory2Hook(UINT flags, REFIID riid, void** ppFactory)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(UINT, REFIID, void**);
    const HRESULT result = reinterpret_cast<Fn>(m_createFactory2.original)(flags, riid, ppFactory);
    if (SUCCEEDED(result) && ppFactory && *ppFactory)
        HookFactory(static_cast<IUnknown*>(*ppFactory));
    return result;
}

HRESULT STDMETHODCALLTYPE D3D12Hook::CreateSwapChainHook(IUnknown* device, void* desc, IDXGISwapChain** swapChain)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, void*, IDXGISwapChain**);
    const HRESULT result = reinterpret_cast<Fn>(m_createSwapChain.original)(device, desc, swapChain);
    OnSwapChainCreated(device, swapChain);
    return result;
}

HRESULT STDMETHODCALLTYPE D3D12Hook::CreateSwapChainForHwndHook(IUnknown* device, HWND hwnd,
    const void* desc, const void* fullscreenDesc, void* output, IDXGISwapChain1** swapChain)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, HWND, const void*, const void*, void*, IDXGISwapChain1**);
    const HRESULT result = reinterpret_cast<Fn>(m_createSwapChainForHwnd.original)(
        device, hwnd, desc, fullscreenDesc, output, swapChain);
    OnSwapChainCreated(device, reinterpret_cast<IDXGISwapChain**>(swapChain));
    return result;
}

HRESULT STDMETHODCALLTYPE D3D12Hook::CreateSwapChainForCoreWindowHook(IUnknown* device, void* window,
    const void* desc, void* output, IDXGISwapChain1** swapChain)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, void*, const void*, void*, IDXGISwapChain1**);
    const HRESULT result = reinterpret_cast<Fn>(m_createSwapChainForCoreWindow.original)(
        device, window, desc, output, swapChain);
    OnSwapChainCreated(device, reinterpret_cast<IDXGISwapChain**>(swapChain));
    return result;
}

HRESULT STDMETHODCALLTYPE D3D12Hook::CreateSwapChainForCompositionHook(IUnknown* device, const void* desc,
    void* canvas, IDXGISwapChain1** swapChain)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, const void*, void*, IDXGISwapChain1**);
    const HRESULT result = reinterpret_cast<Fn>(m_createSwapChainForComposition.original)(
        device, desc, canvas, swapChain);
    OnSwapChainCreated(device, reinterpret_cast<IDXGISwapChain**>(swapChain));
    return result;
}

void STDMETHODCALLTYPE D3D12Hook::ExecuteCommandListsHook(ID3D12CommandQueue* queue, UINT count,
    ID3D12CommandList* const* lists)
{
    CaptureDevice(queue);

    using Fn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    reinterpret_cast<Fn>(m_executeCommandLists.original)(queue, count, lists);
}

HRESULT STDMETHODCALLTYPE D3D12Hook::PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    static UINT frameCounter = 0;
    const bool trace = ++frameCounter <= 12;
    if (trace) Log("Present[%u]: enter", frameCounter);

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    auto callOriginal = [&]() {
        return reinterpret_cast<PresentFn>(m_present.original)(swapChain, syncInterval, flags);
    };

    // Install the queue-discovery hook if we have not yet seen a game queue.
    if (!m_executeCommandLists.installed)
    {
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&queue))) && queue)
        {
            CaptureDevice(queue.Get());
            EnsureExecuteHook(queue.Get());
        }
        else if (!m_probeQueue)
        {
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device)
            {
                D3D12_COMMAND_QUEUE_DESC queueDesc = {};
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
                {
                    m_probeQueue = queue;
                    m_device = device;
                    EnsureExecuteHook(queue.Get());
                    Log("Probe queue created; waiting for the game queue before drawing overlay");
                }
            }
        }
    }

    // Never render through our own queue: writing back-buffer transitions on a second
    // queue causes DXGI_ERROR_DEVICE_REMOVED in this game.
    if (!m_queue && m_gameQueue)
    {
        m_queue = m_gameQueue;
        Log("Using game command queue for overlay");
    }

    if (!m_device && m_queue)
    {
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(m_queue->GetDevice(IID_PPV_ARGS(&device))) && device)
            m_device = device;
    }

    if (!m_queue || !m_device)
        return callOriginal();

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
        swapChain3 = static_cast<IDXGISwapChain3*>(swapChain); // ABI-compatible inheritance

    if (m_lastSwapChain != swapChain)
    {
        g_renderer.Shutdown();
        m_lastSwapChain = swapChain;
    }

    if (!g_renderer.Ready() && swapChain3)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        HWND hwnd = swapChain3->GetDesc(&desc) == S_OK ? desc.OutputWindow : nullptr;
        g_renderer.Init(m_device.Get(), m_queue.Get(), swapChain3.Get(), hwnd);
    }

    if (g_renderer.Ready())
    {
        if (trace) Log("Present[%u]: BeginFrame", frameCounter);
        g_renderer.BeginFrame();
        if (trace) Log("Present[%u]: Menu::Draw", frameCounter);
        Menu::Draw();
        if (trace) Log("Present[%u]: EndFrame", frameCounter);
        g_renderer.EndFrame();
        if (trace) Log("Present[%u]: overlay submitted", frameCounter);
    }

    if (trace) Log("Present[%u]: calling original", frameCounter);
    const HRESULT result = callOriginal();
    if (trace) Log("Present[%u]: original returned 0x%08X", frameCounter, static_cast<unsigned>(result));
    return result;
}

void D3D12Hook::Remove()
{
    if (!m_installed) return;
    g_renderer.Shutdown();

    m_createFactory.Disable();
    m_createFactory1.Disable();
    m_createFactory2.Disable();
    m_createSwapChain.Disable();
    m_createSwapChainForHwnd.Disable();
    m_createSwapChainForCoreWindow.Disable();
    m_createSwapChainForComposition.Disable();
    m_present.Disable();
    m_executeCommandLists.Disable();

    MH_Uninitialize();
    m_device.Reset();
    m_queue.Reset();
    m_gameQueue.Reset();
    m_probeQueue.Reset();
    m_factoryVTable = m_swapChainVTable = m_lastSwapChain = nullptr;
    m_installed = false;
    Log("D3D12Hook removed.");
}
