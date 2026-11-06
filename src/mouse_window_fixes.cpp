// Mouse & Window Fixes for Crimson Desert (2026-11-06), 1.0.0. Keyboard and mouse in borderless window mode:
//
// 1. No accidental resize. The game's borderless window answers Windows' hit-test with an invisible resize border
//    (about 8 px), and the cursor starts on its top-left corner: a click and drag there resized the window and the
//    game dropped to windowed mode. While the window covers its monitor those answers become "client", and the system
//    Size / Move commands are ignored. Windowed mode is untouched.
// 2. No Windows arrow. The game never answers WM_SETCURSOR, so on clicks and at the screen's edges Windows put up its
//    plain arrow. Once the world has loaded, the mod answers with the game's own cursor.
// 3. Camera mode back at once. The game's mouse controller (an object: +0x40 its window, +0x48 / +0x4C the saved
//    cursor spot, +0x50 1 = free / cursor mode, 0 = captured / camera mode; vtable slot 0x28 "focused", 0x40 capture,
//    0x48 release) frees the mouse when the game loses focus and when the HUD appears after a load, and captures it
//    again only when Kliff moves. The mod hooks release (found by its code) and calls the controller's own capture
//    after Alt+Tab (if it was in camera mode before) and when the HUD appears in the world (input layer 4, freed by
//    one function, found by its code; menus free the mouse through other code and keep their cursor). The cursor
//    image stays blank between that release and the capture (no flash). The game's per-frame input function (found by
//    its capture rule, offsets read from the code) also gets the capture applied in gameplay layers 0, 1, 3, 5 while
//    the mouse is free, unless the game freed it itself in that layer.
// 4. The title cursor. No cursor until the title prompts appear; it then starts on the title logo's ring (the
//    controller's saved spot is 0,0 until first used: set to the ring on the first release).
// 5. Zoom with one wheel notch. The game zooms only on a second wheel event within a short window: a lone notch
//    (none in the 400 ms before it) gets a second event 50 ms later, marked so it isn't doubled again.
//
// Every game address is found by searching the game's code at start-up (tools/bl_backtest.py checks a new exe); a
// search that fails switches off only its fix. Log: MouseWindowFixes.log next to the ASI.

#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <map>
#include <mutex>
#include <string>

namespace {

using U64 = std::uint64_t;
using UPTR = std::uintptr_t;
HMODULE g_self = nullptr;

// --- the game's windows ---------------------------------------------------------------------------------------------
std::mutex g_mu;
std::map<HWND, WNDPROC> g_orig; // windows we wrap -> their previous window procedure
HWND g_main = nullptr;          // the game's main window (borderless when it was activated)

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
    case HTLEFT: return "left edge";
    case HTRIGHT: return "right edge";
    case HTTOP: return "top edge";
    case HTTOPLEFT: return "top-left corner";
    case HTTOPRIGHT: return "top-right corner";
    case HTBOTTOM: return "bottom edge";
    case HTBOTTOMLEFT: return "bottom-left corner";
    case HTBOTTOMRIGHT: return "bottom-right corner";
    default: return "other";
    }
}

bool read_u8(UPTR a, std::uint8_t& out)
{
    __try {
        out = *reinterpret_cast<volatile std::uint8_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(UPTR a, UPTR& out)
{
    __try {
        out = *reinterpret_cast<volatile UPTR*>(a);
        return out > 0x10000 && out < 0x7FFFFFFF0000ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- the cursor -----------------------------------------------------------------------------------------------------
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
SetCursorFn g_setcursor_orig = nullptr;
thread_local bool t_own = false;             // (our own SetCursor: not the game's choice)
std::atomic<HCURSOR> g_game_cursor{nullptr}; // the game's last own cursor (not the arrow, not none)
std::atomic<HCURSOR> g_last_set{nullptr};    // the last cursor the game set (a load's end: the busy cursor)

HCURSOR WINAPI setcursor_hook(HCURSOR c)
{
    HCURSOR prev = g_setcursor_orig(c);
    if (t_own) return prev;
    static const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    if (c && c != arrow) g_game_cursor = c;
    g_last_set = c;
    return prev;
}

void set_cursor_own(HCURSOR c)
{
    t_own = true;
    SetCursor(c);
    t_own = false;
}

// --- camera mode: the game's mouse controller -----------------------------------------------------------------------
using MouseFn = void(__fastcall*)(UPTR obj, U64 pos);
using FocusedFn = bool(__fastcall*)(UPTR obj);
constexpr size_t kOffWindow = 0x40, kOffSpotX = 0x48, kOffSpotY = 0x4C, kOffReleased = 0x50;
constexpr size_t kSlotFocused = 0x28 / 8, kSlotCapture = 0x40 / 8, kSlotRelease = 0x48 / 8;
constexpr UINT kRecaptureMsg = WM_APP + 0x4C42; // (posted to the main window: camera mode back; wParam 1 = the HUD)
MouseFn g_release_orig = nullptr;
UPTR g_release_addr = 0;               // the release function found by its code
UPTR g_hud_ret = 0;                    // the return address of the HUD's release call
std::atomic<UPTR> g_mouse{0};          // the controller (seen in its release calls)
std::atomic<UPTR> g_input{0};          // the game's input manager (its per-frame function's object)
size_t g_off_layer = 0x30;             // the input layer in it (read from the code: +0x28 in 1.0.0.2976)
std::atomic<int> g_block_layer{-1};    // the game freed the mouse itself in this layer: not ours to capture
std::atomic<bool> g_freed_once{false}; // the game has freed the mouse itself (the title prompts)
std::atomic<bool> g_blank{false};      // between the HUD's release and our capture: no cursor image
int g_deactivating = 0;                // (window thread: inside the game's handling of losing focus)
bool g_was_captured = false;           // the game was in camera mode when it lost focus

// The saved cursor spot is 0,0 until the cursor is first placed: the title logo's ring instead (measured at 716,381 on
// 1366x768; the logo scales with the height, so the offset is in heights; minus about half the cursor, whose hotspot
// is its tip, so its body covers the ring).
void first_spot(UPTR obj)
{
    __try {
        HWND w = *reinterpret_cast<HWND volatile*>(obj + kOffWindow);
        auto sx = reinterpret_cast<volatile std::int32_t*>(obj + kOffSpotX);
        auto sy = reinterpret_cast<volatile std::int32_t*>(obj + kOffSpotY);
        RECT r{};
        if (*sx || *sy || !IsWindow(w) || !GetClientRect(w, &r)) return;
        LONG W = r.right - r.left, H = r.bottom - r.top;
        LONG cx = GetSystemMetrics(SM_CXCURSOR), cy = GetSystemMetrics(SM_CYCURSOR);
        *sx = r.left + W / 2 + LONG(H * 0.046) - LONG(cx * 0.17);
        *sy = r.top + LONG(H * 0.496) - LONG(cy * 0.30);
        LOG("the cursor starts on the title ring (%ld, %ld)", static_cast<long>(*sx), static_cast<long>(*sy));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall release_hook(UPTR obj, U64 pos)
{
    UPTR ret = reinterpret_cast<UPTR>(_ReturnAddress());
    std::uint8_t released = 1;
    read_u8(obj + kOffReleased, released);
    g_mouse = obj;
    if (g_deactivating) {
        g_was_captured = released == 0;
    } else { // (the game's own choice: a menu or the HUD)
        std::uint8_t layer = 0xFF;
        if (UPTR in = g_input.load()) read_u8(in + g_off_layer, layer);
        g_block_layer = layer;
        g_freed_once = true;
        if (g_hud_ret && ret == g_hud_ret && layer == 4 && g_main) { // (the HUD appearing in the world)
            g_blank = true;
            PostMessageW(g_main, kRecaptureMsg, 1, 0); // (after the caller has set its own released flag)
        } else {
            static const HCURSOR wait = LoadCursorW(nullptr, IDC_WAIT), starting = LoadCursorW(nullptr, IDC_APPSTARTING);
            HCURSOR last = g_last_set.load();
            if ((last == wait || last == starting) && g_main) PostMessageW(g_main, kRecaptureMsg, 1, 0); // (a load ended)
        }
    }
    first_spot(obj);
    g_release_orig(obj, pos);
    if (g_blank.load()) set_cursor_own(nullptr); // (the release just showed the cursor: blank until the capture)
}

// The controller's own checks and capture, as the game does when Kliff moves. Only with the expected vtable.
bool call_capture(UPTR obj, bool& focused)
{
    __try {
        UPTR* vt = *reinterpret_cast<UPTR**>(obj);
        if (vt[kSlotRelease] != g_release_addr) return false;
        focused = reinterpret_cast<FocusedFn>(vt[kSlotFocused])(obj);
        if (!focused) return true;
        reinterpret_cast<MouseFn>(vt[kSlotCapture])(obj, ~0ull); // (-1: the window's centre, as the game passes)
        *reinterpret_cast<volatile std::uint8_t*>(obj + kOffReleased) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void recapture(const char* why)
{
    UPTR obj = g_mouse.load();
    std::uint8_t released = 0;
    if (!obj || !read_u8(obj + kOffReleased, released) || !released) return;
    bool focused = false;
    if (!call_capture(obj, focused)) LOG("camera mode not restored (%s): unexpected mouse controller", why);
    else if (focused) LOG("camera mode restored (%s)", why);
}

// --- the game's per-frame input function ------------------------------------------------------------------------------
using InputFn = void(__fastcall*)(UPTR self);
InputFn g_input_orig = nullptr;
UPTR g_input_flag = 0; // the global the function checks before handling the mouse

void __fastcall input_hook(UPTR self)
{
    g_input_orig(self);
    g_input = self;
    std::uint8_t layer = 0xFF, flag = 0, released = 0, cfg_on = 0;
    if (!read_u8(self + g_off_layer, layer)) return;
    static int last_layer = -2;
    if (layer != last_layer) {
        last_layer = layer;
        if (g_block_layer.load() >= 0 && g_block_layer.load() != layer) g_block_layer = -1; // (the layer changed)
    }
    if (layer > 5 || !((0x2B >> layer) & 1)) return; // (not a capturing layer)
    UPTR a = 0, b = 0, ctrl = 0, list = 0, cfg = 0;
    // as the game reads it: [[self+8]+0x78] -> [+0] -> the controller at +0x78; its setting at [[ctrl+0x20]+0x10]+0x138
    if (!read_u8(g_input_flag, flag) || !flag || !read_ptr(self + 8, a) || !read_ptr(a + 0x78, b) || !read_ptr(b, b) ||
        !read_ptr(b + 0x78, ctrl) || !read_ptr(ctrl + 0x20, list) || !read_ptr(list + 0x10, cfg) ||
        !read_u8(cfg + 0x138, cfg_on) || !cfg_on || !read_u8(ctrl + kOffReleased, released))
        return;
    if (!released) {
        g_block_layer = -1; // (captured: the game's own release no longer stands)
        return;
    }
    if (g_block_layer.load() == layer) return;
    g_mouse = ctrl;
    bool focused = false;
    if (call_capture(ctrl, focused) && focused) LOG("camera mode restored (gameplay, the mouse was free)");
}

// --- the window procedure -------------------------------------------------------------------------------------------
constexpr ULONG_PTR kWheelMark = 0x4D57464Cu; // (the mod's own wheel events)
constexpr UINT_PTR kWheelTimer = 0x4C42;
SHORT g_wheel_pending = 0;

LRESULT CALLBACK fix_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = orig_of(h);
    if (!orig) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCHITTEST: {
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        if (r >= HTLEFT && r <= HTBOTTOMRIGHT && borderless(h)) {
            static LRESULT last = 0;
            if (r != last) LOG("resize border (%s) ignored: borderless", hit_name(r));
            last = r;
            return HTCLIENT;
        }
        return r;
    }
    case WM_SYSCOMMAND: {
        UINT cmd = static_cast<UINT>(w & 0xFFF0);
        if ((cmd == SC_SIZE || cmd == SC_MOVE) && borderless(h)) {
            LOG("system command %s ignored: borderless", cmd == SC_SIZE ? "Size" : "Move");
            return 0;
        }
        break;
    }
    case WM_SETCURSOR: {
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        if (LOWORD(l) != HTCLIENT || !borderless(h)) return r;
        if (!g_freed_once.load() || g_blank.load()) { // (the intro and the title art; between the HUD's release and ours)
            set_cursor_own(nullptr);
            return TRUE;
        }
        // (the game left it to Windows and the arrow went up: once the world has loaded, the game's own cursor - the
        // title screen keeps Windows' answer)
        if (HCURSOR gc = g_game_cursor.load(); gc && g_input.load()) {
            set_cursor_own(gc);
            return TRUE;
        }
        return r;
    }
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_KILLFOCUS: {
        bool losing = (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || (m == WM_ACTIVATEAPP && !w) || m == WM_KILLFOCUS;
        if (m == WM_ACTIVATE && !losing) {
            if (borderless(h)) g_main = h;
            if (g_was_captured) PostMessageW(h, kRecaptureMsg, 0, 0); // (after the activation has gone through)
        }
        if (!losing) break;
        ++g_deactivating;
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        --g_deactivating;
        return r;
    }
    case WM_MOUSEWHEEL: {
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        if (GetMessageExtraInfo() != static_cast<LPARAM>(kWheelMark)) {
            if (now - last > 400 && borderless(h)) { // (a lone notch: its second event, a few frames later)
                g_wheel_pending = static_cast<SHORT>(HIWORD(w));
                SetTimer(h, kWheelTimer, 50, nullptr);
            }
            last = now;
        }
        break;
    }
    case WM_TIMER:
        if (w == kWheelTimer) {
            KillTimer(h, kWheelTimer);
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = static_cast<DWORD>(static_cast<int>(g_wheel_pending));
            in.mi.dwExtraInfo = kWheelMark;
            SendInput(1, &in, sizeof in);
            return 0;
        }
        break;
    case WM_NCDESTROY: {
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        std::lock_guard<std::mutex> lock(g_mu);
        g_orig.erase(h);
        return r;
    }
    default:
        if (m == kRecaptureMsg) {
            if (w || g_was_captured) recapture(w ? "the HUD appeared" : "back in front");
            if (w) g_blank = false;
            else g_was_captured = false;
            return 0;
        }
        break;
    }
    return CallWindowProcW(orig, h, m, w, l);
}

void wrap(HWND h)
{
    if (!h || GetParent(h) || (GetWindowLongW(h, GWL_STYLE) & WS_CHILD)) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_orig.count(h)) return;
        g_orig[h] = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_WNDPROC));
    }
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&fix_proc));
}

using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExWFn g_create_orig = nullptr;

HWND WINAPI create_hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                        HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND wnd = g_create_orig(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (wnd && !parent && !(style & WS_CHILD)) wrap(wnd);
    return wnd;
}

BOOL CALLBACK wrap_existing(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) wrap(h);
    return TRUE;
}

// --- finding the game's code ----------------------------------------------------------------------------------------
const std::uint8_t* g_exe = nullptr;

// Calls fn(section start, size) for each executable section of the game's exe.
template <class F>
void each_code_section(F fn)
{
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) fn(g_exe + sec->VirtualAddress, size_t(sec->Misc.VirtualSize));
}

bool match(const std::uint8_t* p, const std::uint8_t* pat, const char* mask, size_t n)
{
    for (size_t j = 0; j < n; ++j)
        if (mask[j] == 'x' && p[j] != pat[j]) return false;
    return true;
}

unsigned long long rva(UPTR a) { return static_cast<unsigned long long>(a - reinterpret_cast<UPTR>(g_exe)); }

// The release: mov [rsp+18],rbx; mov [rsp+10],rdx; push rdi; sub rsp,20; mov rdi,rcx; mov byte [rcx+50],1; xor ecx,ecx;
// mov rbx,rdx; call [rip+x] (ClipCursor(nullptr)). One match on every build kept (1.0.0.2976 to 26480954).
UPTR find_release()
{
    const std::uint8_t pat[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                0x48, 0x8B, 0xF9, 0xC6, 0x41, 0x50, 0x01, 0x33, 0xC9, 0x48, 0x8B, 0xDA, 0xFF, 0x15};
    UPTR hit = 0;
    int hits = 0;
    each_code_section([&](const std::uint8_t* p, size_t n) {
        __try {
            for (size_t k = 0; k + sizeof pat <= n; ++k)
                if (p[k] == 0x48 && std::memcmp(p + k, pat, sizeof pat) == 0) hit = reinterpret_cast<UPTR>(p + k), ++hits;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
    if (hits != 1) {
        LOG("the game's mouse release: %d matches (need one): the camera fixes are off", hits);
        return 0;
    }
    LOG("the game's mouse release at +%llX", rva(hit));
    return hit;
}

// The HUD's release call: a function starting cmp byte [rip+x],0; je; mov rbx,[rcx+78]; cmp dword [rbx+28],0 with, at
// +0x41, cmp byte [rbx+50],1; je; mov rax,[rbx]; mov rdx,-1; mov rcx,rbx; call [rax+48]; mov byte [rbx+50],1 (the call
// returns 26 bytes in). One match on every build kept.
UPTR find_hud_release()
{
    const std::uint8_t a[] = {0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x0F, 0x84, 0, 0, 0, 0, 0x48, 0x8B, 0x59, 0x78, 0x83, 0x7B, 0x28, 0x00};
    const char am[] = "xx????xxx????xxxxxxxx";
    const std::uint8_t b[] = {0x80, 0x7B, 0x50, 0x01, 0x0F, 0x84, 0, 0, 0, 0, 0x48, 0x8B, 0x03, 0x48, 0xC7, 0xC2, 0xFF,
                              0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x48, 0xC6, 0x43, 0x50, 0x01};
    const char bm[] = "xxxxxx????xxxxxxxxxxxxxxxxxxxx";
    UPTR hit = 0;
    int hits = 0;
    each_code_section([&](const std::uint8_t* p, size_t n) {
        __try {
            for (size_t k = 0; k + 0x41 + sizeof b <= n; ++k)
                if (p[k] == 0x80 && match(p + k, a, am, sizeof a) && match(p + k + 0x41, b, bm, sizeof b))
                    hit = reinterpret_cast<UPTR>(p + k + 0x41 + 26), ++hits;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
    if (hits != 1) {
        LOG("the game's HUD mouse release: %d matches (need one): the camera fix after loading is off", hits);
        return 0;
    }
    LOG("the game's HUD mouse release returns to +%llX", rva(hit));
    return hit;
}

// The input function: anchored on its capture rule, mov ecx,0x2B; bt ecx,r32 (the capturing layers: one match on every
// build kept), with the layer's offset and the mouse-handling global read from the code before it (cmp byte
// [rbx+d8],r8b; jne short; cmp byte [rbx+d8+2],r8b, then cmp byte [rip+x],0). Its start from the exception table.
UPTR find_input()
{
    const std::uint8_t anchor[] = {0xB9, 0x2B, 0x00, 0x00, 0x00, 0x44, 0x0F, 0xA3};
    const std::uint8_t* hit = nullptr;
    int hits = 0;
    each_code_section([&](const std::uint8_t* p, size_t n) {
        __try {
            for (size_t k = 0x80; k + sizeof anchor <= n; ++k)
                if (p[k] == 0xB9 && std::memcmp(p + k, anchor, sizeof anchor) == 0) hit = p + k, ++hits;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
    if (hits != 1) {
        LOG("the game's input function: %d matches (need one): the camera fix in gameplay is off", hits);
        return 0;
    }
    auto is_cmp = [](const std::uint8_t* q) { return q[0] == 0x44 && q[1] == 0x38 && (q[2] & 0xC7) == 0x43; };
    int layer = -1;
    UPTR flag = 0;
    __try {
        for (const std::uint8_t* q = hit - 0x80; q + 10 <= hit && layer < 0; ++q)
            if (is_cmp(q) && q[4] == 0x75 && is_cmp(q + 6) && q[9] == std::uint8_t(q[3] + 2)) {
                layer = q[3];
                for (const std::uint8_t* g = q + 10; g + 7 <= hit; ++g)
                    if (g[0] == 0x80 && g[1] == 0x3D && g[6] == 0x00) {
                        flag = reinterpret_cast<UPTR>(g) + 7 + *reinterpret_cast<const std::int32_t*>(g + 2);
                        break;
                    }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DWORD64 image = 0;
    auto fe = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(hit), &image, nullptr);
    UPTR start = fe ? static_cast<UPTR>(image + fe->BeginAddress) : 0;
    if (layer < 0 || !flag || !start || start >= reinterpret_cast<UPTR>(hit) || reinterpret_cast<UPTR>(hit) - start > 0x4000) {
        LOG("the game's input function: unexpected code: the camera fix in gameplay is off");
        return 0;
    }
    g_off_layer = static_cast<size_t>(layer);
    g_input_flag = flag;
    LOG("the game's input function at +%llX (input layer at +0x%X)", rva(start), layer);
    return start;
}

DWORD WINAPI init_thread(LPVOID)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring path = buf;
    path.resize(path.find_last_of(L"/\\") + 1);
    logx::open((path + L"MouseWindowFixes.log").c_str());
    LOG("Mouse & Window Fixes 1.0.0 started");
    g_exe = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (MH_Initialize() != MH_OK) {
        LOG("MinHook failed: inactive");
        return 0;
    }
    HMODULE u = GetModuleHandleW(L"user32.dll");
    bool ok = MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "CreateWindowExW")), &create_hook,
                            reinterpret_cast<void**>(&g_create_orig)) == MH_OK;
    ok &= MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "SetCursor")), &setcursor_hook,
                        reinterpret_cast<void**>(&g_setcursor_orig)) == MH_OK;
    if ((g_release_addr = find_release()) != 0 &&
        MH_CreateHook(reinterpret_cast<void*>(g_release_addr), &release_hook, reinterpret_cast<void**>(&g_release_orig)) != MH_OK) {
        LOG("the mouse release hook failed: the camera fixes are off");
        g_release_addr = 0;
    }
    if (g_release_addr) {
        g_hud_ret = find_hud_release();
        if (UPTR f = find_input())
            if (MH_CreateHook(reinterpret_cast<void*>(f), &input_hook, reinterpret_cast<void**>(&g_input_orig)) != MH_OK)
                LOG("the input hook failed: the camera fix in gameplay is off");
    }
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
