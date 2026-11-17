#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <delayimp.h>

#include <cstring>

namespace {

FARPROC WINAPI ResolveNodeFromHost(unsigned int event, DelayLoadInfo* info) {
    if (event != dliNotePreLoadLibrary || !info || !info->szDll) {
        return nullptr;
    }

    if (_stricmp(info->szDll, "node.exe") != 0) {
        return nullptr;
    }

    return reinterpret_cast<FARPROC>(GetModuleHandleW(nullptr));
}

} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = ResolveNodeFromHost;

#endif
