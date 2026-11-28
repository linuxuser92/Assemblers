# Assemblers: Minecraft Mod menu for debug overlay

A **single-player-only** mod menu / debug overlay for **Minecraft Bedrock Edition (Windows)**.
It hooks D3D12 through MinHook, renders its UI with Dear ImGui (D3D12 backend), and draws ESP
directly through the game's command queue.

> ⚠️ Use this only in your own single-player worlds. Do not use it on Realms, servers, or anyone
> else's session. Modifying a commercial game may violate its EULA/terms — you are responsible
> for how you use it. Offsets change **every game version**; this project ships no hardcoded
> game offsets.

---

## Known bugs & warnings

This is experimental software. Read this before reporting an issue:

- **Single-player only.** Never use it on Realms, servers, or anyone else's session.
- **Offsets are version-specific and not bundled.** Every game update changes them. A feature
  showing `MISSING` in the menu stays disabled until you fill in the matching `[Addresses]` /
  `[Offsets]` values or run **Auto-detect** successfully.
- **Fly uses `Offsets.EntityVelocity` by default.** When it is missing, enabling Fly starts a
  velocity scan that works from any menu column: **release `Space`, then press and hold it once**.
  The scan now keeps zero-velocity candidates (the old filter discarded them), validates both
  samples, rejects impossible deltas, and weights vertical change, so a normal jump identifies the
  offset. On a fresh launch it retries quietly until the player is ready instead of wasting the
  prompt with a 0-candidate scan. If velocity genuinely cannot be found,
  **Detect position (fallback)** in the Offsets window can set `EntityPosition`: the mod then
  re-asserts your Y after each tick, **hold `Space` to climb, release it to hover**. The
  configured `FlySpeed` magnitude is used, so a negative value still climbs. Position detection
  no longer guesses by vector size — it must match a real player candidate or your entered
  coordinates, avoiding bogus offsets such as `0x1A0` with `Y=0`. Check the `F9` log for
  `AutoDetect:` lines if detection fails.
- **ESP requires several values:** `LocalPlayer`, `Level` (or `Patterns.Level`),
  `LevelEntityList`, `EntityPosition`, `Camera` (or `Patterns.Camera`), and `ViewProj`. Missing ones
  are marked `MISSING` in the ESP tab, and the HUD shows `unavailable` for coordinates/speed.
- **Auto-detect is a heuristic read-only scan.** False positives are possible; use **Filter by
  coordinates** with your real X/Y/Z when you can. In Bedrock, coordinates are at
  **Settings → World → Game → World options → Show coordinates** — not under Video.
- **Do not re-enable WndProc subclassing or direct `io.MousePos` / `io.MouseDown` writes.** Both
  caused `0xc0000409` crashes in `ucrtbase.dll` during live testing. Mouse input uses Win32 queries
  plus `AddMousePosEvent` / `AddMouseButtonEvent` instead.
- **If crashes ever return after resize/maximize**, set `DisableD3DHooks=1` in `BedrockMenu.ini` to
  shut the overlay off immediately. Renderer must never retain `ComPtr` references to swap-chain
  back buffers.
- **Config flag names can look inverted:** `DisableD3DHooks=1` means hooks are off;
  `DisableImGuiInput=1` means keyboard-only mode.
- **`ShowConsole=0` is the default** because `AllocConsole()` is unstable in the UWP process. Use
  `F9` for the in-game log console instead.
- **Synthetic keystrokes/clicks from another process cannot control the game.** Hotkeys, the
  velocity scan, and menu input need real physical input.

---

## Features

| Tab | Feature | Notes |
|-----|---------|-------|
| ESP | Boxes, names, distance, snaplines, health bars, player/entity filters, distance sorting, radar | Reads entities from the level's entity list |
| Movement | Fly, speed multiplier, step height, no fall damage | Applied in a hooked tick, or from the render loop when no Tick RVA is set |
| World | Fullbright, set time, coordinates / HUD display | Fullbright patches a gamma float if an address is configured |
| Settings | Config save/load, unload DLL, live log console | `Insert` menu, `F1`-`F5` toggles, `F9` log, `End` unload, `Ctrl+Arrows` move |

---

## Dependencies

The required sources are vendored in `deps/imgui` and `deps/MinHook`, so the project builds
without extra setup. To update them later:

```powershell
git clone https://github.com/ocornut/imgui.git deps/imgui
git clone https://github.com/TsudaKageyu/minhook.git deps/MinHook
```

Use an ImGui release that has `backends/imgui_impl_dx12.cpp` and the **new**
`ImGui_ImplDX12_Init(device, numFrames, format, srvHeap, cpuHandle, gpuHandle)` signature
(1.89+ recommended). If you use an older ImGui, adapt `Renderer.cpp` accordingly.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Substitute your installed VS generator, e.g. `"Visual Studio 17 2022"`.
Output: `build/Release/BedrockMenu.dll` (x64 only — the game is x64).

The included `deps/imgui` and `deps/MinHook` were compile-tested with MSVC 19.52 /
Windows SDK 10.0.26100. Both the legacy and the new `ImGui_ImplDX12_InitInfo` ImGui APIs are
supported.

## Injection

Bedrock (Windows) is a packaged UWP/GDK-style app. Generic injectors fail because the game's
AppContainer cannot read an arbitrary DLL path. There are two supported ways in.

### Preferred: LeviLauncher `preload-native`

LeviLauncher loads native DLLs from the game's import table before DXGI starts, which is exactly
what the hooks need. From **64-bit PowerShell**:

```powershell
$launcher = "$env:USERPROFILE\Downloads\LeviLauncher.exe"
$dll = "$PWD\build\Release\BedrockMenu.dll"
$instance = "My Instance"

& $launcher stop $instance -y
& $launcher mod import $dll -i $instance -y
# Put BedrockMenu.ini in the new mod folder, then:
& $launcher play $instance
```

The mod folder is:

```
%LOCALAPPDATA%\LeviLauncher\Instances\<instance>\mods\BedrockMenu
```

`BedrockMenu.log` and `BedrockMenu.ini` live next to the DLL there.

### Fallback: in-repo UWP injector

`tools\Inject-BedrockMenu.ps1` copies the DLL into the package's own writable folder, grants
`ALL APPLICATION PACKAGES` (`S-1-15-2-1`) read/execute, then calls the compiled
`BedrockInjector.exe` helper to inject with `CreateRemoteThread(LoadLibraryW)` and verify the
module loaded. No PowerShell `Add-Type` is used, so antivirus products that block dynamic
P/Invoke compilation will not break it. Build the helper with
`cmake --build build --target BedrockInjector`.

The overlay submits its draw commands on the **game's** D3D12 command queue. The hook briefly
patches the shared `ExecuteCommandLists` vtable slot only to discover that queue, then forwards
calls immediately. Rendering through a second queue caused `DXGI_ERROR_DEVICE_REMOVED` and a game
crash, so do not change that behavior. If you ever need to disable the overlay temporarily, set
`DisableD3DHooks=1` in `BedrockMenu.ini`.

### HUD & notifications

The in-game HUD shows FPS, local coordinates, movement speed, and feature status. Configure it in
**Settings** (enable/disable each line and set `HUD X` / `HUD Y`), or in the `[HUD]` section of
`BedrockMenu.ini`. Toast notifications appear briefly in the top-right when you use F1–F5 or run
auto-setup. Both can be turned off independently.

### Menu layout

The menu is split into three independent windows: **ESP**, **Movement**, and **World**. Each one
can be dragged and resized separately and closed with its own title-bar **X**. The small buttons at
the top of each window reopen a closed section, including **Offsets** and **Settings**. Related
controls are paired inside each window (for example requirement statuses and ESP toggles).

The **ESP** column also has `Health bars`, `Show players`, `Show entities`, `Sort by distance`, and
`Radar` controls. The radar appears in the bottom-right corner and plots nearby entities around
your position; `Radar range` sets how many blocks it covers. Health bars require `EntityHealth`,
which can be detected with **Detect health** in the Offsets window or automatically during
**One-click full setup**. Player/entity classification requires `PlayerVTable`; without it all
entities are treated as generic entities.

### Hotkeys / debug console

- `Insert` — toggle the menu; reopening it restores the three feature windows
- `F1` — toggle Fly
- `F2` — toggle Speed (remembers your last multiplier)
- `F3` — toggle ESP
- `F4` — toggle Fullbright
- `F5` — toggle Coordinates
- `F9` — toggle the in-game debug console (last 200 `Log()` lines, live, and closes it again)
- `End` — unload the DLL
- Drag a window title bar — move that individual window
- `Ctrl` + `Shift` + arrow keys — move the debug console window

With `DisableImGuiInput=0` (the default), the menu supports normal mouse dragging and the
title-bar **X** button. Mouse state is read with `GetCursorPos`/`GetAsyncKeyState` and submitted
through ImGui's `AddMousePosEvent`/`AddMouseButtonEvent` API — no WndProc subclassing and no writes
to ImGui's legacy `io.MouseDown`/`io.MousePos` fields. With `DisableImGuiInput=1`, the UI is
keyboard-only; `Insert`, `F9`, `End`, and the `Ctrl+Shift`-arrow debug-console mover still work.

The optional Windows console is still available with `ShowConsole=1`, but it is off by default
because `AllocConsole()` inside UWP can be unstable.

```powershell
# Confirm injection first — all hooks disabled:
powershell -ExecutionPolicy Bypass -File tools\Inject-BedrockMenu.ps1 -ResetConfig

# Inject early with overlay enabled:
powershell -ExecutionPolicy Bypass -File tools\Inject-BedrockMenu.ps1 -WaitForProcess -WaitSeconds 120 -EnableOverlay
```

Start Minecraft while `-WaitForProcess` is waiting. Avoid `-LateHook` unless injecting into an
already-running game; the dummy-swapchain fallback is less stable.

`BedrockMenu.log` and `BedrockMenu.ini` are written to the DLL's folder. The DLL runs **inside**
the game's AppContainer — it does not bypass it.

### AppContainer reality check

This project does **not** escape or disable AppContainer. Once injected, the DLL runs with the
game's token and is subject to the same restrictions: no arbitrary file paths, no registry writes
outside the package, no crossing into other processes unless capabilities allow it. What *does*
work is reading/writing the game's own memory, hooking DXGI/D3D12 in-process, subclassing the
game window, and using package-writable storage. The injection itself (getting a DLL loaded into
a UWP process) is a separate problem that this DLL does not solve.

## Configuration: offsets & patterns

Everything version-specific lives in `BedrockMenu.ini` (created next to the DLL on first run).
You can also edit all values in the in-game **Offsets** tab: use **Save & apply** to write the INI
and reload them live (the tick hook is refreshed automatically). The same tab has an **Auto-detect**
section: enable Minecraft's coordinate display, enter your current X/Y/Z, then click **One-click
full setup**. It finds `LocalPlayer`, auto-detects a matching `EntityPosition` independently of ESP
success, applies every ESP offset it can (`Level`, `LevelEntityList`, `EntityListStride`, `Camera`,
`ViewProj`, `MatrixTransposed`), and always prompts you to release then press `Space` once when
`EntityVelocity` is still missing. Everything is applied automatically.
The individual **Scan for LocalPlayer**, **Detect position (fallback)**, **Detect all ESP offsets**, and
**Detect velocity** buttons remain available if you want to run the steps manually or pick a
different candidate. Enabling Fly with `EntityVelocity` missing starts the velocity scan first;
position detection is available as a fallback but is no longer guessed from arbitrary large vectors.
All addresses are **RVAs relative to the Minecraft module base** — ASLR is handled automatically.
If a value is `0`, that feature is simply disabled. The menu now shows green `OK` / red `MISSING`
statuses for each requirement, so you can see exactly what is holding a feature back.

| Feature | Required INI values |
| --- | --- |
| Fly / speed | `Addresses.LocalPlayer`, `Offsets.EntityVelocity` (or `Offsets.EntityPosition` for fly only) |
| No fall damage | `Addresses.LocalPlayer`, `Offsets.EntityOnGround` |
| ESP | `Addresses.LocalPlayer`, `Addresses.Level` (or `Patterns.Level`), `Offsets.LevelEntityList`, `Offsets.EntityPosition`, `Patterns.Camera` (or `Addresses.Camera`), `Offsets.ViewProj` |
| ESP health bars | `Offsets.EntityHealth` (optional — bars are hidden when missing) |
| Fullbright | `Addresses.Gamma` |
| Set time | `Addresses.Level` (or `Patterns.Level`), `Offsets.LevelTime` |

`Addresses.Tick` is optional: without it, movement is applied from the render loop instead.

```ini
[HUD]
Enabled=1
ShowFps=1
ShowCoords=1
ShowSpeed=1
ShowStatus=1
X=12
Y=12
Notifications=1

[Addresses]
; RVA of a function called once per frame/tick with (this, deltaTime)
Tick=0x0
; RVA of a pointer to the local player (e.g. `mov rax, [module+XXX]`)
LocalPlayer=0x0
; RVA of the gamma/float used for brightness (optional)
Gamma=0x0
; RVA of a pointer to Level* (alternative to Patterns.Level)
Level=0x0
; RVA of a pointer to the camera/render context (alternative to Patterns.Camera)
Camera=0x0

[Offsets]
; Offsets from the Level object
LevelEntityList=0x0
LevelTime=0x0
; Offsets from the Player/Entity object
EntityPosition=0x0
EntityVelocity=0x0     ; Vec3, used by fly/speed
EntityOnGround=0x0     ; bool, used by no-fall-damage
EntityHealth=0x0       ; float, optional ESP health bars
EntityName=0x0
PlayerVTable=0x0       ; value of the player vtable pointer RVA (for filtering)
EntityListStride=8
; Offsets from the LevelRenderContext / camera holder
ViewProj=0x0
MatrixTransposed=0

[Patterns]
; Byte signatures for finding pointers when RVAs aren't known.
; Format: "48 8B 05 ?? ?? ?? ??" (?? = wildcard), result = RVA of the
; instruction start; use Addresses=... if you already know it.
Level=
Camera=
```

### How to find them

1. Attach x64dbg, search for the string `"level"` / `"Minecraft"` or use a known pattern, and
   find a global that holds `Level*`. Put the **RVA** in `[Addresses]`.
2. In the entity list, look at Level + N: it's usually a `std::vector<Entity*>` or an array of
   pointers. Record `LevelEntityList` and `EntityListStride`.
3. Find the player position by using a pointer scanner while moving in-game; record
   `EntityPosition` (a `Vec3`, 3 floats).
4. The view-projection matrix is typically in a render context / camera struct. Record its
   offset and set `MatrixTransposed=1` if boxes don't line up (mirrored/Y-flipped).
5. Optional: find a function called every frame (breakpoint on movement update) and put its RVA in
   `Tick`. The detour assumes the signature `void __fastcall Tick(void* self, float deltaTime)` —
   if the real function differs, adjust `TickHook` in `src/features/GameHook.cpp`. Without `Tick`,
   fly/speed are applied from the render loop instead, so ESP/UI and movement still work.

The code is deliberately defensive: unset offsets are skipped and logged rather than crashing.

## Source layout

```
src/main.cpp              DllMain, init/unload thread, console + log
src/common.*              logging, module paths, shared includes
src/config/Config.*       INI-backed settings
src/utils/Memory.*        pattern scanning, RIP-relative resolution
src/sdk/Offsets.*         version-specific addresses/offsets (INI)
src/sdk/Game.*            entity list, camera, world-to-screen projection
src/render/Renderer.*     ImGui + D3D12 device/RTV/SRV, command list
src/render/D3D12Hook.*    MinHook vtable hooks: SwapChain::Present,
                          CommandQueue::ExecuteCommandLists
src/features/Features.*   movement/world feature application
src/menu/Menu.*           ImGui UI + ESP drawing
```

## Notes on the D3D12 hook

* `IDXGISwapChain::Present` is vtable slot **8**; `ID3D12CommandQueue::ExecuteCommandLists`
  is slot **10**.
* `ExecuteCommandLists` captures the game's device + command queue; `Present` captures the
  swap chain, HWND, back-buffer format and frame index.
* Our RTV heap creates render targets on the game's own swap-chain buffers. We transition the
  back buffer `PRESENT -> RENDER_TARGET -> PRESENT`, draw ImGui, and submit on the game's queue
  immediately before the original `Present` — queue ordering guarantees the overlay lands in
  the same frame. Back buffers are borrowed per frame (never retained), so maximizing or
  resizing the window can reallocate them safely; the RTVs are recreated on resize.
* Input never subclasses the game HWND. Mouse position/buttons are read with read-only Win32
  queries and queued via ImGui's event API; keyboard hotkeys are polled with `GetAsyncKeyState`.
