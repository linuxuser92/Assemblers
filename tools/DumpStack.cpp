// Minimal native minidump stack printer for debugging BedrockMenu crashes.
// Usage: DumpStack.exe <dump.dmp> [symbol-search-path]
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

struct LoadedModule
{
    ULONG64 base;
    ULONG32 size;
    std::wstring name;
};
static std::vector<LoadedModule> g_modules;

static BYTE* g_dump = nullptr;
static const MINIDUMP_THREAD* g_targetThread = nullptr;

static const LoadedModule* ModuleForAddress(ULONG64 address)
{
    for (const auto& module : g_modules)
        if (address >= module.base && address < module.base + module.size) return &module;
    return nullptr;
}

static std::wstring DumpString(ULONG32 rva)
{
    if (!g_dump || rva == 0) return L"";
    const auto* str = reinterpret_cast<const MINIDUMP_STRING*>(g_dump + rva);
    const size_t chars = str->Length / sizeof(WCHAR);
    return std::wstring(str->Buffer, chars);
}

static std::string Narrow(const std::wstring& value)
{
    const int size = WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? static_cast<size_t>(size) : 0, '\0');
    if (size > 0)
        WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()),
                            &result[0], size, nullptr, nullptr);
    return result;
}

static BOOL CALLBACK ReadDumpMemory(HANDLE, ULONG64 address, PVOID buffer, DWORD size,
                                    PDWORD bytesRead)
{
    *bytesRead = 0;

    auto tryRange = [&](const MINIDUMP_MEMORY_DESCRIPTOR& range) -> bool {
        const ULONG64 start = range.StartOfMemoryRange;
        const ULONG64 end = start + range.Memory.DataSize;
        if (address < start || address >= end) return false;
        const ULONG64 offset = address - start;
        const DWORD available = static_cast<DWORD>(range.Memory.DataSize - offset);
        const DWORD copySize = (size < available) ? size : available;
        std::memcpy(buffer, g_dump + range.Memory.Rva + offset, copySize);
        *bytesRead = copySize;
        return true;
    };

    if (g_targetThread && tryRange(g_targetThread->Stack)) return TRUE;

    MINIDUMP_DIRECTORY* dir = nullptr;
    PVOID stream = nullptr;
    ULONG streamSize = 0;
    if (!MiniDumpReadDumpStream(g_dump, MemoryListStream, &dir, &stream, &streamSize)) return FALSE;
    if (!stream || streamSize < sizeof(ULONG32)) return FALSE;

    auto* list = static_cast<MINIDUMP_MEMORY_LIST*>(stream);
    for (ULONG32 i = 0; i < list->NumberOfMemoryRanges; ++i)
        if (tryRange(list->MemoryRanges[i])) return TRUE;
    return FALSE;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("Usage: DumpStack.exe <dump.dmp> [symbol-search-path]\n");
        return 1;
    }

    HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        std::printf("CreateFile failed: %lu\n", GetLastError());
        return 1;
    }
    HANDLE mapping = CreateFileMapping(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping)
    {
        std::printf("CreateFileMapping failed: %lu\n", GetLastError());
        CloseHandle(file);
        return 1;
    }
    g_dump = static_cast<BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!g_dump)
    {
        std::printf("MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        CloseHandle(file);
        return 1;
    }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    std::string symbolPath = "srv*;https://msdl.microsoft.com/download/symbols";
    if (argc >= 3) symbolPath = std::string(argv[2]) + ";" + symbolPath;
    SymInitialize(GetCurrentProcess(), symbolPath.c_str(), FALSE);

    MINIDUMP_DIRECTORY* dir = nullptr;
    PVOID stream = nullptr;
    ULONG streamSize = 0;

    // Load modules so SymFromAddr can resolve names.
    if (MiniDumpReadDumpStream(g_dump, ModuleListStream, &dir, &stream, &streamSize) && stream)
    {
        auto* modules = static_cast<MINIDUMP_MODULE_LIST*>(stream);
        for (ULONG32 i = 0; i < modules->NumberOfModules; ++i)
        {
            const auto& module = modules->Modules[i];
            const std::wstring name = DumpString(module.ModuleNameRva);
            g_modules.push_back({ module.BaseOfImage, module.SizeOfImage, name });
            const std::string path = Narrow(name);
            SymLoadModuleEx(GetCurrentProcess(), nullptr, path.c_str(), nullptr,
                            module.BaseOfImage, module.SizeOfImage, nullptr, 0);
        }
    }

    DWORD exceptionThreadId = 0;
    ULONG64 exceptionAddress = 0;
    ULONG32 exceptionCode = 0;
    if (MiniDumpReadDumpStream(g_dump, ExceptionStream, &dir, &stream, &streamSize) && stream)
    {
        auto* ex = static_cast<MINIDUMP_EXCEPTION_STREAM*>(stream);
        exceptionThreadId = ex->ThreadId;
        exceptionAddress = ex->ExceptionRecord.ExceptionAddress;
        exceptionCode = ex->ExceptionRecord.ExceptionCode;
        std::printf("Exception: code=0x%08X address=0x%llX thread=%lu data=%llu\n",
                    exceptionCode, static_cast<unsigned long long>(exceptionAddress),
                    exceptionThreadId,
                    static_cast<unsigned long long>(ex->ExceptionRecord.NumberParameters > 0
                        ? ex->ExceptionRecord.ExceptionInformation[0] : 0));
    }

    const MINIDUMP_THREAD* targetThread = nullptr;
    if (MiniDumpReadDumpStream(g_dump, ThreadListStream, &dir, &stream, &streamSize) && stream)
    {
        auto* threads = static_cast<MINIDUMP_THREAD_LIST*>(stream);
        for (ULONG32 i = 0; i < threads->NumberOfThreads; ++i)
            if (threads->Threads[i].ThreadId == exceptionThreadId) targetThread = &threads->Threads[i];
    }
    if (!targetThread)
    {
        std::printf("Exception thread not found in ThreadListStream\n");
        return 2;
    }
    g_targetThread = targetThread;

    CONTEXT context = {};
    const DWORD contextSize = targetThread->ThreadContext.DataSize < sizeof(CONTEXT)
        ? targetThread->ThreadContext.DataSize : sizeof(CONTEXT);
    std::memcpy(&context, g_dump + targetThread->ThreadContext.Rva, contextSize);
    context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
    std::printf("Thread context: size=%lu rip=0x%llX rsp=0x%llX rbp=0x%llX stack=%llu bytes @ 0x%llX\n",
                targetThread->ThreadContext.DataSize,
                static_cast<unsigned long long>(context.Rip),
                static_cast<unsigned long long>(context.Rsp),
                static_cast<unsigned long long>(context.Rbp),
                static_cast<unsigned long long>(targetThread->Stack.Memory.DataSize),
                static_cast<unsigned long long>(targetThread->Stack.StartOfMemoryRange));

    if (MiniDumpReadDumpStream(g_dump, MemoryListStream, &dir, &stream, &streamSize) && stream)
    {
        auto* ranges = static_cast<MINIDUMP_MEMORY_LIST*>(stream);
        std::printf("MemoryListStream: %lu ranges\n", ranges->NumberOfMemoryRanges);
        bool containsPc = false;
        for (ULONG32 i = 0; i < ranges->NumberOfMemoryRanges; ++i)
        {
            const auto& range = ranges->MemoryRanges[i];
            if (context.Rip >= range.StartOfMemoryRange &&
                context.Rip < range.StartOfMemoryRange + range.Memory.DataSize)
                containsPc = true;
        }
        std::printf("MemoryList contains PC: %d\n", containsPc ? 1 : 0);
    }

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    std::printf("Stack for thread %lu:\n", exceptionThreadId);
    for (int depth = 0; depth < 80; ++depth)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
                         &frame, &context, ReadDumpMemory, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
        {
            std::printf("StackWalk64 stopped: GetLastError=%lu\n", GetLastError());
            break;
        }

        std::printf("%2d  0x%llX", depth, static_cast<unsigned long long>(frame.AddrPC.Offset));
        if (const LoadedModule* module = ModuleForAddress(frame.AddrPC.Offset))
            std::printf("  [%s+0x%llX]", Narrow(module->name).c_str(),
                        static_cast<unsigned long long>(frame.AddrPC.Offset - module->base));
        char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        ULONG64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol))
            std::printf("  %s+0x%llX", symbol->Name, static_cast<unsigned long long>(displacement));

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), frame.AddrPC.Offset,
                                 &lineDisplacement, &line))
            std::printf("  %s:%lu", line.FileName, line.LineNumber);
        std::printf("\n");
    }

    std::printf("\nRaw stack scan (possible return addresses):\n");
    for (int i = 0; i < 256; ++i)
    {
        ULONG64 value = 0;
        DWORD read = 0;
        if (!ReadDumpMemory(GetCurrentProcess(), context.Rsp + i * 8, &value, 8, &read) || read != 8)
            break;
        if (value < 0x10000) continue;

        char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        ULONG64 displacement = 0;
        if (!SymFromAddr(GetCurrentProcess(), value, &displacement, symbol)) continue;
        std::printf("  +0x%04X  0x%llX", i * 8, static_cast<unsigned long long>(value));
        if (const LoadedModule* module = ModuleForAddress(value))
            std::printf("  %s+0x%llX", Narrow(module->name).c_str(),
                        static_cast<unsigned long long>(value - module->base));
        std::printf("  %s+0x%llX\n", symbol->Name, static_cast<unsigned long long>(displacement));
    }

    SymCleanup(GetCurrentProcess());
    UnmapViewOfFile(g_dump);
    CloseHandle(mapping);
    CloseHandle(file);
    return 0;
}
