#pragma once
#include "common.h"

#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class Renderer
{
public:
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapChain, HWND hwnd);
    void Shutdown();

    void BeginFrame();
    void EndFrame();

    bool Ready() const { return m_ready; }
    ID3D12Device* Device() const { return m_device.Get(); }

private:
    static constexpr UINT kMaxSwapChainBuffers = 16;

    void WaitForGpu();
    bool EnsureRenderTarget(UINT index, ID3D12Resource* buffer);
    void SyncSwapChainSize();
    HWND FindGameWindow() const;
    ImVec2 PollMousePosition();

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;

    // Raw pointers only: holding ComPtr refs to swap-chain buffers prevents ResizeBuffers.
    std::vector<ID3D12Resource*> m_backBufferPtrs;
    std::vector<unsigned char> m_rtvCreated;

    UINT m_rtvDescriptorSize = 0;
    UINT m_bufferCount = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    DXGI_FORMAT m_rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    HWND m_hwnd = nullptr;
    bool m_ready = false;
    bool m_imguiInitialized = false;
    bool m_mouseEnabled = false;
    bool m_mouseReady = false;
    bool m_mouseDown[3] = {};
    bool m_mouseOriginLogged = false;
};

extern Renderer g_renderer;
