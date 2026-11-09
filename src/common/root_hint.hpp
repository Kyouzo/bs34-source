#pragma once
#include <windows.h>
#include <string>

namespace ipc {

inline bool ReadRootHint(std::wstring& root) {
    wchar_t tmp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    std::wstring path = std::wstring(tmp) + L"CiyE_BSLoader\\root.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size < 2 || size > 4096) {
        CloseHandle(h);
        return false;
    }
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &buf[0], size, &read, nullptr);
    CloseHandle(h);
    if (!ok || read < 2) return false;
    buf.resize(read / sizeof(wchar_t));
    root = buf;
    return true;
}

} // namespace ipc
