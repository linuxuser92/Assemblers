#pragma once
#include "common.h"

#include <wrl/client.h>

class D3D12Hook
{
public:
    // Installs MinHook patches on DXGI exports and COM vtables.
    static bool Install();
    static void Remove();

    // Late-injection fallback: discover DXGI's swapchain vtable with a throwaway swapchain.
    static bool InstallLatePresentHook();

    static bool HasDevice() { return m_device != nullptr; }
    static bool HasPresentHook() { return m_present.installed; }

private:
    struct Hook
    {
        void* target = nullptr;
        void* detour = nullptr;
        void* original = nullptr;
        bool installed = false;

        bool Create(void* t, void* d);
        bool Enable();
        void Disable();
    };

    static HRESULT STDMETHODCALLTYPE CreateDXGIFactoryHook(REFIID riid, void** ppFactory);
    static HRESULT STDMETHODCALLTYPE CreateDXGIFactory1Hook(REFIID riid, void** ppFactory);
    static HRESULT STDMETHODCALLTYPE CreateDXGIFactory2Hook(UINT flags, REFIID riid, void** ppFactory);
    static HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IUnknown* device, void* desc, IDXGISwapChain** swapChain);
    static HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IUnknown* device, HWND hwnd,
        const void* desc, const void* fullscreenDesc, void* output, IDXGISwapChain1** swapChain);
    static HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(IUnknown* device, void* window,
        const void* desc, void* output, IDXGISwapChain1** swapChain);
    static HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(IUnknown* device, const void* desc,
        void* canvas, IDXGISwapChain1** swapChain);
    static HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

    // Only used to discover the game's queue; the detour forwards immediately.
    static void STDMETHODCALLTYPE ExecuteCommandListsHook(ID3D12CommandQueue* queue, UINT count,
        ID3D12CommandList* const* lists);
    static bool EnsureExecuteHook(ID3D12CommandQueue* queue);

    static bool HookFactory(IUnknown* factory);
    static bool HookSwapChain(IDXGISwapChain* swapChain);
    static void OnSwapChainCreated(IUnknown* device, IDXGISwapChain** swapChain);
    static void CaptureDevice(ID3D12CommandQueue* queue);

    static Hook m_createFactory;
    static Hook m_createFactory1;
    static Hook m_createFactory2;
    static Hook m_createSwapChain;
    static Hook m_createSwapChainForHwnd;
    static Hook m_createSwapChainForCoreWindow;
    static Hook m_createSwapChainForComposition;
    static Hook m_present;
    static Hook m_executeCommandLists;

    static void* m_factoryVTable;
    static void* m_swapChainVTable;
    static void* m_lastSwapChain;

    static Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    static Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;       // queue the renderer submits to
    static Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_gameQueue;   // captured from the game
    static Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_probeQueue;  // temporary queue, hook installation only
    static bool m_installed;
};
