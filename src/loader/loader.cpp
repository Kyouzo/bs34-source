#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

#include <sstream>
#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "ui_theme.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "common/inject_common.hpp"
#include "common/string_utils.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

constexpr const char* kBrand = "CiyE";
constexpr const char* kVersion = "v2.1";
constexpr const wchar_t* kGame = L"BloodStrike.exe";
constexpr uint8_t kMagic[8] = {'B', 'S', 'L', 'D', 'P', 'A', 'Y', '1'};

const ImU32 kAccent  = CiyEUi::Accent;
const ImU32 kText    = CiyEUi::Text;
const ImU32 kMuted   = CiyEUi::Muted;
const ImU32 kSuccess = CiyEUi::Success;
const ImU32 kError   = CiyEUi::Danger;
const ImU32 kWarn    = CiyEUi::Warning;

// ---------------------------------------------------------------------------
// D3D / window state
// ---------------------------------------------------------------------------

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
bool g_running = true;
float g_scale = 1.0f;

ImFont* g_font = nullptr;
ImFont* g_font_bold = nullptr;
ImFont* g_font_med = nullptr;
ImFont* g_font_small = nullptr;
ImFont* g_font_mono = nullptr;

inline float S(float v) { return v * g_scale; }

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got,
                                           &g_context);
    }
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swap) {
        g_swap->Release();
        g_swap = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

std::mutex g_log_mutex;
std::deque<std::string> g_log_lines;
std::atomic<int> g_log_seq{0};

void Log(const std::string& msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char head[24];
    sprintf_s(head, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_lines.push_back(head + msg);
    while (g_log_lines.size() > 800) g_log_lines.pop_front();
    g_log_seq.fetch_add(1);
}

// ---------------------------------------------------------------------------
// Strings / files
// ---------------------------------------------------------------------------

using strutil::Utf8ToWide;
using strutil::WideToUtf8;

std::wstring SelfPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring SelfDir() {
    std::wstring p = SelfPath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (1ll << 30)) {
        CloseHandle(f);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(f);
    return ok && read == out.size();
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == data.size();
}

// ---------------------------------------------------------------------------
// Embedded payload
// ---------------------------------------------------------------------------

struct PayloadEntry {
    std::string name;
    std::vector<uint8_t> data;
};

bool LoadPayload(std::vector<PayloadEntry>& out) {
    std::vector<uint8_t> file;
    if (!ReadFileBytes(SelfPath(), file)) return false;
    if (file.size() < 16) return false;

    const uint8_t* foot = file.data() + file.size() - 16;
    if (memcmp(foot, kMagic, 8) != 0) return false;

    uint64_t payload_size = 0;
    memcpy(&payload_size, foot + 8, 8);
    if (payload_size == 0 || payload_size > file.size() - 16) return false;

    const size_t body_end = file.size() - 16;
    size_t pos = body_end - (size_t)payload_size;

    while (pos + 4 <= body_end) {
        uint32_t name_len = 0;
        memcpy(&name_len, file.data() + pos, 4);
        pos += 4;
        if (name_len == 0 || name_len > 512 || pos + name_len > body_end) return false;

        std::string name((const char*)file.data() + pos, name_len);
        pos += name_len;

        if (pos + 8 > body_end) return false;
        uint64_t data_len = 0;
        memcpy(&data_len, file.data() + pos, 8);
        pos += 8;
        if (data_len > body_end || pos + (size_t)data_len > body_end) return false;

        PayloadEntry entry;
        entry.name = std::move(name);
        entry.data.assign(file.begin() + pos, file.begin() + pos + (size_t)data_len);
        pos += (size_t)data_len;
        out.push_back(std::move(entry));
    }
    return !out.empty();
}

std::wstring MakeExtractDir() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"CiyE_BSLoader";
    CreateDirectoryW(base.c_str(), nullptr);
    return base;
}

void WriteRootHint(const std::wstring& base, const std::wstring& used) {
    std::vector<uint8_t> bytes((const uint8_t*)used.data(),
                               (const uint8_t*)(used.data() + used.size()));
    WriteFileBytes(base + L"\\root.txt", bytes);
}

bool ExtractPayload(const std::vector<PayloadEntry>& entries, const std::wstring& dir,
                    std::wstring& used_dir) {
    auto ensure = [](const std::wstring& p) { CreateDirectoryW(p.c_str(), nullptr); };
    ensure(dir);
    ensure(dir + L"\\pyexec");
    ensure(dir + L"\\driver");
    ensure(dir + L"\\ipc");

    // If a previously injected copy still holds the DLLs, use a fresh sibling dir.
    std::wstring target = dir;
    if (GetFileAttributesW((dir + L"\\bs_pyexec.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        HANDLE probe = CreateFileW((dir + L"\\bs_pyexec.dll").c_str(), GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            char suffix[32];
            sprintf_s(suffix, "_%lu", (unsigned long)GetCurrentProcessId());
            target = dir + Utf8ToWide(suffix);
            ensure(target);
            ensure(target + L"\\pyexec");
            ensure(target + L"\\driver");
            ensure(target + L"\\ipc");
        } else {
            CloseHandle(probe);
        }
    }

    for (const PayloadEntry& entry : entries) {
        std::wstring rel = Utf8ToWide(entry.name);
        for (wchar_t& ch : rel) {
            if (ch == L'/') ch = L'\\';
        }
        std::wstring path = target + L"\\" + rel;
        size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos) ensure(path.substr(0, slash));
        if (!WriteFileBytes(path, entry.data)) {
            Log("write failed: " + entry.name);
            return false;
        }
    }
    used_dir = target;
    return true;
}

// ---------------------------------------------------------------------------
// Process / injection primitives (from inject_common.hpp)
// ---------------------------------------------------------------------------

using inject::FindProcess;
using inject::WaitForProcess;
using inject::EnableDebugPrivilege;
using inject::ReadMem;
using inject::WriteMem;
using inject::QueryPeb;
using inject::ReadRemoteWide;
using inject::FindModuleBase;
using inject::ZeroRemoteString;
using inject::HideModuleFromPeb;
using inject::CreateHiddenThread;
using inject::OpenProcessProgressive;

bool InjectDll(DWORD pid, const std::wstring& dll_path, bool hide) {
    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("missing dll: " + WideToUtf8(dll_path));
        return false;
    }

    DWORD granted = 0;
    HANDLE process = OpenProcessProgressive(pid, granted);
    if (!process) {
        Log("OpenProcess denied (HyperShield?)");
        return false;
    }
    const DWORD needed =
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    if ((granted & needed) != needed) {
        Log("handle rights stripped (have=" + std::to_string(granted) + ")");
        CloseHandle(process);
        return false;
    }

    const SIZE_T path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        Log("VirtualAllocEx failed");
        CloseHandle(process);
        return false;
    }
    if (!WriteProcessMemory(process, remote, dll_path.c_str(), path_bytes, nullptr)) {
        Log("WriteProcessMemory failed");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* load_library = reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = CreateHiddenThread(process, load_library, remote);
    if (!thread) {
        Log("CreateRemoteThread failed");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    std::wstring module_name = dll_path.substr(dll_path.find_last_of(L"\\/") + 1);
    uintptr_t peb = QueryPeb(process);
    uintptr_t module_base = FindModuleBase(process, peb, module_name);
    if (!module_base && wait == WAIT_OBJECT_0 && exit_code) module_base = exit_code;
    if (!module_base) {
        Log("injection failed: module not present");
        CloseHandle(process);
        return false;
    }

    bool hidden = false;
    if (hide) hidden = HideModuleFromPeb(process, module_base, peb);
    CloseHandle(process);

    char buf[96];
    sprintf_s(buf, "injected %ls base=0x%llx%s", module_name.c_str(),
              (unsigned long long)module_base, hidden ? " (hidden)" : "");
    Log(buf);
    return true;
}

// ---------------------------------------------------------------------------
// Driver path
// ---------------------------------------------------------------------------

bool DriverDeviceReady() {
    HANDLE h = CreateFileW(L"\\\\.\\WinSyncDrv", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool RunAndWait(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                DWORD timeout_ms, DWORD& exit_code) {
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!args.empty()) cmdline += L" " + args;

    HANDLE hRead = nullptr, hWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | (hWrite ? STARTF_USESTDHANDLES : 0);
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(0);

    BOOL ok = CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    if (hWrite) CloseHandle(hWrite);

    if (!ok) {
        if (hRead) CloseHandle(hRead);
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (hRead) {
        char buf[1024];
        DWORD read = 0;
        std::string accumulated;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
            buf[read] = 0;
            accumulated += buf;
        }
        CloseHandle(hRead);

        size_t start = 0;
        while (start < accumulated.size()) {
            size_t end = accumulated.find_first_of("\r\n", start);
            if (end == std::string::npos) end = accumulated.size();
            if (end > start) {
                std::string line = accumulated.substr(start, end - start);
                if (!line.empty() && line != "\r") {
                    Log("  > " + line);
                }
            }
            start = end + 1;
        }
    }

    return wait == WAIT_OBJECT_0;
}

bool LoadKernelDriver(const std::wstring& dir) {
    std::wstring drv_dir = dir + L"\\driver";
    std::wstring kdmapper = drv_dir + L"\\kdmapper.exe";
    std::wstring sys = drv_dir + L"\\bskdrv.sys";
    if (GetFileAttributesW(kdmapper.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(sys.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("driver payload not embedded");
        return false;
    }
    Log("loading kernel driver via kdmapper...");
    DWORD code = 1;
    std::wstring sys_arg = L"\"" + sys + L"\"";
    RunAndWait(kdmapper, sys_arg, drv_dir, 20000, code);
    for (int i = 0; i < 10; ++i) {
        if (DriverDeviceReady()) {
            Log("kernel driver ready");
            return true;
        }
        Sleep(500);
    }
    Log("kernel driver did not respond");
    return false;
}

bool MapViaDriver(const std::wstring& dir, DWORD pid, const std::wstring& pyexec,
                  const std::wstring& overlay) {
    (void)pid;
    std::wstring bsmap = dir + L"\\bsmap.exe";
    if (GetFileAttributesW(bsmap.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("bsmap.exe not embedded");
        return false;
    }
    std::wstring args = L"\"" + std::wstring(kGame) + L"\" \"" + pyexec + L"\" \"" + overlay + L"\"";
    Log("mapping modules through kernel driver...");
    DWORD code = 1;
    RunAndWait(bsmap, args, dir, 30000, code);
    char buf[64];
    sprintf_s(buf, "bsmap exit=%lu", (unsigned long)code);
    Log(buf);
    return code == 0;
}

// ---------------------------------------------------------------------------
// Worker state
// ---------------------------------------------------------------------------

enum Phase { kIdle = 0, kWaiting = 1, kInjecting = 2, kReady = 3, kFailed = 4 };

std::atomic<int> g_phase{kIdle};
std::atomic<unsigned long> g_pid{0};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_driver_mode{false};
std::atomic<bool> g_used_driver{false};
std::atomic<unsigned long> g_live_pid{0};
std::atomic<bool> g_auto_inject{false};
std::atomic<bool> g_close_on_done{false};

void InjectWorker(bool driver_mode) {
    g_busy.store(true);
    g_phase.store(kWaiting);

    Log("reading embedded payload...");
    std::vector<PayloadEntry> payload;
    if (!LoadPayload(payload)) {
        Log("payload missing - rebuild with pack_loader");
        g_phase.store(kFailed);
        g_busy.store(false);
        return;
    }

    std::wstring dir;
    std::wstring base = MakeExtractDir();
    if (!ExtractPayload(payload, base, dir)) {
        Log("payload extraction failed");
        g_phase.store(kFailed);
        g_busy.store(false);
        return;
    }
    WriteRootHint(base, dir);
    Log("payload ready (" + std::to_string(payload.size()) + " files)");

    std::wstring pyexec = dir + L"\\bs_pyexec.dll";
    std::wstring overlay = dir + L"\\bs_overlay.dll";

    Log("waiting for BloodStrike.exe (up to 90s)...");
    DWORD pid = WaitForProcess(kGame, 90);
    if (!pid) {
        Log("game not found - start it and press INJECT again");
        g_phase.store(kFailed);
        g_busy.store(false);
        return;
    }
    g_pid.store(pid);
    char buf[64];
    sprintf_s(buf, "found BloodStrike.exe pid=%lu", (unsigned long)pid);
    Log(buf);

    if (!EnableDebugPrivilege()) Log("warning: SeDebugPrivilege not enabled (run as admin)");

    g_phase.store(kInjecting);
    bool ok = false;

    if (driver_mode) {
        g_used_driver.store(true);
        if (!DriverDeviceReady()) LoadKernelDriver(dir);
        if (DriverDeviceReady()) ok = MapViaDriver(dir, pid, pyexec, overlay);
        if (!ok) {
            Log("driver path unavailable - falling back to usermode");
            g_used_driver.store(false);
        }
    }

    if (!ok) {
        Log("injecting python bridge...");
        ok = InjectDll(pid, pyexec, true);
        if (ok) {
            Log("waiting for runtime boot...");
            Sleep(2000);
            Log("injecting overlay...");
            ok = InjectDll(pid, overlay, true);
        }
    }

    if (ok) {
        Log("done - press INSERT in game to open the menu");
        g_phase.store(kReady);
    } else {
        Log("injection failed");
        g_phase.store(kFailed);
    }
    g_busy.store(false);
}

bool ToggleButton(const char* label, bool active, ImVec2 size) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.22f, .19f, .31f, 1));
    bool clicked = ImGui::Button(label, size);
    if (active) ImGui::PopStyleColor();
    return clicked;
}

const char* PhaseText(int phase) {
    switch (phase) {
        case kWaiting: return "WAITING";
        case kInjecting: return "INJECTING";
        case kReady: return "READY";
        case kFailed: return "FAILED";
        default: return "IDLE";
    }
}

ImU32 PhaseColor(int phase) {
    switch (phase) {
        case kWaiting: return kWarn;
        case kInjecting: return kAccent;
        case kReady: return kSuccess;
        case kFailed: return kError;
        default: return kMuted;
    }
}

void RenderUI() {
    auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    auto* dl = ImGui::GetWindowDrawList(); const ImVec2 wp = ImGui::GetWindowPos(), ws = io.DisplaySize;
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), CiyEUi::Ground);
    const float margin = S(28), left = S(264), gap = S(32), right_x = margin + left + gap;
    CiyEUi::Logo(dl, ImVec2(wp.x + S(20), wp.y + S(12)), S(22));
    dl->AddText(g_font_med, S(13), ImVec2(wp.x + S(54), wp.y + S(15)), kText, kBrand);
    const float badge_x = S(54) + g_font_med->CalcTextSizeA(S(13), FLT_MAX, 0, kBrand).x + S(10);
    const float badge_w = g_font_small->CalcTextSizeA(S(11.5f), FLT_MAX, 0, kVersion).x + S(14);
    dl->AddRectFilled(ImVec2(wp.x+badge_x,wp.y+S(13)),ImVec2(wp.x+badge_x+badge_w,wp.y+S(31)),CiyEUi::Elevated,S(9));
    dl->AddText(g_font_small,S(11.5f),ImVec2(wp.x+badge_x+S(7),wp.y+S(16)),kMuted,kVersion);
    dl->AddLine(ImVec2(wp.x,wp.y+S(44)),ImVec2(wp.x+ws.x,wp.y+S(44)),CiyEUi::Border);
    static DWORD last_scan = 0;
    DWORD now_tick = GetTickCount();
    if (!g_busy.load() && now_tick - last_scan > 800) {
        last_scan = now_tick; DWORD found = FindProcess(kGame);
        g_live_pid.store(found); if (!found) g_pid.store(0);
    }
    bool busy = g_busy.load();
    if (g_auto_inject.load() && !busy && g_phase.load() == kIdle && g_live_pid.load()) {
        bool dm_auto = g_driver_mode.load(); g_used_driver.store(dm_auto); g_busy.store(true);
        std::thread(InjectWorker, dm_auto).detach(); busy = true;
    }
    const int phase = g_phase.load();
    unsigned long pid = g_pid.load(); if (!pid) pid = g_live_pid.load();
    const char* title = phase == kReady ? "Session ready" : phase == kFailed ? "Session needs attention" : busy ? "Starting session" : "Ready when you are";
    CiyEUi::Logo(dl, ImVec2(wp.x + margin, wp.y + S(66)), S(40));
    dl->AddCircle(ImVec2(wp.x + margin + S(20), wp.y + S(86)), S(25), PhaseColor(phase), 48, S(1.25f));
    dl->AddText(g_font_bold, S(20), ImVec2(wp.x + margin, wp.y + S(120)), kText, title);
    dl->AddText(g_font, S(13), ImVec2(wp.x + margin, wp.y + S(152)), kMuted, "Blood Strike");
    dl->AddLine(ImVec2(wp.x + margin, wp.y + S(190)), ImVec2(wp.x + margin + left, wp.y + S(190)), CiyEUi::Border);
    ImGui::SetCursorPos(ImVec2(margin, S(203)));
    ImGui::TextDisabled("Launch mode"); ImGui::SameLine(S(170));
    ImGui::TextUnformatted(g_driver_mode.load() ? "Kernel mode" : "User mode");
    ImGui::SetCursorPos(ImVec2(margin, S(234)));
    ImGui::BeginDisabled(busy);
    if (ToggleButton("User mode", !g_driver_mode.load(), ImVec2(S(127), S(32)))) g_driver_mode.store(false);
    ImGui::SameLine(0, S(10));
    if (ToggleButton("Kernel mode", g_driver_mode.load(), ImVec2(S(127), S(32)))) g_driver_mode.store(true);
    ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(margin, ws.y - S(86)));
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(CiyEUi::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.73f, .65f, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.07f, .055f, .11f, 1));
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(busy ? "Starting..." : "Start session", ImVec2(left, S(40)))) {
        if (!busy) { bool dm = g_driver_mode.load(); g_used_driver.store(dm); std::thread(InjectWorker, dm).detach(); }
    }
    ImGui::EndDisabled(); ImGui::PopStyleColor(3);
    ImGui::SetCursorPos(ImVec2(margin, S(284)));
    bool auto_start = g_auto_inject.load(), close_done = g_close_on_done.load();
    if (CiyEUi::Toggle("Start automatically", &auto_start, left, g_scale)) g_auto_inject.store(auto_start);
    ImGui::SetCursorPos(ImVec2(margin, S(318)));
    if (CiyEUi::Toggle("Close when ready", &close_done, left, g_scale)) g_close_on_done.store(close_done);
    ImGui::SetCursorPos(ImVec2(margin, ws.y - S(42)));
    if (ImGui::Button("Open folder", ImVec2(S(122), S(28)))) { std::wstring dir = SelfDir(); ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    ImGui::SetCursorPos(ImVec2(right_x, S(58)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(CiyEUi::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    ImGui::BeginChild("startup", ImVec2(ws.x - right_x - S(16), ws.y - S(74)), ImGuiChildFlags_Borders);
    ImGui::PushFont(g_font_med, S(14));
    ImGui::TextUnformatted("Startup");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(14)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, S(16)));
    auto status_row = [&](const char* label, const char* detail, ImU32 color, bool last = false) {
        const float row_y = ImGui::GetCursorPosY();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        dl = ImGui::GetWindowDrawList();
        const ImVec2 center(pos.x + S(9), pos.y + S(9));
        if (!last) dl->AddLine(ImVec2(center.x, center.y + S(13)), ImVec2(center.x, center.y + S(59)), CiyEUi::Border, S(1));
        if (color == kSuccess) {
            dl->AddCircleFilled(center, S(9), color);
            dl->AddLine(ImVec2(center.x-S(3),center.y),ImVec2(center.x-S(1),center.y+S(2)),CiyEUi::Ground,S(1.5f));
            dl->AddLine(ImVec2(center.x-S(1),center.y+S(2)),ImVec2(center.x+S(4),center.y-S(3)),CiyEUi::Ground,S(1.5f));
        } else dl->AddCircle(center, S(8), color, 24, S(1.5f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(34)); ImGui::BeginGroup();
        ImGui::PushFont(g_font_med, S(13)); ImGui::TextUnformatted(label); ImGui::PopFont();
        ImGui::PushFont(g_font_small, S(11.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color == kError ? kError : kMuted));
        ImGui::TextWrapped("%s", detail); ImGui::PopStyleColor(); ImGui::PopFont(); ImGui::EndGroup();
        ImGui::SetCursorPosY(row_y + S(72));
    };
    char pid_text[80]; if (pid) sprintf_s(pid_text, "BloodStrike.exe - PID %lu", pid); else strcpy_s(pid_text, "Open Blood Strike to continue.");
    status_row("Game process", pid_text, pid ? kSuccess : kWarn);
    status_row("Launch mode", g_driver_mode.load() ? "Kernel mode selected" : "User mode selected", kAccent);
    status_row("Current state", PhaseText(phase), PhaseColor(phase), true);
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Activity log")) {
        std::vector<std::string> lines;
        { std::lock_guard<std::mutex> lock(g_log_mutex); lines.assign(g_log_lines.begin(), g_log_lines.end()); }
        if (lines.empty()) ImGui::TextDisabled("No activity yet.");
        for (const auto& line : lines) ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
    if (g_close_on_done.load() && g_phase.load() == kReady) {
        static bool close_posted = false; if (!close_posted) { close_posted = true; PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
    }
    ImGui::End(); ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Fonts / style / window
// ---------------------------------------------------------------------------

ImFont* LoadFont(const char* path, float size) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &cfg);
}

void LoadFonts() {
    g_font = LoadFont("C:\\Windows\\Fonts\\segoeui.ttf", S(13));
    g_font_med = LoadFont("C:\\Windows\\Fonts\\seguisb.ttf", S(14));
    g_font_bold = LoadFont("C:\\Windows\\Fonts\\seguisb.ttf", S(20));
    g_font_small = LoadFont("C:\\Windows\\Fonts\\segoeui.ttf", S(11.5f));
    g_font_mono = LoadFont("C:\\Windows\\Fonts\\consola.ttf", S(13));
    if (!g_font) g_font = ImGui::GetIO().Fonts->AddFontDefault();
    if (!g_font_med) g_font_med = g_font;
    if (!g_font_bold) g_font_bold = g_font;
    if (!g_font_small) g_font_small = g_font;
    if (!g_font_mono) g_font_mono = g_font;
}

void ApplyStyle() { CiyEUi::ApplyStyle(g_scale); ImGui::GetStyle().WindowRounding = 0; ImGui::GetStyle().WindowBorderSize = 0; }

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF caption = RGB(16, 16, 18);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    COLORREF text = RGB(230, 236, 248);
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && g_device && g_swap) {
                CleanupRenderTarget();
                g_swap->ResizeBuffers(0, (UINT)LOWORD(lparam), (UINT)HIWORD(lparam),
                                      DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            g_running = false;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();

    UINT dpi = 96;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        typedef UINT(WINAPI * GetDpiForSystemFn)();
        auto get_dpi = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
        if (get_dpi) dpi = get_dpi();
    }
    g_scale = dpi / 96.0f;
    if (g_scale < 0.75f) g_scale = 0.75f;
    if (g_scale > 2.5f) g_scale = 2.5f;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CiyELoaderWindow";
    RegisterClassExW(&wc);

    int win_w = (int)S(680);
    int win_h = (int)S(520);
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_w - win_w) / 2;
    int pos_y = (screen_h - win_h) / 2;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"CiyE Loader", WS_OVERLAPPED | WS_CAPTION |
                                 WS_SYSMENU | WS_MINIMIZEBOX,
                             pos_x, pos_y, win_w, win_h, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return 1;

    ApplyDarkTitleBar(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(g_hwnd);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    LoadFonts();
    ApplyStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    Log("CiyE loader ready.");
    Log("select mode and press INJECT.");

    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;
        if (IsIconic(g_hwnd)) {
            Sleep(10);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();
        const float clear[4] = {0.03f, 0.04f, 0.06f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    return 0;
}
