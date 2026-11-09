#include <windows.h>
#include <cstdint>
#include <string>

#include "common/root_hint.hpp"

namespace {
constexpr uintptr_t kPyGILStateEnsure = 0x2ADC1C0; // 48 89 5C 24 ? 57 48 83 EC ? 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B D8 48 85 C0 75
constexpr uintptr_t kPyGILStateRelease = 0x2ADC290; // 48 89 5C 24 ? 57 48 83 EC ? 8B F9 48 8D 0D ? ? ? ? E8
constexpr uintptr_t kPyRunSimpleStringFlags = 0x2A9DEE0; // 48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B F1 48 8B DA 48 8D 0D

std::wstring g_root;
std::wstring g_cmd;
std::wstring g_out;
std::wstring g_ack;
std::wstring g_boot;

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

bool ReadRootHint(std::wstring& root) {
    std::wstring hint;
    if (!ipc::ReadRootHint(hint)) return false;
    if (GetFileAttributesW((hint + L"\\pyexec\\cmd.py").c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    root = hint;
    return true;
}

void InitPaths() {
    if (!g_root.empty()) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InitPaths), &self);
    wchar_t dll[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(self, dll, MAX_PATH);
    std::wstring dir(dll, n ? n : 0);
    size_t p = dir.find_last_of(L"\\/");
    if (p != std::wstring::npos) dir.resize(p);

    std::wstring root;
    if (GetFileAttributesW((dir + L"\\pyexec\\cmd.py").c_str()) != INVALID_FILE_ATTRIBUTES) {
        root = dir;
    } else if (GetFileAttributesW((dir + L"\\..\\..\\pyexec\\cmd.py").c_str()) !=
               INVALID_FILE_ATTRIBUTES) {
        root = dir + L"\\..\\..";
    } else if (!ReadRootHint(root)) {
        root = dir;
    }

    g_root = root;
    g_cmd = root + L"\\pyexec\\cmd.py";
    g_out = root + L"\\pyexec\\out.txt";
    g_ack = root + L"\\pyexec\\ack.txt";
    g_boot = root + L"\\pyexec\\booted.txt";
    CreateDirectoryW((root + L"\\ipc").c_str(), nullptr);
    SetEnvironmentVariableW(L"BS_ROOT", root.c_str());
}

std::string BuildRunner() {
    std::string out = ToUtf8(g_out);
    std::string cmd = ToUtf8(g_cmd);
    std::string src;
    src += "import sys, traceback, time\n";
    src += "_out = open(r'" + out + "','a',encoding='utf-8',errors='replace')\n";
    src += "sys.stdout = _out\n";
    src += "sys.stderr = _out\n";
    src += "try:\n";
    src += "    _src = open(r'" + cmd + "','r',encoding='utf-8').read()\n";
    src += "    exec(compile(_src, r'" + cmd + "', 'exec'), {'__name__':'__main__','__file__':r'" + cmd + "'})\n";
    src += "except BaseException:\n";
    src += "    traceback.print_exc()\n";
    src += "_out.write('=== done %s ===\\n' % time.strftime('%H:%M:%S'))\n";
    src += "_out.flush()\n";
    src += "_out.close()\n";
    return src;
}

using PyGILStateEnsureFn = int(__cdecl*)();
using PyGILStateReleaseFn = void(__cdecl*)(int);
using PyRunSimpleStringFlagsFn = int(__cdecl*)(const char*, void*);

void Touch(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

FILETIME LastWrite(const wchar_t* path, bool& exists) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    exists = GetFileAttributesExW(path, GetFileExInfoStandard, &data) != FALSE;
    return exists ? data.ftLastWriteTime : FILETIME{};
}

bool SameTime(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

DWORD WINAPI Worker(LPVOID) {
    InitPaths();
    HMODULE game = nullptr;
    for (int i = 0; i < 600 && !game; ++i) {
        game = GetModuleHandleW(L"BloodStrike.exe");
        if (!game) Sleep(100);
    }
    if (!game) return 0;

    auto ensure = reinterpret_cast<PyGILStateEnsureFn>(reinterpret_cast<uintptr_t>(game) + kPyGILStateEnsure);
    auto release = reinterpret_cast<PyGILStateReleaseFn>(reinterpret_cast<uintptr_t>(game) + kPyGILStateRelease);
    auto run = reinterpret_cast<PyRunSimpleStringFlagsFn>(reinterpret_cast<uintptr_t>(game) + kPyRunSimpleStringFlags);

    Sleep(2000);
    for (int i = 0; i < 120; ++i) {
        int st = ensure();
        int ok = run("import gclient.framework.util.story_tick", nullptr);
        release(st);
        if (ok == 0) break;
        Sleep(500);
    }

    std::string runner = BuildRunner();
    bool exists = false;
    FILETIME last = LastWrite(g_cmd.c_str(), exists);
    DeleteFileW(g_boot.c_str());
    bool first = true;
    unsigned long last_try = 0;
    while (true) {
        Sleep(400);
        bool nowExists = false;
        FILETIME now = LastWrite(g_cmd.c_str(), nowExists);
        if (!nowExists) continue;
        bool changed = !SameTime(now, last);
        unsigned long tick = GetTickCount();
        bool booted = GetFileAttributesW(g_boot.c_str()) != INVALID_FILE_ATTRIBUTES;
        bool retry = !booted && !first && tick - last_try >= 2000;
        if (!first && !changed && !retry) continue;
        if (changed) last = now;
        first = false;
        last_try = tick;
        int state = ensure();
        int rc = run(runner.c_str(), nullptr);
        release(state);
        Touch(g_ack.c_str());
        (void)rc;
    }
}
}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
