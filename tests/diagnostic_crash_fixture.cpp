#include "../src/app/CrashDiagnostics.h"

namespace {
decltype(&MiniDumpWriteDump) nativeDump = nullptr;
std::wstring scenario;
int attempts = 0;

BOOL WINAPI controlledDump(HANDLE process, DWORD pid, HANDLE output, MINIDUMP_TYPE type,
                          PMINIDUMP_EXCEPTION_INFORMATION exception,
                          PMINIDUMP_USER_STREAM_INFORMATION streams,
                          PMINIDUMP_CALLBACK_INFORMATION callback) {
    ++attempts;
    fprintf(stderr, "Dump attempt %d\n", attempts);
    fflush(stderr);
    if (scenario == L"denied") {
        SetLastError(DWORD(E_ACCESSDENIED));
        return FALSE;
    }
    if (scenario == L"partial-copy-always" || (scenario == L"partial-copy-once" && attempts == 1)) {
        const char partial[] = "MDMP-incomplete-fixture";
        DWORD written;
        WriteFile(output, partial, sizeof(partial), &written, nullptr);
        SetLastError(DWORD(HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY)));
        return FALSE;
    }
    if (attempts > 1) {
        LARGE_INTEGER size{}, position{};
        if (!GetFileSizeEx(output, &size) || !SetFilePointerEx(output, {}, &position, FILE_CURRENT)
            || size.QuadPart || position.QuadPart) {
            SetLastError(DWORD(E_UNEXPECTED));
            return FALSE;
        }
    }
    return nativeDump(process, pid, output, type, exception, streams, callback);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    CrashDiagnostics::initialize(argv[1], "WindowsHelper", "fixture", "Dynamic Lighting");
    CrashDiagnostics::appendAt(argv[1], "Dynamic Lighting", "INFO", "Fixture started");
    scenario = argv[2];
    if (scenario == L"missing-api") {
        CrashDiagnostics::dump = nullptr;
        CrashDiagnostics::dumpLoadError = ERROR_PROC_NOT_FOUND;
    } else if (scenario != L"crash") {
        nativeDump = CrashDiagnostics::dump;
        if (!nativeDump)
            return 3;
        CrashDiagnostics::dump = controlledDump;
    }
    // A write access violation carries the operation and fault address.
    const ULONG_PTR details[]{1, 0};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, details);
    return 0;
}
