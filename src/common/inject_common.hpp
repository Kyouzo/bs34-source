#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <string>

namespace inject {

typedef LONG NTSTATUS;

struct ProcessBasicInformation {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
};

struct UnicodeStringRef {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct LdrEntry {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UnicodeStringRef FullDllName;
    UnicodeStringRef BaseDllName;
};

using NtQueryInformationProcessFn = NTSTATUS(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtCreateThreadExFn = NTSTATUS(WINAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID,
                                             PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);

inline DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

inline DWORD WaitForProcess(const wchar_t* name, int seconds) {
    int tries = seconds > 0 ? seconds * 2 : 1;
    for (int i = 0; i < tries; ++i) {
        DWORD pid = FindProcess(name);
        if (pid) return pid;
        Sleep(500);
    }
    return 0;
}

inline bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

inline uintptr_t QueryPeb(HANDLE process) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!query) return 0;
    ProcessBasicInformation info{};
    ULONG size = 0;
    NTSTATUS status = query(process, 0, &info, sizeof(info), &size);
    return status == 0 ? reinterpret_cast<uintptr_t>(info.PebBaseAddress) : 0;
}

inline HANDLE CreateHiddenThread(HANDLE process, void* start, void* arg) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto create = reinterpret_cast<NtCreateThreadExFn>(
            GetProcAddress(ntdll, "NtCreateThreadEx"));
        if (create) {
            HANDLE thread = nullptr;
            constexpr ULONG kHideFromDebugger = 0x4;
            NTSTATUS status = create(&thread, THREAD_ALL_ACCESS, nullptr, process, start, arg,
                                     kHideFromDebugger, 0, 0, 0, nullptr);
            if (status == 0 && thread) return thread;
        }
    }
    return CreateRemoteThread(process, nullptr, 0,
                              reinterpret_cast<LPTHREAD_START_ROUTINE>(start), arg, 0, nullptr);
}

template <typename T>
inline bool ReadMem(HANDLE process, uintptr_t address, T& out) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &out, sizeof(T), &got) &&
           got == sizeof(T);
}

template <typename T>
inline bool WriteMem(HANDLE process, uintptr_t address, const T& in) {
    SIZE_T put = 0;
    return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address), &in, sizeof(T), &put) &&
           put == sizeof(T);
}

inline bool ReadRemoteWide(HANDLE process, const UnicodeStringRef& str, std::wstring& out) {
    if (!str.Buffer || str.Length == 0) return false;
    wchar_t buf[512]{};
    SIZE_T bytes = str.Length;
    if (bytes > sizeof(buf) - sizeof(wchar_t)) bytes = sizeof(buf) - sizeof(wchar_t);
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, str.Buffer, buf, bytes, &got) || got == 0) return false;
    buf[bytes / sizeof(wchar_t)] = 0;
    out.assign(buf);
    return true;
}

inline uintptr_t FindModuleBase(HANDLE process, uintptr_t peb, const std::wstring& module_name) {
    if (!peb) return 0;
    uintptr_t ldr = 0;
    if (!ReadMem(process, peb + 0x18, ldr) || !ldr) return 0;
    uintptr_t head = ldr + 0x10;
    uintptr_t cursor = 0;
    if (!ReadMem(process, head, cursor)) return 0;
    for (int guard = 0; guard < 2048 && cursor && cursor != head; ++guard) {
        LdrEntry entry{};
        if (!ReadMem(process, cursor - 0x10, entry)) return 0;
        std::wstring name;
        if (ReadRemoteWide(process, entry.BaseDllName, name) &&
            _wcsicmp(name.c_str(), module_name.c_str()) == 0) {
            return reinterpret_cast<uintptr_t>(entry.DllBase);
        }
        uintptr_t next = 0;
        if (!ReadMem(process, cursor, next)) return 0;
        cursor = next;
    }
    return 0;
}

inline void ZeroRemoteString(HANDLE process, const UnicodeStringRef& str) {
    if (!str.Buffer || str.Length == 0) return;
    std::vector<BYTE> zeros(str.Length, 0);
    SIZE_T put = 0;
    WriteProcessMemory(process, str.Buffer, zeros.data(), zeros.size(), &put);
}

inline bool HideModuleFromPeb(HANDLE process, uintptr_t module_base, uintptr_t peb) {
    if (!module_base || !peb) return false;
    uintptr_t ldr = 0;
    if (!ReadMem(process, peb + 0x18, ldr) || !ldr) return false;
    uintptr_t head = ldr + 0x10;
    uintptr_t cursor = 0;
    if (!ReadMem(process, head, cursor)) return false;
    for (int guard = 0; guard < 2048 && cursor && cursor != head; ++guard) {
        uintptr_t entry_address = cursor - 0x10;
        LdrEntry entry{};
        if (!ReadMem(process, entry_address, entry)) return false;
        if (reinterpret_cast<uintptr_t>(entry.DllBase) == module_base) {
            const uintptr_t links[3] = {entry_address, entry_address + 0x10, entry_address + 0x20};
            for (uintptr_t link_address : links) {
                LIST_ENTRY link{};
                if (!ReadMem(process, link_address, link)) continue;
                uintptr_t prev = reinterpret_cast<uintptr_t>(link.Blink);
                uintptr_t next = reinterpret_cast<uintptr_t>(link.Flink);
                if (!prev || !next) continue;
                WriteMem(process, prev, next);
                WriteMem(process, next + 8, prev);
            }
            ZeroRemoteString(process, entry.BaseDllName);
            ZeroRemoteString(process, entry.FullDllName);
            return true;
        }
        uintptr_t next = 0;
        if (!ReadMem(process, cursor, next)) return false;
        cursor = next;
    }
    return false;
}

inline HANDLE OpenProcessProgressive(DWORD pid, DWORD& granted) {
    const DWORD masks[] = {
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_QUERY_LIMITED_INFORMATION,
    };
    granted = 0;
    for (DWORD mask : masks) {
        HANDLE handle = OpenProcess(mask, FALSE, pid);
        if (handle) {
            granted = mask;
            return handle;
        }
    }
    return nullptr;
}

} // namespace inject
