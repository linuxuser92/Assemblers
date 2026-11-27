#include "Renderer.h"
#include "config/Config.h"
#include "Game.h"
#include "menu/Menu.h"

#include <imgui_impl_dx12.h>

#include <algorithm>
#include <cmath>

Renderer g_renderer;

bool Renderer::Init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapChain, HWND hwnd)
{
    if (m_ready) return true;
    if (!device || !queue || !swapChain) return false;

    Log("Renderer::Init called (device=%p queue=%p swap=%p hwnd=%p)",
        device, queue, swapChain, hwnd);

    // Dear ImGui requires an explicit context before GetIO()/backend init.
    if (!ImGui::GetCurrentContext())
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // avoid stale off-screen window positions
        Log("Renderer: ImGui context created");
    }

    m_device = device;
    m_queue = queue;
    m_swapChain = swapChain;

    DXGI_SWAP_CHAIN_DESC desc = {};
    HRESULT hr = swapChain->GetDesc(&desc);
    if (FAILED(hr))
    {
        Log("Renderer: GetDesc failed (hr=0x%08X)", static_cast<unsigned>(hr));
        return false;
    }

    m_bufferCount = (std::max)(1U, desc.BufferCount);
    m_hwnd = hwnd ? hwnd : desc.OutputWindow;
    Log("Renderer: desc buffers=%u size=%ux%u hwnd=%p", m_bufferCount,
        desc.BufferDesc.Width, desc.BufferDesc.Height, m_hwnd);

    ComPtr<ID3D12Resource> backBuffer;
    hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr))
    {
        Log("Renderer: GetBuffer(0) failed (hr=0x%08X)", static_cast<unsigned>(hr));
        return false;
    }
    m_rtvFormat = backBuffer->GetDesc().Format;
    Log("Renderer: back buffer format=%d", static_cast<int>(m_rtvFormat));

    // Use a fixed-size RTV heap. The game may resize the swap chain and change the
    // buffer count, so never size this from the initial description alone.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kMaxSwapChainBuffers;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap))))
    {
        Log("Renderer: failed to create RTV heap");
        return false;
    }
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    Log("Renderer: RTV heap ready");

    // Shader-visible SRV heap for ImGui's font texture.
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap))))
    {
        Log("Renderer: failed to create SRV heap");
        return false;
    }
    Log("Renderer: SRV heap ready");

    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(),
                                           nullptr, IID_PPV_ARGS(&m_commandList))) ||
        FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
    {
        Log("Renderer: failed to create command list/fence");
        return false;
    }
    m_commandList->Close();
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
    {
        Log("Renderer: failed to create fence event");
        return false;
    }
    Log("Renderer: command list/fence ready");

    // Back buffers are acquired per frame in EndFrame. We deliberately do not keep
    // references to them here, because that blocks the game's ResizeBuffers call.
    m_backBufferPtrs.assign(kMaxSwapChainBuffers, nullptr);
    m_rtvCreated.assign(kMaxSwapChainBuffers, 0);
    m_width = desc.BufferDesc.Width;
    m_height = desc.BufferDesc.Height;
    Log("Renderer: render target slots ready");

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(static_cast<float>(desc.BufferDesc.Width),
                            static_cast<float>(desc.BufferDesc.Height));
    io.DeltaTime = 1.0f / 60.0f;

    // Mouse input uses read-only window queries plus ImGui's event queue. We never
    // subclass the game window or write ImGui's legacy MouseDown/MousePos fields.
    m_mouseEnabled = !Config::g_settings.disableImGuiInput;
    Log("ImGui mouse input %s (event API; no window subclassing)",
        m_mouseEnabled ? "enabled" : "disabled");

#if IMGUI_VERSION_NUM >= 19150
    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_queue.Get();
    initInfo.NumFramesInFlight = static_cast<int>(m_bufferCount);
    initInfo.RTVFormat = m_rtvFormat;
    initInfo.SrvDescriptorHeap = m_srvHeap.Get();
    initInfo.LegacySingleSrvCpuDescriptor = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    initInfo.LegacySingleSrvGpuDescriptor = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    m_imguiInitialized = ImGui_ImplDX12_Init(&initInfo);
#else
    ImGui_ImplDX12_Init(m_device.Get(), static_cast<int>(m_bufferCount), m_rtvFormat,
                        m_srvHeap.Get(),
                        m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                        m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_imguiInitialized = true;
#endif
    if (!m_imguiInitialized)
    {
        Log("Renderer: ImGui D3D12 init failed");
        return false;
    }

    m_ready = true;

    RECT rect = {};
    if (m_hwnd && GetClientRect(m_hwnd, &rect))
        g_game.SetViewport(static_cast<float>(rect.right - rect.left),
                           static_cast<float>(rect.bottom - rect.top));
    else
        g_game.SetViewport(static_cast<float>(desc.BufferDesc.Width),
                           static_cast<float>(desc.BufferDesc.Height));

    Log("Renderer ready: %u buffers, format %d, hwnd %p", m_bufferCount,
        static_cast<int>(m_rtvFormat), m_hwnd);
    return true;
}

bool Renderer::EnsureRenderTarget(UINT index, ID3D12Resource* buffer)
{
    if (!buffer || index >= kMaxSwapChainBuffers || !m_device || !m_rtvHeap) return false;
    if (m_rtvCreated[index] && m_backBufferPtrs[index] == buffer) return true;

    // Only happens on init/resize. Wait for our previous overlay commands so we do
    // not overwrite a descriptor the GPU may still be reading.
    WaitForGpu();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * m_rtvDescriptorSize;
    m_device->CreateRenderTargetView(buffer, nullptr, rtv);
    m_backBufferPtrs[index] = buffer;
    m_rtvCreated[index] = 1;
    return true;
}

void Renderer::SyncSwapChainSize()
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(m_swapChain->GetDesc(&desc))) return;

    if (desc.BufferDesc.Width != m_width || desc.BufferDesc.Height != m_height ||
        desc.BufferCount != m_bufferCount)
    {
        Log("Renderer: swap chain resized to %ux%u (%u buffers); recreating RTVs",
            desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferCount);
        m_width = desc.BufferDesc.Width;
        m_height = desc.BufferDesc.Height;
        m_bufferCount = desc.BufferCount;
        std::fill(m_rtvCreated.begin(), m_rtvCreated.end(), 0);
        std::fill(m_backBufferPtrs.begin(), m_backBufferPtrs.end(), nullptr);
    }

    g_game.SetViewport(static_cast<float>(m_width), static_cast<float>(m_height));
}

namespace
{
    bool ClientSizeMatches(HWND hwnd, const ImVec2& viewport)
    {
        if (!hwnd || !IsWindow(hwnd)) return false;
        RECT rect = {};
        if (!GetClientRect(hwnd, &rect)) return false;

        const float width = static_cast<float>(rect.right - rect.left);
        const float height = static_cast<float>(rect.bottom - rect.top);
        return width > 0.0f && height > 0.0f &&
               std::fabs(width - viewport.x) < 80.0f &&
               std::fabs(height - viewport.y) < 80.0f;
    }
}

HWND Renderer::FindGameWindow() const
{
    const ImVec2 viewport = g_game.Viewport();

    if (ClientSizeMatches(m_hwnd, viewport)) return m_hwnd;

    HWND foreground = GetForegroundWindow();
    if (ClientSizeMatches(foreground, viewport)) return foreground;

    struct Search
    {
        ImVec2 viewport;
        HWND result;
    } search{ viewport, nullptr };

    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* s = reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (!ClientSizeMatches(hwnd, s->viewport)) return TRUE;
        s->result = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));

    return search.result;
}

ImVec2 Renderer::PollMousePosition()
{
    const ImVec2 viewport = g_game.Viewport();
    POINT cursor = {};
    if (!GetCursorPos(&cursor)) return { -FLT_MAX, -FLT_MAX };

    HWND hwnd = FindGameWindow();
    if (hwnd && IsWindow(hwnd))
    {
        POINT client = cursor;
        if (ScreenToClient(hwnd, &client))
        {
            const ImVec2 pos(static_cast<float>(client.x), static_cast<float>(client.y));
            if (pos.x >= -1.0f && pos.y >= -1.0f &&
                pos.x <= viewport.x + 1.0f && pos.y <= viewport.y + 1.0f)
            {
                if (!m_mouseOriginLogged)
                {
                    Log("ImGui mouse: using window client origin (hwnd=%p, pos=%.0f,%.0f)",
                        hwnd, pos.x, pos.y);
                    m_mouseOriginLogged = true;
                }
                return pos;
            }
        }
    }

    const ImVec2 raw(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    if (!m_mouseOriginLogged)
    {
        Log("ImGui mouse: no matching HWND; using screen coords (pos=%.0f,%.0f)",
            raw.x, raw.y);
        m_mouseOriginLogged = true;
    }
    if (raw.x >= -1.0f && raw.y >= -1.0f &&
        raw.x <= viewport.x + 1.0f && raw.y <= viewport.y + 1.0f)
        return raw;
    return { -FLT_MAX, -FLT_MAX };
}

void Renderer::BeginFrame()
{
    if (!m_ready) return;

    SyncSwapChainSize();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = g_game.Viewport();
    io.DeltaTime = 1.0f / 60.0f;

    if (m_mouseEnabled && Menu::AnyWindowOpen())
    {
        const ImVec2 pos = PollMousePosition();
        io.AddMousePosEvent(pos.x, pos.y);

        // Queue only transitions. On the first active frame we seed the previous
        // state so a button already held when the menu opens does not click it.
        static const int vks[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
        for (int i = 0; i < 3; ++i)
        {
            const bool down = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
            if (!m_mouseReady)
            {
                m_mouseDown[i] = down;
            }
            else if (down != m_mouseDown[i])
            {
                io.AddMouseButtonEvent(i, down);
                m_mouseDown[i] = down;
            }
        }
        m_mouseReady = true;
    }
    else
    {
        if (m_mouseReady)
        {
            for (int i = 0; i < 3; ++i)
            {
                if (m_mouseDown[i]) io.AddMouseButtonEvent(i, false);
                m_mouseDown[i] = false;
            }
        }
        m_mouseReady = false;
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
}

void Renderer::WaitForGpu()
{
    if (!m_fence || !m_fenceEvent || m_fenceValue == 0) return;
    if (m_fence->GetCompletedValue() >= m_fenceValue) return;

    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

void Renderer::EndFrame()
{
    if (!m_ready) return;

    // No CPU-side wait on the hot path: overlay commands are submitted to the game
    // queue before the game's own Present. A short wait happens only if the swap
    // chain was resized and a new RTV must replace one the GPU may still be using.

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || !drawData->CmdListsCount) return;

    const UINT frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (frameIndex >= kMaxSwapChainBuffers) return;

    // Acquire the current buffer for this frame only. Persistent references would
    // block the game's ResizeBuffers call when the window is maximized/restored.
    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(frameIndex, IID_PPV_ARGS(&backBuffer))) ||
        !EnsureRenderTarget(frameIndex, backBuffer.Get()))
    {
        Log("Renderer: failed to prepare back buffer %u", frameIndex);
        return;
    }

    if (FAILED(m_allocator->Reset()) ||
        FAILED(m_commandList->Reset(m_allocator.Get(), nullptr)))
    {
        Log("Renderer: command list reset failed");
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frameIndex) * m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(drawData, m_commandList.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(1, lists);

    // The game queue is shared with the game, so the original Present already waits
    // for these commands. Signal only for the next frame's allocator-reuse check;
    // waiting here would stall the render thread and can trip the game's watchdog.
    ++m_fenceValue;
    m_queue->Signal(m_fence.Get(), m_fenceValue);
}

void Renderer::Shutdown()
{
    if (!m_ready) return;

    if (m_imguiInitialized)
        ImGui_ImplDX12_Shutdown();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    m_commandList.Reset();
    m_allocator.Reset();
    m_fence.Reset();
    m_fenceValue = 0;
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
    m_backBufferPtrs.clear();
    m_rtvCreated.clear();
    m_srvHeap.Reset();
    m_rtvHeap.Reset();
    m_swapChain.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_ready = false;
    m_imguiInitialized = false;
    Log("Renderer shut down.");
}
