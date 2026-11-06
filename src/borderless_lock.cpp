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
//    ShowCursor's count is kept between -1 (hidden) and 0 (shown), so the game's own capture after ours can't hide
//    the cursor twice and keep it hidden in the next menu.
// 4. Loads (v5): the game captures the mouse only when its input layer CHANGES to a gameplay one (its per-frame input
//    function: layers 0, 1, 3, 5 capture), so after loading into the world (the mouse free from the title menus, the
//    layer already gameplay) it stayed free until Kliff moved. The mod hooks that function (found by its code) and
//    applies the same rule as a state: gameplay layer + focused + mouse free -> capture. Menus free the mouse in their
//    own layers (4, 7: not capturing), so they're never touched. v6: the release when Kliff appears after a load comes
//    in gameplay layer 1 and is overridden too; only if the game keeps re-freeing it in a gameplay layer (3 times in
//    2 s) does the mod stand back until the layer changes.
//
// Log: BorderlessLock.log next to it.

#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    // (kept between -1 hidden and 0 shown: a second capture or release must not stack)
    if (n > 0) n = g_showcursor_orig(FALSE);
    else if (n < -1) n = g_showcursor_orig(TRUE);
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
        bool capturing = layer <= 5 && ((0x2B >> layer) & 1);
        bool repeated = capturing && times[0] && now - times[0] < 2000;
        g_block_layer = !capturing || repeated ? layer : -1;
        LOG("the game released the mouse itself (input layer %u)%s", layer,
            !capturing ? "" : repeated ? ": again and again, left free until the layer changes" : ": overridden");
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
    g_release_orig(obj, pos);
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

void __fastcall input_hook(UPTR self)
{
    g_input_orig(self);
    g_input = self;
    std::uint8_t layer = 0xFF, flag = 0, released = 0, cfg_on = 0;
    if (!read_u8(self + kOffLayer, layer)) return;
    static int last_layer = -2;
    if (layer != last_layer) {
        LOG("DIAG input layer %d -> %u", last_layer, layer);
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
    if (call_capture(ctrl, focused) && focused) LOG("camera mode restored (gameplay input layer %u, the mouse was free)", layer);
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
        if (LOWORD(l) == HTCLIENT && borderless(h)) { // (the game left it to Windows: the arrow went up)
            if (HCURSOR gc = g_game_cursor.load()) {
                t_own = true;
                SetCursor(gc);
                t_own = false;
                if (++g_replaced % 500 == 1)
                    LOG("the Windows arrow replaced by the game's cursor (%u times so far)", g_replaced.load());
            }
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
    case WM_NCDESTROY: {
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        std::lock_guard<std::mutex> lock(g_mu);
        g_orig.erase(h);
        return r;
    }
    default:
        if (m == kRecaptureMsg) {
            if (w || g_was_captured) recapture(w ? "after a load" : "back in front");
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

DWORD WINAPI init_thread(LPVOID)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring path = buf;
    path.resize(path.find_last_of(L"/\\") + 1);
    logx::open((path + L"BorderlessLock.log").c_str());
    LOG("Borderless Lock v6 started");
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
