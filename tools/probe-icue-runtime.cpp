#include <windows.h>
#include <cstdio>
#include <string>
int wmain(int argc,wchar_t **argv) {
    if (argc!=2) return 2;
    wchar_t full[32768]{}; if (!GetFullPathNameW(argv[1],32768,full,nullptr)) return 2;
    std::wstring directory=full;
    for (auto &ch : directory) if (ch==L'/') ch=static_cast<wchar_t>(92);
    auto library=LoadLibraryExW((directory+L"\\iCUESDK.x64_2019.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) { std::printf("SDK load failed: %lu\n",GetLastError()); return 3; }
    for (auto name : {L"msvcp140.dll",L"vcruntime140.dll",L"vcruntime140_1.dll"}) {
        wchar_t path[32768]{}; auto module=GetModuleHandleW(name);
        if (!module || !GetModuleFileNameW(module,path,32768)) return 4;
        if (_wcsnicmp(path,directory.c_str(),directory.size())!=0) { std::wprintf(L"Unexpected non-local dependency: %ls\n",path); return 5; }
        std::wprintf(L"App-local dependency verified: %ls\n",path);
    }
    std::puts("SDK loaded successfully without using the installed VC++ redistributable.");
    return 0;
}
