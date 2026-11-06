// Borderless Lock (Crimson Desert, 2026-11-06), v4.
//
// 1. Resize: the game's borderless window answers Windows' hit-test with an invisible resize border (about 8 px: the
//    top-left corner 13, left edge 10, top 12, bottom-right 17), and the cursor starts on that corner; a click and drag
//    there resized the window and the game dropped to windowed mode. While the window covers its monitor those answers
//    become "client", and the system Size / Move commands are ignored. Windowed mode is untouched.
// 2. The arrow: the game never answers WM_SETCURSOR, so on every click / move Windows put up the window's default
//    arrow until the game's next SetCursor covered it (right clicks in menus: until release; the outer strip of the
//    screen: always). Over the game the mod answers it with the game's last own cursor.
// 3. Cursor mode: the game's mouse controller (an object: +0x40 the window, +0x50 1 = released / cursor mode,
//    0 = captured / camera mode; vtable slot 0x28 "focused", 0x40 capture(pos), 0x48 release(pos)) releases the mouse
//    when the game loses focus and when a load ends, and captures it again only when Kliff moves: until then the
//    mouse drove a cursor, not the camera. The mod finds release by its code (one match in build 26480954), hooks it,
//    and after the game comes back in front (if it was in camera mode when it lost focus) or after a load, calls the
//    object's own capture, as the game does when Kliff moves. Before each call it checks the object's vtable still
//    holds that release (a patch that changes the object turns this part off, nothing else).
//    The game's ShowCursor count is left alone (v7: clamping it broke the title menu's cursor, which the game shows
//    with counts above 0); its own capture skips itself once the mouse is captured, so ours can't stack.
// 5. v12, mapped with the user's F10 markers: the world is input layer 4, its menus too (the title is layer 1, the
//    Esc menu 5). When the HUD appears, one function frees the mouse (the same one does for the title prompts and the
//    Esc menu); the inventory and map free it through other code. The mod finds that function by its code (one
//    match: it checks the global mouse flag, then the controller at [this+0x78]) and, when it frees the mouse in
//    layer 4, captures it back on the next frame. And until the game frees the mouse for the first time (the title
//    prompts appearing) there is no cursor over the game (the intro and the title art).
//
// Log: BorderlessLock.log next to it.

#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>

typedef unsigned __int64 QWORD; // (NEXTRAWINPUTBLOCK uses it)
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

// --- the cursor -----------------------------------------------------------------------------------------------------
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
using ShowCursorFn = int(WINAPI*)(BOOL);
SetCursorFn g_setcursor_orig = nullptr;
ShowCursorFn g_showcursor_orig = nullptr;
thread_local bool t_own = false;             // (our own SetCursor: not the game's choice)
std::atomic<HCURSOR> g_game_cursor{nullptr}; // the game's last own cursor (not the arrow, not none)
std::atomic<HCURSOR> g_last_set{nullptr};    // the last cursor the game set (a load's end: the busy cursor)
std::atomic<unsigned> g_replaced{0};

HCURSOR WINAPI setcursor_hook(HCURSOR c)
{
    HCURSOR prev = g_setcursor_orig(c);
    if (t_own) return prev;
    static const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    if (c && c != arrow) g_game_cursor = c;
    g_last_set = c;
    return prev;
}

int WINAPI showcursor_hook(BOOL show)
{
    int n = g_showcursor_orig(show);
    if (!t_own) { // DIAG (v7): every show / hide, the count, and the two callers above it (+RVA in the exe)
        void* fr[3] = {};
        USHORT k = CaptureStackBackTrace(1, 3, fr, nullptr);
        static const UPTR exe = reinterpret_cast<UPTR>(GetModuleHandleW(nullptr));
        char where[64] = "";
        int pos = 0;
        for (USHORT i = 0; i < k && pos < 50; ++i)
            pos += std::snprintf(where + pos, sizeof where - pos, " +%llX",
                                 static_cast<unsigned long long>(reinterpret_cast<UPTR>(fr[i]) - exe));
        LOG("DIAG ShowCursor(%s) -> count %d (%s) from%s", show ? "TRUE" : "FALSE", n, n >= 0 ? "shown" : "hidden", where);
    }
    return n;
}

// --- cursor mode: the game's mouse controller -----------------------------------------------------------------------
using MouseFn = void(__fastcall*)(UPTR obj, U64 pos);
using FocusedFn = bool(__fastcall*)(UPTR obj);
constexpr size_t kOffReleased = 0x50;
constexpr size_t kSlotFocused = 0x28 / 8, kSlotCapture = 0x40 / 8, kSlotRelease = 0x48 / 8;
MouseFn g_release_orig = nullptr;
UPTR g_release_addr = 0;      // the release function found by its code
std::atomic<UPTR> g_mouse{0}; // the controller (seen in its release calls)
int g_deactivating = 0;       // (window thread: inside the game's handling of losing focus)
bool g_was_captured = false;  // the game was in camera mode when it lost focus
UPTR g_hud_ret = 0;                     // the return address of the HUD's release call (see 5.)
std::atomic<bool> g_freed_once{false};  // the game has freed the mouse itself at least once (the title prompts)
std::atomic<bool> g_blank{false};       // between the HUD's release and our capture: no cursor image (v13: no flash)
std::atomic<UPTR> g_input{0}; // the game's input manager (its per-frame function's this; +0x30 the input layer)
std::atomic<int> g_block_layer{-1}; // the game freed the mouse itself in this layer: not ours to capture (-1: none)
constexpr size_t kOffLayer = 0x30;
constexpr UINT kRecaptureMsg = WM_APP + 0x4C42; // (posted to the main window: back to camera mode; wParam 1 = a load)

bool read_u8(UPTR a, std::uint8_t& out)
{
    __try {
        out = *reinterpret_cast<volatile std::uint8_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall release_hook(UPTR obj, U64 pos)
{
    UPTR ret = reinterpret_cast<UPTR>(_ReturnAddress());
    std::uint8_t released = 1;
    read_u8(obj + kOffReleased, released);
    g_mouse = obj;
    if (!g_deactivating) { // (the game's own choice, a menu or a load: respected while its layer lasts)
        std::uint8_t layer = 0xFF;
        if (UPTR in = g_input.load()) read_u8(in + kOffLayer, layer);
        // (a release in a capturing layer is a load's leftover: overridden, unless it keeps coming)
        static ULONGLONG times[3] = {};
        ULONGLONG now = GetTickCount64();
        times[0] = times[1], times[1] = times[2], times[2] = now;
        bool capturing = false; // (v11: none overridden - layer 1 turned out to be the title screen; layers being mapped)
        bool repeated = capturing && times[0] && now - times[0] < 2000;
        g_block_layer = !capturing || repeated ? layer : -1;
        LOG("the game released the mouse itself (input layer %u)%s", layer,
            !capturing ? "" : repeated ? ": again and again, left free until the layer changes" : ": overridden");
        g_freed_once = true;
        if (g_hud_ret && ret == g_hud_ret && layer == 4 && g_main) { // (the HUD appearing in the world: see 5.)
            LOG("  (the HUD appeared: camera mode back on the next frame)");
            g_blank = true;
            PostMessageW(g_main, kRecaptureMsg, 1, 0); // (after the caller has set its own released flag)
        }
    }
    if (g_deactivating) {
        g_was_captured = released == 0;
        LOG("the game released the mouse (lost focus; it was in %s mode)", released ? "cursor" : "camera");
    } else {
        static const HCURSOR wait = LoadCursorW(nullptr, IDC_WAIT), starting = LoadCursorW(nullptr, IDC_APPSTARTING);
        HCURSOR last = g_last_set.load();
        if (last == wait || last == starting) { // (a load ended: the busy cursor, then the release)
            LOG("the game released the mouse at a load's end");
            if (g_main) PostMessageW(g_main, kRecaptureMsg, 1, 0);
        }
    }
    { // (v19: the saved spot is 0,0 until the cursor was first placed - the top left; the middle instead, whichever
      // release comes first: the title prompts, or losing focus when the game started in the background)
        RECT r{};
        __try {
            HWND w = *reinterpret_cast<HWND volatile*>(obj + 0x40); // (the controller's own window)
            auto sx = reinterpret_cast<volatile std::int32_t*>(obj + 0x48), sy = reinterpret_cast<volatile std::int32_t*>(obj + 0x4C);
            if (*sx == 0 && *sy == 0 && IsWindow(w) && GetClientRect(w, &r)) {
                // (v20: on the title logo's ring - measured at 716,381 on a 1366x768 screenshot: the logo scales with
                // the height, so the offset from the middle is in heights: +0.043 across, 0.496 down)
                LONG W = r.right - r.left, H = r.bottom - r.top;
                // (v22: +0.046: a capture put v21 4 px left of the ring at 1440p. v21: the title cursor, whose point is its tip and whose body hangs down-right of it:
                // shifted up-left by about half the arrow - 17% / 30% of the cursor size - so the arrow covers the ring)
                LONG cx = GetSystemMetrics(SM_CXCURSOR), cy = GetSystemMetrics(SM_CYCURSOR);
                *sx = r.left + W / 2 + LONG(H * 0.046) - LONG(cx * 0.17), *sy = r.top + LONG(H * 0.496) - LONG(cy * 0.30);
                LOG("the cursor's first spot: the title ring (%ld,%ld)", static_cast<long>(*sx), static_cast<long>(*sy));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_release_orig(obj, pos);
    if (g_blank.load()) { // (the HUD's release just showed the cursor: blank its image until the capture)
        t_own = true;
        SetCursor(nullptr);
        t_own = false;
    }
}

// Calls the controller's own checks and capture, as the game does when Kliff moves. Only with the expected vtable.
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
    if (!obj || !read_u8(obj + kOffReleased, released)) {
        LOG("camera mode not restored (%s): the mouse controller isn't known yet", why);
        return;
    }
    if (!released) return; // (already in camera mode)
    bool focused = false;
    if (!call_capture(obj, focused)) {
        LOG("camera mode not restored (%s): the mouse controller isn't the expected one (a game update?)", why);
        return;
    }
    if (focused) LOG("camera mode restored (%s)", why);
    else LOG("camera mode not restored (%s): the game isn't focused yet", why);
}

// --- loads: the game's per-frame input function -------------------------------------------------------------------
using InputFn = void(__fastcall*)(UPTR self);
InputFn g_input_orig = nullptr;
UPTR g_input_flag = 0; // the global the function checks before handling the mouse (cmp byte [rip+x],0)

bool read_ptr(UPTR a, UPTR& out)
{
    __try {
        out = *reinterpret_cast<volatile UPTR*>(a);
        return out > 0x10000 && out < 0x7FFFFFFF0000ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// DIAG (v9): F10 writes the input objects to the log (hex): the input function's object, the object at its +8, the
// one that holds the mouse controller, the controller itself. Snapshots taken locked / unlocked show what a direction
// key changes.
bool dump_block(const char* name, UPTR a, size_t n)
{
    if (!a) return false;
    LOG("DIAG snapshot %s at %llX:", name, static_cast<unsigned long long>(a));
    char line[200];
    for (size_t o = 0; o < n; o += 32) {
        int pos = std::snprintf(line, sizeof line, "  +%03zX:", o);
        for (size_t i = 0; i < 32; ++i) {
            std::uint8_t b = 0;
            if (!read_u8(a + o + i, b)) {
                LOG("%s (unreadable after this)", line);
                return true;
            }
            pos += std::snprintf(line + pos, sizeof line - pos, i % 8 ? "%02X" : " %02X", b);
        }
        LOG("%s", line);
    }
    return true;
}

void snapshot(UPTR self)
{
    static int count = 0;
    LOG("DIAG ===== snapshot %d =====", ++count);
    UPTR a = 0, b = 0, ctrl = 0;
    dump_block("input function's object", self, 0x400);
    if (read_ptr(self + 8, a)) dump_block("its +8", a, 0x100);
    if (a && read_ptr(a + 0x78, b) && read_ptr(b, b)) {
        dump_block("the controller's holder", b, 0x100);
        if (read_ptr(b + 0x78, ctrl)) dump_block("the mouse controller", ctrl, 0x100);
    }
}

std::atomic<bool> g_kick{false}; // a load's release was overridden: move the input layer to gameplay (0) once
constexpr size_t kOffLayerFlags = 0x3A; // 7 flags, one per layer: the first one set is the layer

void __fastcall input_hook(UPTR self)
{
    if (g_kick.exchange(false)) {
        std::uint8_t layer = 0xFF, f[7] = {};
        read_u8(self + kOffLayer, layer);
        for (int i = 0; i < 7; ++i) read_u8(self + kOffLayerFlags + i, f[i]);
        LOG("DIAG input layer %u, flags %u %u %u %u %u %u %u", layer, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
        if (layer == 1) {
            __try {
                *reinterpret_cast<volatile std::uint8_t*>(self + kOffLayerFlags) = 1;
                LOG("input layer set to gameplay (0) after the load, as Kliff's first step does");
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    g_input_orig(self);
    g_input = self;
    { // DIAG (v9): F10 = snapshot
        static bool was = false;
        bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down && !was) { // (v11: a marker: the user presses it at each screen, to map the layers)
            static int n = 0;
            std::uint8_t layer = 0xFF, rel = 0xFF;
            read_u8(self + kOffLayer, layer);
            UPTR ctrl = g_mouse.load();
            if (ctrl) read_u8(ctrl + kOffReleased, rel);
            CURSORINFO ci{sizeof ci};
            GetCursorInfo(&ci);
            LOG("MARKER %d (a scroll that didn't zoom): input layer %u, mouse %s, Windows cursor %s", ++n, layer,
                rel == 1 ? "free (cursor mode)" : rel == 0 ? "captured (camera mode)" : "unknown",
                (ci.flags & CURSOR_SHOWING) ? "showing" : "hidden");
        }
        was = down;
    }
    std::uint8_t layer = 0xFF, flag = 0, released = 0, cfg_on = 0;
    if (!read_u8(self + kOffLayer, layer)) return;
    { // DIAG (v16): the input function's second value (+0x32, picked from 18 flags at +0x41) and its last-frame
      // copy (+0x31): when they change the game rebuilds its key mapping; a wheel notch lost there would explain
      // the missed zooms
        static int last_sub = -2, last_31 = -2;
        std::uint8_t sub = 0xFF, b31 = 0xFF, f[18] = {};
        read_u8(self + 0x32, sub);
        read_u8(self + 0x31, b31);
        if (sub != last_sub || b31 != last_31) {
            for (int i = 0; i < 18; ++i) read_u8(self + 0x41 + i, f[i]);
            char fl[64] = "";
            for (int i = 0; i < 18; ++i) fl[i] = f[i] ? char('0' + (f[i] > 9 ? 9 : f[i])) : '.';
            fl[18] = 0;
            LOG("DIAG input +32 %d -> %u, +31 %d -> %u (flags +41..: %s)", last_sub, sub, last_31, b31, fl);
            last_sub = sub, last_31 = b31;
        }
    }
    static int last_layer = -2;
    if (layer != last_layer) {
        std::uint8_t f[7] = {};
        for (int i = 0; i < 7; ++i) read_u8(self + 0x3A + i, f[i]);
        LOG("DIAG input layer %d -> %u (flags %u %u %u %u %u %u %u)", last_layer, layer, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
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
    if (call_capture(ctrl, focused) && focused) {
        LOG("camera mode restored (gameplay input layer %u, the mouse was free)", layer);
        // (v11: no layer kick: layer 1 is the title screen)
    }
}

// --- DIAG (v14): the mouse wheel as the game reads it --------------------------------------------------------------
// (vanilla: zooming sometimes needs two or more notches) Every wheel event, by route: raw input (each read of an event
// counted: a game may read the same one several times), the raw input buffer, the window message.
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetRawInputDataFn g_rid_orig = nullptr;
GetRawInputBufferFn g_rib_orig = nullptr;
std::atomic<unsigned> g_wheel_n{0};
constexpr ULONG_PTR kWheelMark = 0x4D57464Cu; // (the mod's own wheel events)
constexpr UINT_PTR kWheelTimer = 0x4C42;
SHORT g_wheel_pending = 0;

UINT WINAPI rid_hook(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_rid_orig(h, cmd, data, size, header);
    if (cmd == RID_INPUT && data && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER)) {
        __try {
            auto in = static_cast<RAWINPUT*>(data);
            if (in->header.dwType == RIM_TYPEMOUSE && (in->data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
                static HRAWINPUT last = nullptr;
                static int reads = 0;
                reads = h == last ? reads + 1 : 1;
                last = h;
                static int stacks = 0; // (v15: who reads it: the first 8 notches, every read)
                char where[160] = "";
                if (stacks < 32) {
                    ++stacks;
                    void* fr[6] = {};
                    USHORT k = CaptureStackBackTrace(1, 6, fr, nullptr);
                    static const UPTR exe = reinterpret_cast<UPTR>(GetModuleHandleW(nullptr));
                    int pos = 0;
                    for (USHORT i = 0; i < k && pos < 140; ++i)
                        pos += std::snprintf(where + pos, sizeof where - pos, " +%llX",
                                             static_cast<unsigned long long>(reinterpret_cast<UPTR>(fr[i]) - exe));
                }
                LOG("DIAG wheel (raw input %p, read %d): %d%s%s", static_cast<void*>(h), reads,
                    static_cast<SHORT>(in->data.mouse.usButtonData), where[0] ? " from" : "", where);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return r;
}

UINT WINAPI rib_hook(PRAWINPUT data, PUINT size, UINT header)
{
    UINT r = g_rib_orig(data, size, header);
    if (data && r != UINT(-1) && r > 0) {
        __try {
            PRAWINPUT in = data;
            for (UINT i = 0; i < r; ++i) {
                if (in->header.dwType == RIM_TYPEMOUSE && (in->data.mouse.usButtonFlags & RI_MOUSE_WHEEL))
                    LOG("DIAG wheel (raw input buffer, %u events): %d", r, static_cast<SHORT>(in->data.mouse.usButtonData));
                in = NEXTRAWINPUTBLOCK(in);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return r;
}

// --- the window procedure -------------------------------------------------------------------------------------------
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
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        if ((!g_freed_once.load() || g_blank.load()) && LOWORD(l) == HTCLIENT && borderless(h)) { // (no cursor, v12/v13)
            t_own = true;
            SetCursor(nullptr);
            t_own = false;
            return TRUE;
        }
        HCURSOR gc = g_game_cursor.load();
        // (the game left it to Windows: the arrow went up. Before the game has a cursor of its own - the title menu
        // uses the plain arrow - Windows' answer stays, v8)
        // (v10: and only once the world has loaded - the gameplay input function has run. The game's own cursors
        // are blank, its menus draw their own; the title screen shows Windows' arrow and must keep it)
        if (gc && g_input.load() && LOWORD(l) == HTCLIENT && borderless(h)) {
            t_own = true;
            SetCursor(gc);
            t_own = false;
            if (++g_replaced % 500 == 1)
                LOG("the Windows arrow replaced by the game's cursor (%u times so far)", g_replaced.load());
            r = TRUE;
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
        // (v18: the game zooms only on a second wheel event within a short window; a lone notch - none in the last
        // 400 ms - gets a second event right after it, marked so it isn't doubled again)
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        bool ours = GetMessageExtraInfo() == static_cast<LPARAM>(kWheelMark);
        LOG("DIAG wheel (window message): %d%s", static_cast<SHORT>(HIWORD(w)), ours ? " (the mod's second event)" : "");
        if (!ours) {
            if (now - last > 400 && borderless(h)) { // (v19: 50 ms later, a few frames on: two in one frame count once)
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
            if (w) g_blank = false; // (captured: the cursor is hidden by the game's own count now)
            if (!w) g_was_captured = false;
            return 0;
        }
        break;
    }
    return CallWindowProcW(orig, h, m, w, l);
}

void wrap(HWND h, const char* why)
{
    if (!h || GetParent(h) || (GetWindowLongW(h, GWL_STYLE) & WS_CHILD)) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_orig.count(h)) return;
        g_orig[h] = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_WNDPROC));
    }
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&lock_proc));
    wchar_t cls[128] = L"";
    GetClassNameW(h, cls, 128);
    LOG("wrapped window %p (%s, class %ls)", static_cast<void*>(h), why, cls);
}

using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExWFn g_create_orig = nullptr;

HWND WINAPI create_hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                        HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND wnd = g_create_orig(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (wnd && !parent && !(style & WS_CHILD)) wrap(wnd, "created");
    return wnd;
}

BOOL CALLBACK wrap_existing(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) wrap(h, "already there");
    return TRUE;
}

// --- finding the release function -----------------------------------------------------------------------------------
// mov [rsp+18],rbx; mov [rsp+10],rdx; push rdi; sub rsp,20; mov rdi,rcx; mov byte [rcx+50],1; xor ecx,ecx;
// mov rbx,rdx; call [rip+x] (ClipCursor(nullptr)): one match in the exe's code (build 26480954).
const std::uint8_t kReleaseCode[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC,
                                     0x20, 0x48, 0x8B, 0xF9, 0xC6, 0x41, 0x50, 0x01, 0x33, 0xC9, 0x48, 0x8B, 0xDA, 0xFF,
                                     0x15};

int scan_section(const std::uint8_t* p, size_t n, UPTR& found)
{
    int hits = 0;
    __try {
        for (size_t k = 0; k + sizeof kReleaseCode <= n; ++k)
            if (p[k] == 0x48 && std::memcmp(p + k, kReleaseCode, sizeof kReleaseCode) == 0)
                found = reinterpret_cast<UPTR>(p + k), ++hits;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return hits;
}

UPTR find_release()
{
    auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    UPTR found = 0;
    int hits = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && sec->Misc.VirtualSize >= sizeof kReleaseCode)
            hits += scan_section(base + sec->VirtualAddress, sec->Misc.VirtualSize, found);
    if (hits != 1) {
        LOG("the game's mouse release code: %d matches (need exactly one): cursor mode fix off", hits);
        return 0;
    }
    LOG("the game's mouse release code at +%llX", static_cast<unsigned long long>(found - reinterpret_cast<UPTR>(base)));
    return found;
}

// cmp [rbx+30],r14b; jne +a; cmp [rbx+32],r15b; je rel32; cmp byte [rip+x],0 - one match (build 26480954), inside
// the input function; its start from the exception table, checked against its prologue.
UPTR find_input()
{
    auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t pat[] = {0x44, 0x38, 0x73, 0x30, 0x75, 0, 0x44, 0x38, 0x7B, 0x32, 0x0F, 0x84, 0, 0, 0, 0, 0x80, 0x3D};
    const bool any[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0};
    UPTR hit = 0;
    int hits = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        __try {
            for (size_t k = 0; k + sizeof pat <= n; ++k) {
                if (p[k] != 0x44) continue;
                size_t j = 0;
                while (j < sizeof pat && (any[j] || p[k + j] == pat[j])) ++j;
                if (j == sizeof pat) hit = reinterpret_cast<UPTR>(p + k), ++hits;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (hits != 1) {
        LOG("the game's input layer code: %d matches (need exactly one): the after-load fix is off", hits);
        return 0;
    }
    DWORD64 image = 0;
    auto fe = RtlLookupFunctionEntry(hit, &image, nullptr);
    const std::uint8_t prologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    UPTR start = fe ? static_cast<UPTR>(image + fe->BeginAddress) : 0;
    if (!start || std::memcmp(reinterpret_cast<void*>(start), prologue, sizeof prologue) != 0) {
        LOG("the game's input function: unexpected start: the after-load fix is off");
        return 0;
    }
    g_input_flag = hit + 16 + 7 + *reinterpret_cast<const std::int32_t*>(hit + 18); // (cmp byte [rip+x],0: 7 bytes)
    LOG("the game's input function at +%llX", static_cast<unsigned long long>(start - reinterpret_cast<UPTR>(base)));
    return start;
}

// cmp byte [rip+x],0; je rel32; mov rbx,[rcx+78]; cmp dword [rbx+28],0 at the function's start, and at +0x41
// cmp byte [rbx+50],1; je; mov rax,[rbx]; mov rdx,-1; mov rcx,rbx; call [rax+48]; mov byte [rbx+50],1 (the call returns
// 26 bytes in): one match in build 26480954.
UPTR find_hud_release()
{
    const std::uint8_t a[] = {0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x0F, 0x84, 0, 0, 0, 0, 0x48, 0x8B, 0x59, 0x78, 0x83, 0x7B, 0x28, 0x00};
    const bool aw[] = {0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::uint8_t b[] = {0x80, 0x7B, 0x50, 0x01, 0x0F, 0x84, 0, 0, 0, 0, 0x48, 0x8B, 0x03, 0x48, 0xC7, 0xC2, 0xFF,
                              0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x48, 0xC6, 0x43, 0x50, 0x01};
    const bool bw[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    UPTR hit = 0;
    int hits = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        __try {
            for (size_t k = 0; k + 0x41 + sizeof b <= n; ++k) {
                if (p[k] != 0x80) continue;
                size_t j = 0;
                while (j < sizeof a && (aw[j] || p[k + j] == a[j])) ++j;
                if (j != sizeof a) continue;
                j = 0;
                while (j < sizeof b && (bw[j] || p[k + 0x41 + j] == b[j])) ++j;
                if (j == sizeof b) hit = reinterpret_cast<UPTR>(p + k + 0x41 + 26), ++hits;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (hits != 1) {
        LOG("the game's HUD mouse release: %d matches (need exactly one): that fix is off", hits);
        return 0;
    }
    LOG("the game's HUD mouse release returns to +%llX", static_cast<unsigned long long>(hit - reinterpret_cast<UPTR>(base)));
    return hit;
}

DWORD WINAPI init_thread(LPVOID)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring path = buf;
    path.resize(path.find_last_of(L"/\\") + 1);
    logx::open((path + L"BorderlessLock.log").c_str());
    LOG("Borderless Lock v22 started");
    if (MH_Initialize() != MH_OK) {
        LOG("MinHook failed: inactive");
        return 0;
    }
    HMODULE u = GetModuleHandleW(L"user32.dll");
    bool ok = MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "CreateWindowExW")), &create_hook,
                            reinterpret_cast<void**>(&g_create_orig)) == MH_OK;
    ok &= MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "SetCursor")), &setcursor_hook,
                        reinterpret_cast<void**>(&g_setcursor_orig)) == MH_OK;
    ok &= MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "ShowCursor")), &showcursor_hook,
                        reinterpret_cast<void**>(&g_showcursor_orig)) == MH_OK;
    if ((g_release_addr = find_release()) != 0 &&
        MH_CreateHook(reinterpret_cast<void*>(g_release_addr), &release_hook,
                      reinterpret_cast<void**>(&g_release_orig)) != MH_OK) {
        LOG("the mouse release hook failed: cursor mode fix off");
        g_release_addr = 0;
    }
    g_hud_ret = find_hud_release();
    MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "GetRawInputData")), &rid_hook,
                  reinterpret_cast<void**>(&g_rid_orig));
    MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(u, "GetRawInputBuffer")), &rib_hook,
                  reinterpret_cast<void**>(&g_rib_orig));
    if (UPTR f = find_input()) {
        if (MH_CreateHook(reinterpret_cast<void*>(f), &input_hook, reinterpret_cast<void**>(&g_input_orig)) != MH_OK)
            LOG("the input hook failed: the after-load fix is off");
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
