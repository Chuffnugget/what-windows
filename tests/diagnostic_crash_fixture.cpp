#include "../src/app/CrashDiagnostics.h"

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    CrashDiagnostics::initialize(argv[1], "WindowsHelper", "fixture", "Dynamic Lighting");
    CrashDiagnostics::appendAt(argv[1], "Dynamic Lighting", "INFO", "Fixture started");
    if (std::wstring(argv[2]) == L"crash")
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 0;
}
