#pragma once
// Shared by WHAT and the Qt-free Windows helper. Crash capture is best-effort;
// it uses no Qt objects and never uploads data or records command arguments.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace CrashDiagnostics {
inline wchar_t folder[2048]{};
inline char role[64]{}, version[64]{}, component[64]{};
inline LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
inline volatile LONG capturing = 0;
inline HMODULE dbghelp = nullptr;
inline decltype(&MiniDumpWriteDump) dump = nullptr;
inline DWORD dumpLoadError = ERROR_PROC_NOT_FOUND;
inline decltype(&StackWalk64) walk = nullptr;
inline decltype(&SymFunctionTableAccess64) functions = nullptr;
inline decltype(&SymGetModuleBase64) moduleBase = nullptr;

inline bool enabled(const wchar_t *directory, const wchar_t *flag) {
    wchar_t path[2100];
    if (swprintf_s(path, L"%s\\%s", directory, flag) < 0)
        return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

inline void appendAt(const wchar_t *directory, const char *module, const char *level, const char *message,
                     DWORD thread = GetCurrentThreadId()) {
    if (!enabled(directory, L"logging.enabled"))
        return;
    wchar_t path[2100];
    if (swprintf_s(path, L"%s\\activity.log", directory) < 0)
        return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart < 4 * 1024 * 1024) {
        SYSTEMTIME now;
        GetSystemTime(&now);
        char line[4096];
        int count = snprintf(line, sizeof(line),
                             "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%s][%s][PID %lu][TID %lu][%s] %.3000s\n",
                             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                             now.wMilliseconds, role, module, GetCurrentProcessId(), thread, level, message);
        DWORD written;
        if (count > 0)
            WriteFile(file, line, DWORD(count), &written, nullptr);
    }
    CloseHandle(file);
}

inline void line(HANDLE file, const char *text) {
    DWORD written;
    WriteFile(file, text, DWORD(strlen(text)), &written, nullptr);
}

inline void frame(HANDLE file, DWORD64 address) {
    HMODULE module = nullptr;
    char name[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(address), &module);
    if (module)
        GetModuleFileNameA(module, name, MAX_PATH);
    const auto base = strrchr(name, '\\');
    char text[512];
    snprintf(text, sizeof(text), "  %s+0x%llX (0x%llX)\n", base ? base + 1 : "unknown",
             static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)),
             static_cast<unsigned long long>(address));
    line(file, text);
}

inline void dumpFailure(HANDLE report, const char *operation, DWORD error) {
    char text[256];
    snprintf(text, sizeof(text), "Memory dump: %s failed (0x%08lX)", operation, error);
    if (report != INVALID_HANDLE_VALUE) {
        line(report, text);
        line(report, "\n");
    }
    appendAt(folder, component, "ERROR", text);
}

inline void writeDump(const wchar_t *path, EXCEPTION_POINTERS *exception, HANDLE report) {
    if (!dump) {
        dumpFailure(report, "loading MiniDumpWriteDump", dumpLoadError);
        return;
    }
    HANDLE output = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        dumpFailure(report, "creating dump file", GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
    bool saved = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        saved = dump(GetCurrentProcess(), GetCurrentProcessId(), output, MiniDumpNormal,
                     &info, nullptr, nullptr);
        // DbgHelp reports HRESULTs. Capture the error before any cleanup API
        // overwrites it. Threads or modules disappearing can cause partial reads.
        const DWORD error = saved ? ERROR_SUCCESS : GetLastError();
        if (saved)
            break;
        const bool transient = error == DWORD(HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY))
                            || error == DWORD(HRESULT_FROM_WIN32(ERROR_BAD_LENGTH));
        if (!transient || attempt == 2) {
            dumpFailure(report, "MiniDumpWriteDump", error);
            break;
        }
        // Reuse only an empty file. A failed attempt may have left dump data
        // behind, including a valid header that is not a complete dump.
        if (!SetFilePointerEx(output, {}, nullptr, FILE_BEGIN) || !SetEndOfFile(output)) {
            dumpFailure(report, "resetting incomplete dump", GetLastError());
            break;
        }
        Sleep(50);
    }
    CloseHandle(output);
    if (saved) {
        if (report != INVALID_HANDLE_VALUE)
            line(report, "Memory dump: written\n");
    } else if (!DeleteFileW(path)) {
        dumpFailure(report, "removing incomplete dump", GetLastError());
    }
}

inline LONG WINAPI crash(EXCEPTION_POINTERS *exception) {
    if (exception && exception->ExceptionRecord && enabled(folder, L"logging.enabled") &&
        InterlockedCompareExchange(&capturing, 1, 0) == 0) {
        SYSTEMTIME now;
        GetSystemTime(&now);
        wchar_t stem[2200], path[2240];
        swprintf_s(stem, L"%s\\crash-%04u%02u%02u-%02u%02u%02u-%03u-%lu", folder, now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
        swprintf_s(path, L"%s.txt", stem);
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char text[256];
            snprintf(
                text, sizeof(text),
                "WHAT %s\nProcess: %s\nModule: %s\nPID: %lu\nThread: %lu\nException: 0x%08lX\nFault and stack (module + offset):\n",
                version, role, component, GetCurrentProcessId(), GetCurrentThreadId(),
                exception->ExceptionRecord->ExceptionCode);
            line(file, text);
            frame(file, reinterpret_cast<DWORD64>(exception->ExceptionRecord->ExceptionAddress));
#if defined(_M_X64) || defined(_M_ARM64)
            if (walk && functions && moduleBase && exception->ContextRecord) {
                CONTEXT context = *exception->ContextRecord;
                STACKFRAME64 stack{};
#if defined(_M_ARM64)
                constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
                stack.AddrPC = {context.Pc, 0, AddrModeFlat};
                stack.AddrStack = {context.Sp, 0, AddrModeFlat};
                stack.AddrFrame = {context.Fp, 0, AddrModeFlat};
#else
                constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
                stack.AddrPC = {context.Rip, 0, AddrModeFlat};
                stack.AddrStack = {context.Rsp, 0, AddrModeFlat};
                stack.AddrFrame = {context.Rbp, 0, AddrModeFlat};
#endif
                DWORD64 last = 0;
                for (int i = 0;
                     i < 48 && walk(machine, GetCurrentProcess(), GetCurrentThread(), &stack,
                                    &context, nullptr, functions, moduleBase, nullptr);
                     ++i) {
                    if (!stack.AddrPC.Offset || stack.AddrPC.Offset == last)
                        break;
                    last = stack.AddrPC.Offset;
                    frame(file, last);
                }
            }
#endif
            FlushFileBuffers(file);
        }
        appendAt(folder, component, "ERROR", "Unhandled exception; see crash report");
        if (enabled(folder, L"dumps.enabled")) {
            swprintf_s(path, L"%s.dmp", stem);
            writeDump(path, exception, file);
        }
        if (file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }
    return previous ? previous(exception) : EXCEPTION_CONTINUE_SEARCH;
}

inline void initialize(const wchar_t *directory, const char *process, const char *build,
                       const char *owner = "WHAT") {
    if (wcslen(directory) >= 2048)
        return;
    wcscpy_s(folder, directory);
    strncpy_s(component, owner, _TRUNCATE);
    strcpy_s(role, process);
    strncpy_s(version, build, _TRUNCATE);
    // Keep the latest reports; do this during healthy startup, never in the exception handler.
    if (enabled(folder, L"logging.enabled")) {
        std::wstring pattern = std::wstring(folder) + L"\\crash-*.txt";
        WIN32_FIND_DATAW item{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &item);
        std::vector<std::wstring> reports;
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    reports.emplace_back(item.cFileName);
            } while (FindNextFileW(search, &item));
            FindClose(search);
        }
        std::sort(reports.begin(), reports.end());
        for (size_t i = 0; i + 9 < reports.size(); ++i) {
            auto path = std::wstring(folder) + L"\\" + reports[i];
            DeleteFileW(path.c_str());
            path.replace(path.size() - 4, 4, L".dmp");
            DeleteFileW(path.c_str());
        }
    }
    capturing = 0;
    // Load only the Windows copy; do not search the current directory.
    if (!dbghelp)
        dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbghelp)
        dumpLoadError = GetLastError();
    if (dbghelp) {
        dump = reinterpret_cast<decltype(dump)>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        dumpLoadError = dump ? ERROR_SUCCESS : GetLastError();
        walk = reinterpret_cast<decltype(walk)>(GetProcAddress(dbghelp, "StackWalk64"));
        functions =
            reinterpret_cast<decltype(functions)>(GetProcAddress(dbghelp, "SymFunctionTableAccess64"));
        moduleBase = reinterpret_cast<decltype(moduleBase)>(GetProcAddress(dbghelp, "SymGetModuleBase64"));
        auto init = reinterpret_cast<decltype(&SymInitialize)>(GetProcAddress(dbghelp, "SymInitialize"));
        auto options = reinterpret_cast<decltype(&SymSetOptions)>(GetProcAddress(dbghelp, "SymSetOptions"));
        if (options)
            options(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS |
                    SYMOPT_IGNORE_NT_SYMPATH);
        if (init)
            init(GetCurrentProcess(), "", TRUE);
    }
    previous = SetUnhandledExceptionFilter(crash);
}

inline void restore() {
    SetUnhandledExceptionFilter(previous);
    if (dbghelp) {
        auto cleanup = reinterpret_cast<decltype(&SymCleanup)>(GetProcAddress(dbghelp, "SymCleanup"));
        if (cleanup)
            cleanup(GetCurrentProcess());
    }
}
} // namespace CrashDiagnostics
