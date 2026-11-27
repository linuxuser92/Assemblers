#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <vector>

// Handle of this DLL, set in DllMain.
extern HMODULE g_thisModule;

// Set by the window procedure (End key) or the watchdog thread to request a clean unload.
extern volatile bool g_shouldUnload;

// Module handle/base of the game (Minecraft.Windows.exe).
extern HMODULE g_gameModule;
extern uintptr_t g_gameBase;
extern size_t g_gameSize;

void Log(const char* fmt, ...);

// Recent log lines (newest last), for the in-game F9 debug console.
std::vector<std::string> LogLines(size_t maxLines = 200);

std::filesystem::path DllDir();

// Folder we can actually write to from inside the game's AppContainer.
// Falls back from the DLL folder to the Minecraft package's LocalState folder.
std::filesystem::path DataDir();

std::string PathToString(const std::filesystem::path& path);

// Reads a hex or decimal integer from an INI value ("0xABC" or "2748").
uintptr_t ParseInt(const std::string& value);

// Safe-ish readable pointer check. Not perfect, but avoids most crashes from stale offsets.
bool CanRead(const void* ptr, size_t size);
