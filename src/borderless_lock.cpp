// Borderless Lock (Crimson Desert, 2026-11-06): the game's borderless window answers Windows' hit-test with an invisible
// resize border (about 8 px: top-left corner 13, left edge 10, top 12, bottom-right 17), and the cursor starts on that
// corner. A click-and-drag there starts a real resize and the game drops to windowed mode. While the window covers its
// monitor, those answers become "client" and the system Size / Move commands are ignored; windowed mode is untouched.
//
// The cursor (v1, from v0's log): the game never answers WM_SETCURSOR, so on every click / move Windows puts up the
// window's default arrow until the game's next SetCursor covers it (right clicks in menus: until release; the outer
// strip of the screen: always). Over the game's own area the mod answers it with the game's last cursor. And the game
// shows its cursor when it loses focus and at the end of a load, and stays in cursor mode (the mouse moves a cursor,
// not the camera) until Kliff moves. That is the game's own input state: hiding the cursor (v2) only made it
// invisible and stuck it hidden in menus, so the mod leaves showing / hiding to the game. (rehide() is unused.)

#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace {

using U32 = std::uint32_t;
HMODULE g_self = nullptr;

// --- the game's windows -------------------------------------------------------------------------------------------
std::mutex g_mu;
std::map<HWND, WNDPROC> g_orig; // windows we wrap -> their previous window procedure

WNDPROC orig_of(HWND h)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_orig.find(h);
    return it == g_orig.end() ? nullptr : it->second;
}

// The window covers its whole monitor (borderless or fullscreen) and has no title bar.
bool borderless(HWND h)
{
    RECT r{};
    MONITORINFO mi{sizeof mi};
    if (!GetWindowRect(h, &r) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    LONG style = GetWindowLongW(h, GWL_STYLE);
    return (style & WS_CAPTION) != WS_CAPTION && r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

const char* hit_name(LRESULT ht)
{
    switch (ht) {
    case HTCLIENT: return "client";
    case HTCAPTION: return "caption";
    case HTLEFT: return "left edge";
    case HTRIGHT: return "right edge";
    case HTTOP: return "top edge";
    case HTTOPLEFT: return "top-left corner";
    case HTTOPRIGHT: return "top-right corner";
    case HTBOTTOM: return "bottom edge";
    case HTBOTTOMLEFT: return "bottom-left corner";
    case HTBOTTOMRIGHT: return "bottom-right corner";
    case HTNOWHERE: return "nowhere";
    default: return "other";
    }
}

const char* mouse_msg_name(UINT m)
{
    switch (m) {
    case WM_MOUSEMOVE: return "move";
    case WM_LBUTTONDOWN: return "left down";
    case WM_LBUTTONUP: return "left up";
    case WM_RBUTTONDOWN: return "right down";
    case WM_RBUTTONUP: return "right up";
    case WM_MBUTTONDOWN: return "middle down";
    case WM_MBUTTONUP: return "middle up";
    case 0: return "none (asked without a mouse message)";
    default: return "other";
    }
}

std::string cursor_name(HCURSOR c)
{
    static const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    if (!c) return "none";
    if (c == arrow) return "Windows arrow";
    char b[32];
    std::snprintf(b, sizeof b, "%p", static_cast<void*>(c));
    return b;
}

thread_local bool t_in_setcursor = false; // (the game's SetCursor calls made while it answers WM_SETCURSOR)
thread_local bool t_own = false;           // (our own SetCursor: not the game's choice)
std::string g_last_setcursor_line;         // DIAG: the last WM_SETCURSOR line logged (only changes are logged)
std::atomic<HCURSOR> g_game_cursor{nullptr}; // the game's last own cursor (not the arrow, not none)
std::atomic<U32> g_replaced{0};              // arrows replaced (logged now and then)
constexpr UINT kRehideMsg = WM_APP + 0x4C42; // (posted to the window: hide the cursor again once it is in front)
bool g_hidden_before = false;                // the cursor was hidden when the game lost focus (gameplay)
HWND g_main = nullptr;                       // the game's main window (borderless when it was activated)

bool cursor_showing()
{
    CURSORINFO ci{sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}

// The cursor hidden again, as the game does when Kliff moves (only while the game is in front and borderless).
void rehide(HWND h, const char* why)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId() || !borderless(h) || !cursor_showing()) return;
    t_own = true;
    int n = ShowCursor(FALSE);
    t_own = false;
    LOG("cursor hidden again (%s): count %d", why, n);
}

LRESULT CALLBACK lock_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = orig_of(h);
    if (!orig) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCHITTEST: {
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        if (r >= HTLEFT && r <= HTBOTTOMRIGHT && borderless(h)) {
            static LRESULT last = 0;
            if (r != last) LOG("hit-test: the game said %s at %d,%d; answered client (borderless)", hit_name(r),
                               static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)));
            last = r;
            return HTCLIENT;
        }
        return r;
    }
    case WM_SYSCOMMAND: {
        UINT cmd = static_cast<UINT>(w & 0xFFF0);
        if ((cmd == SC_SIZE || cmd == SC_MOVE) && borderless(h)) {
            LOG("system command %s ignored (borderless)", cmd == SC_SIZE ? "Size" : "Move");
            return 0;
        }
        break;
    }
    case WM_SETCURSOR: {
        t_in_setcursor = true;
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        t_in_setcursor = false;
        if (LOWORD(l) == HTCLIENT && borderless(h)) { // (the game left it to Windows: the arrow went up)
            if (HCURSOR gc = g_game_cursor.load()) {
                t_own = true;
                SetCursor(gc);
                t_own = false;
                if (++g_replaced % 200 == 1) LOG("the Windows arrow replaced by the game's cursor (%u times so far)",
                                                 g_replaced.load());
            }
            r = TRUE;
        }
        CURSORINFO ci{sizeof ci};
        GetCursorInfo(&ci);
        char line[200];
        std::snprintf(line, sizeof line, "WM_SETCURSOR over %s on %s: the game %s; cursor %s, %s",
                      hit_name(static_cast<short>(LOWORD(l))), mouse_msg_name(HIWORD(l)),
                      r ? "set it (returned TRUE)" : "left it to Windows", cursor_name(ci.hCursor).c_str(),
                      (ci.flags & CURSOR_SHOWING) ? "showing" : "hidden");
        if (g_last_setcursor_line != line) LOG("%s", line);
        g_last_setcursor_line = line;
        return r;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && borderless(h)) g_main = h;
        if (LOWORD(w) == WA_INACTIVE) g_hidden_before = !cursor_showing(); // (before the game shows it)
        LOG("window %s%s", LOWORD(w) == WA_INACTIVE ? "deactivated" : "activated",
            LOWORD(w) == WA_INACTIVE ? (g_hidden_before ? " (cursor was hidden: gameplay)" : " (cursor was showing)") : "");
        break;
    case WM_ACTIVATEAPP:
        LOG("game %s", w ? "in front" : "in the background");
        break;
    default:
        if (m == kRehideMsg) {
            if (w || g_hidden_before) rehide(h, w ? "after a load" : "back in front in gameplay");
            if (!w) g_hidden_before = false;
            return 0;
        }
        break;
    case WM_NCDESTROY: {
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        std::lock_guard<std::mutex> lock(g_mu);
        g_orig.erase(h);
        return r;
    }
    }
    return CallWindowProcW(orig, h, m, w, l);
}

void wrap(HWND h, const char* why)
{
    if (!h || GetParent(h)) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_orig.count(h)) return;
    }
    wchar_t cls[128] = L"";
    GetClassNameW(h, cls, 128);
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_CHILD) return;
    WNDPROC prev = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_WNDPROC));
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_orig[h] = prev;
    }
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&lock_proc));
    LOG("wrapped window %p (%s, class %ls, style %08lX)", static_cast<void*>(h), why, cls, static_cast<unsigned long>(style));
}

// --- the game's top-level windows as they are created -------------------------------------------------------------
using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExWFn g_create_orig = nullptr;

HWND WINAPI create_hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                        HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND wnd = g_create_orig(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (wnd && !parent && !(style & WS_CHILD)) wrap(wnd, "created");
    return wnd;
}

// --- DIAG (v0): the game's own cursor calls -------------------------------------------------------------------------
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
using ShowCursorFn = int(WINAPI*)(BOOL);
SetCursorFn g_setcursor_orig = nullptr;
ShowCursorFn g_showcursor_orig = nullptr;
std::atomic<HCURSOR> g_last_set{reinterpret_cast<HCURSOR>(~0ull)};

std::mutex g_seen_mu;
std::set<HCURSOR> g_seen; // DIAG: cursor handles the game has set (each logged once)

HCURSOR WINAPI setcursor_hook(HCURSOR c)
{
    HCURSOR prev = g_setcursor_orig(c);
    if (t_own) return prev;
    static const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    if (c && c != arrow) g_game_cursor = c;
    g_last_set = c;
    bool fresh;
    {
        std::lock_guard<std::mutex> lock(g_seen_mu);
        fresh = g_seen.insert(c).second;
    }
    if (fresh) LOG("the game set cursor %s for the first time", cursor_name(c).c_str());
    return prev;
}


int WINAPI showcursor_hook(BOOL show)
{
    int n = g_showcursor_orig(show);
    if (t_own) return n;
    static const HCURSOR wait = LoadCursorW(nullptr, IDC_WAIT), starting = LoadCursorW(nullptr, IDC_APPSTARTING);
    HCURSOR last = g_last_set.load();
    if (show && n == 0 && (last == wait || last == starting)) { // (a loading screen ended: the busy cursor, then shown)
        LOG("a load ended (busy cursor, then shown)");
    }
    // (logged when it changes whether the cursor shows: the count crosses 0)
    if ((show && n == 0) || (!show && n == -1))
        LOG("ShowCursor(%s) -> count %d: cursor %s%s", show ? "TRUE" : "FALSE", n, show ? "shown" : "hidden",
            t_in_setcursor ? " (while answering WM_SETCURSOR)" : "");
    return n;
}

BOOL CALLBACK wrap_existing(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) wrap(h, "already there");
    return TRUE;
}

DWORD WINAPI init_thread(LPVOID)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring path = buf;
    path.resize(path.find_last_of(L"/\\") + 1);
    logx::open((path + L"BorderlessLock.log").c_str());
    LOG("Borderless Lock v3 started");
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MinHook failed: inactive");
        return 0;
    }
    HMODULE u = GetModuleHandleW(L"user32.dll");
    bool ok = true;
    ok &= MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "CreateWindowExW")), &create_hook,
                        reinterpret_cast<void**>(&g_create_orig)) == MH_OK;
    ok &= MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "SetCursor")), &setcursor_hook,
                        reinterpret_cast<void**>(&g_setcursor_orig)) == MH_OK;
    ok &= MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "ShowCursor")), &showcursor_hook,
                        reinterpret_cast<void**>(&g_showcursor_orig)) == MH_OK;
    ok &= MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    LOG("hooks: %s", ok ? "in" : "FAILED (inactive)");
    EnumWindows(&wrap_existing, 0); // (a window made before the hooks went in)
    return 0;
}

bool in_game_process()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe = buf;
    exe = exe.substr(exe.find_last_of(L"/\\") + 1);
    return _wcsicmp(exe.c_str(), L"CrimsonDesert.exe") == 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        if (in_game_process())
            if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
