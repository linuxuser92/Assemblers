#include "common.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <mutex>
#include <psapi.h>

HMODULE g_thisModule = nullptr;
volatile bool g_shouldUnload = false;
HMODULE g_gameModule = nullptr;
uintptr_t g_gameBase = 0;
size_t g_gameSize = 0;

static std::deque<std::string> g_logLines;
static std::mutex g_logMutex;

static std::filesystem::path GetDllPath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(g_thisModule, buf, MAX_PATH);
    return std::filesystem::path(buf);
}

std::filesystem::path DllDir()
{
    return GetDllPath().parent_path();
}

static bool DirectoryWritable(const std::filesystem::path& directory)
{
    const std::wstring probe = (directory / L"BedrockMenu.write_test").wstring();
    HANDLE handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    DeleteFileW(probe.c_str());
    return true;
}

std::filesystem::path DataDir()
{
    static const std::filesystem::path result = []() -> std::filesystem::path {
        if (DirectoryWritable(DllDir())) return DllDir();

        wchar_t localAppData[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) != 0)
        {
            const std::filesystem::path packages = std::filesystem::path(localAppData) / L"Packages";
            std::error_code ec;
            for (std::filesystem::directory_iterator it(packages, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;
                const std::wstring name = it->path().filename().wstring();
                const bool isMinecraft =
                    name.rfind(L"Microsoft.MinecraftUWP_", 0) == 0 ||
                    name.rfind(L"Microsoft.MinecraftWindowsBeta_", 0) == 0;
                if (!isMinecraft) continue;

                for (const wchar_t* sub : { L"AC\\LocalState", L"AC\\LocalCache", L"LocalState" })
                {
                    const std::filesystem::path candidate = it->path() / sub;
                    std::filesystem::create_directories(candidate, ec);
                    if (!ec && DirectoryWritable(candidate)) return candidate;
                }
            }
        }

        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        return std::filesystem::path(temp);
    }();
    return result;
}

std::string PathToString(const std::filesystem::path& path)
{
    return path.string();
}

void Log(const char* fmt, ...)
{
    static std::filesystem::path logPath = [] {
        auto p = DataDir() / "BedrockMenu.log";
        std::FILE* f = std::fopen(p.string().c_str(), "w");
        if (f) std::fclose(f);
        return p;
    }();

    char message[1024] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char line[1100] = {};
    std::snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s\n",
                    st.wHour, st.wMinute, st.wSecond, message);

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::string entry(line);
        while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r'))
            entry.pop_back();
        g_logLines.push_back(std::move(entry));
        if (g_logLines.size() > 256) g_logLines.pop_front();
    }

    OutputDebugStringA(line);

    // Echo to the debug console created in main.cpp (no-op if there is none).
    std::printf("%s", line);
    std::fflush(stdout);

    if (std::FILE* f = std::fopen(logPath.string().c_str(), "a"))
    {
        std::fwrite(line, 1, std::strlen(line), f);
        std::fclose(f);
    }
}

std::vector<std::string> LogLines(size_t maxLines)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    const size_t count = maxLines < g_logLines.size() ? maxLines : g_logLines.size();
    return std::vector<std::string>(g_logLines.end() - count, g_logLines.end());
}

uintptr_t ParseInt(const std::string& value)
{
    if (value.empty()) return 0;
    try
    {
        size_t consumed = 0;
        unsigned long long result = std::stoull(value, &consumed, 0); // 0 = auto base (0x prefix)
        return static_cast<uintptr_t>(result);
    }
    catch (...)
    {
        return 0;
    }
}

bool CanRead(const void* ptr, size_t size)
{
    if (!ptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & readable) != 0 &&
           reinterpret_cast<uintptr_t>(ptr) + size <=
           reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
