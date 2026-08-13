// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdarg>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include "LksClient.hpp"
#include "cs2dumper/analysis/analyze.hpp"
#include "cs2dumper/memory/process.hpp"
#include "overlay.hpp"
#include "hud_window.hpp"
#include "driver_loader.hpp"
#include "modern_ui.hpp"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

HWND g_hWnd; static HWND g_hTab, g_hTabEsp;
static HWND g_hClose, g_hMin;
HWND g_hStatus;
LksClient g_Client;
std::atomic_bool g_UseKernelRead{false};
std::atomic_bool g_Running{true};
static std::thread g_DumpWorker;
static HBRUSH g_BgBrush, g_InputBrush;
static HFONT g_FontCons, g_FontSegoe, g_FontBody, g_FontSmall, g_FontSection;
static constexpr int MENU_HOTKEY_ID = 0x4C4B;
static constexpr UINT_PTR FADE_TIMER_ID = 0xFADE;
static constexpr UINT_PTR STARTUP_SOUND_TIMER_ID = 0x5A11;
static constexpr UINT_PTR EXIT_FADE_TIMER_ID = 0xE017;
static DWORD g_FadeStarted = 0;
static DWORD g_ExitStarted = 0;
static BYTE g_CurrentAlpha = 0;
static BYTE g_ExitInitialAlpha = 255;
static bool g_Closing = false;
static std::vector<BYTE> g_StartupSound;

static void PlayStartupSound(bool reversed = false) {
    constexpr int sampleRate = 44100;
    constexpr float duration = 0.72f;
    constexpr int samples = static_cast<int>(sampleRate * duration);
    constexpr int dataBytes = samples * 2;
    g_StartupSound.clear();
    g_StartupSound.reserve(44 + dataBytes);
    const auto bytes = [](auto value, int count, std::vector<BYTE>& out) {
        for (int i = 0; i < count; ++i)
            out.push_back(static_cast<BYTE>((value >> (i * 8)) & 0xFF));
    };
    const auto tag = [](const char* value, std::vector<BYTE>& out) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<BYTE>(value[i]));
    };
    tag("RIFF", g_StartupSound); bytes(36 + dataBytes, 4, g_StartupSound);
    tag("WAVE", g_StartupSound); tag("fmt ", g_StartupSound);
    bytes(16, 4, g_StartupSound); bytes(1, 2, g_StartupSound);
    bytes(1, 2, g_StartupSound); bytes(sampleRate, 4, g_StartupSound);
    bytes(sampleRate * 2, 4, g_StartupSound); bytes(2, 2, g_StartupSound);
    bytes(16, 2, g_StartupSound); tag("data", g_StartupSound);
    bytes(dataBytes, 4, g_StartupSound);
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float attack = std::min(1.0f, t / 0.055f);
        const float release = std::clamp((duration - t) / 0.42f, 0.0f, 1.0f);
        const float envelope = attack * release * release;
        const float shimmer = 0.58f * std::sin(2.0f * 3.14159265f * 523.25f * t) +
                              0.27f * std::sin(2.0f * 3.14159265f * 659.25f * t) +
                              0.15f * std::sin(2.0f * 3.14159265f * 783.99f * t);
        const short sample = static_cast<short>(shimmer * envelope * 5600.0f);
        bytes(static_cast<unsigned short>(sample), 2, g_StartupSound);
    }
    if (reversed) {
        for (int left = 44, right = 44 + dataBytes - 2; left < right;
             left += 2, right -= 2) {
            std::swap(g_StartupSound[left], g_StartupSound[right]);
            std::swap(g_StartupSound[left + 1], g_StartupSound[right + 1]);
        }
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(g_StartupSound.data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

namespace Ui {
constexpr COLORREF App = RGB(10, 12, 17);
constexpr COLORREF Rail = RGB(14, 18, 25);
constexpr COLORREF Canvas = RGB(16, 20, 27);
constexpr COLORREF Surface = RGB(22, 28, 37);
constexpr COLORREF SurfaceHover = RGB(29, 37, 49);
constexpr COLORREF Stroke = RGB(34, 43, 55);
constexpr COLORREF StrokeStrong = RGB(51, 64, 79);
constexpr COLORREF TextHi = RGB(243, 246, 251);
constexpr COLORREF Text = RGB(191, 200, 214);
constexpr COLORREF TextDim = RGB(122, 134, 152);
constexpr COLORREF Accent = RGB(30, 215, 96);
constexpr COLORREF AccentHi = RGB(77, 234, 136);
constexpr COLORREF AccentInk = RGB(4, 34, 14);
constexpr COLORREF Danger = RGB(237, 66, 69);

static void RoundFill(HDC dc, const RECT& rc, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(pen); DeleteObject(brush);
}

static void RoundFrame(HDC dc, const RECT& rc, int radius, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    const auto oldPen = SelectObject(dc, pen);
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

static bool IsCheckId(int id) {
    return (id >= 100 && id <= 111 && id != 106) ||
           (id >= 300 && id <= 311 && id != 301 && id != 302 &&
            id != 303 && id != 304 && id != 307 && id != 308 &&
            id != 309 && id != 310) || (id >= 402 && id <= 405);
}

static bool IsColorId(int id) { return id >= 200 && id <= 204; }

static COLORREF ColorForId(int id);

static bool DrawControl(const DRAWITEMSTRUCT* di) {
    if (!di || di->CtlType != ODT_BUTTON) return false;
    RECT rc = di->rcItem;
    const int id = static_cast<int>(di->CtlID);
    const bool hot = GetPropW(di->hwndItem, L"LksHot") != nullptr;
    const bool pressed = (di->itemState & ODS_SELECTED) != 0;
    SetBkMode(di->hDC, TRANSPARENT);

    if (IsColorId(id)) {
        RoundFill(di->hDC, rc, 10, Surface);
        RECT swatch = rc; InflateRect(&swatch, -4, -4);
        RoundFill(di->hDC, swatch, 8, ColorForId(id));
        RoundFrame(di->hDC, rc, 10, hot ? Accent : StrokeStrong);
        return true;
    }

    wchar_t label[128] = {};
    GetWindowTextW(di->hwndItem, label, static_cast<int>(_countof(label)));
    if (IsCheckId(id)) {
        const bool checked = SendMessageW(di->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
        RECT track{rc.right - 48, rc.top + (rc.bottom - rc.top - 24) / 2,
                   rc.right - 4, rc.top + (rc.bottom - rc.top + 24) / 2};
        SetTextColor(di->hDC, checked ? TextHi : Text);
        SelectObject(di->hDC, g_FontBody);
        RECT textRc = rc; textRc.right = track.left - 12;
        DrawTextW(di->hDC, label, -1, &textRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RoundFill(di->hDC, track, 24, checked ? Accent : (hot ? StrokeStrong : Stroke));
        RECT knob = track;
        knob.left = checked ? track.right - 21 : track.left + 3;
        knob.right = knob.left + 18; knob.top += 3; knob.bottom -= 3;
        HBRUSH kb = CreateSolidBrush(checked ? AccentInk : TextDim);
        const auto old = SelectObject(di->hDC, kb);
        Ellipse(di->hDC, knob.left, knob.top, knob.right, knob.bottom);
        SelectObject(di->hDC, old); DeleteObject(kb);
        return true;
    }

    const bool primary = id == 500;
    const bool destructive = id == 502 || id == 503;
    COLORREF fill = primary ? Accent : Surface;
    if (hot) fill = primary ? AccentHi : SurfaceHover;
    if (pressed) fill = primary ? Accent : StrokeStrong;
    RoundFill(di->hDC, rc, 10, fill);
    if (!primary) RoundFrame(di->hDC, rc, 10, destructive && hot ? Danger : StrokeStrong);
    SelectObject(di->hDC, g_FontBody);
    SetTextColor(di->hDC, primary ? AccentInk : (destructive && hot ? Danger : TextHi));
    DrawTextW(di->hDC, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return true;
}

static LRESULT CALLBACK ControlSubclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                        UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetPropW(hwnd, L"LksHot")) {
            SetPropW(hwnd, L"LksHot", reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme); InvalidateRect(hwnd, nullptr, TRUE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, L"LksHot"); InvalidateRect(hwnd, nullptr, TRUE); break;
    case WM_NCDESTROY:
        RemovePropW(hwnd, L"LksHot"); RemoveWindowSubclass(hwnd, ControlSubclass, 1); break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

static void StyleChildren(HWND parent) {
    EnumChildWindows(parent, [](HWND child, LPARAM) -> BOOL {
        wchar_t cls[32] = {}; GetClassNameW(child, cls, 32);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_FontBody), TRUE);
        if (_wcsicmp(cls, L"Button") == 0) {
            const auto style = GetWindowLongPtrW(child, GWL_STYLE);
            SetWindowLongPtrW(child, GWL_STYLE,
                (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
            SetWindowSubclass(child, ControlSubclass, 1, 0);
        } else if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"ComboBox") == 0) {
            SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        }
        return TRUE;
    }, 0);
}
} // namespace Ui

std::vector<OffsetEntry> g_Offsets;
extern EspSettings g_EspSettings;
extern AimSettings g_AimSettings;
extern MiscSettings g_MiscSettings;
extern ConfigSettings g_ConfigSettings;

static void SetStatus(const wchar_t* s) {
    SetWindowText(g_hStatus, s);
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}


static void DrawBtnIcon(HDC dc, RECT rc, int type) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
    HGDIOBJ oldBitmap = SelectObject(mem, bmp);
    SetBkColor(mem, Ui::Rail);
    RECT r = {0, 0, rc.right - rc.left, rc.bottom - rc.top};
    ExtTextOut(mem, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    int cx = r.right / 2, cy = r.bottom / 2;
    HPEN pen = CreatePen(PS_SOLID, 2, type == 0 ? Ui::Danger : Ui::TextDim);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    if (type == 0) {
        MoveToEx(mem, cx - 5, cy - 5, 0); LineTo(mem, cx + 5, cy + 5);
        MoveToEx(mem, cx + 5, cy - 5, 0); LineTo(mem, cx - 5, cy + 5);
    } else {
        MoveToEx(mem, 6, cy, 0); LineTo(mem, r.right - 6, cy);
    }
    BitBlt(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBitmap);
    DeleteObject(pen);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static bool hover[2] = {};
    int id = (int)GetWindowLongPtr(h, GWLP_ID);
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        DrawBtnIcon(dc, rc, id); EndPaint(h, &ps);
    } return 0;
    case WM_LBUTTONDOWN:
        PostMessage(GetParent(h), WM_CLOSE, 0, 0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}
static LRESULT CALLBACK MinProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        DrawBtnIcon(dc, rc, 1); EndPaint(h, &ps); } return 0;
    case WM_LBUTTONDOWN: ShowWindow(GetParent(h), SW_MINIMIZE); return 0;
    }
    return DefWindowProc(h, m, w, l);
}


static COLORREF g_TempBoxCol = RGB(0, 200, 255);
static COLORREF g_TempSkelCol = RGB(255, 200, 0);
static COLORREF g_TempSkelVisibleCol = RGB(60, 230, 110);
static COLORREF g_TempSkelHiddenCol = RGB(235, 70, 70);
static COLORREF g_TempFovCol = RGB(110, 190, 255);

COLORREF Ui::ColorForId(int id) {
    switch (id) {
    case 200: return g_TempBoxCol;
    case 201: return g_TempSkelCol;
    case 202: return g_TempFovCol;
    case 203: return g_TempSkelVisibleCol;
    default: return g_TempSkelHiddenCol;
    }
}

static LRESULT CALLBACK TabSubclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)w, &rc, g_BgBrush);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Ui::Text);
        return (LRESULT)g_BgBrush;
    }
    }
    return DefSubclassProc(hwnd, msg, w, l);
}


static LRESULT CALLBACK EspPageSubclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{}; GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_BgBrush);
        RECT card{18, 12, rc.right - 18, rc.bottom - 14};
        Ui::RoundFill(dc, card, 18, Ui::Surface);
        Ui::RoundFrame(dc, card, 18, Ui::Stroke);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)w, &rc, g_BgBrush);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Ui::Text);
        return (LRESULT)g_BgBrush;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w; SetBkColor(dc, Ui::Surface);
        SetTextColor(dc, Ui::TextHi);
        return (LRESULT)g_InputBrush;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (Ui::DrawControl(di)) return TRUE;
    } return TRUE;
    case WM_COMMAND: return SendMessage(g_hWnd, WM_COMMAND, w, l);
    }
    return DefSubclassProc(hwnd, msg, w, l);
}


static HWND g_hChkBox, g_hChkSkel, g_hChkHead, g_hChkHealth, g_hChkShield, g_hChkName, g_hChkVisible;
static HWND g_hChkEspEnable;
static HWND g_hChkWeapons, g_hChkGrenades, g_hChkBombEsp;
static HWND g_hComboStyle;
static HWND g_hBtnColorBox, g_hBtnColorSkel;
static HWND g_hBtnSkelVisible, g_hBtnSkelHidden;
static HWND g_hChkFovCircle, g_hBtnColorFov;
static HWND g_hTabAim;
static HWND g_hChkAim, g_hChkAimTeam, g_hChkAimVisible, g_hChkHumanize;
static HWND g_hBtnKey, g_hComboBone;
static HWND g_hEditFov, g_hEditSmooth, g_hEditReaction, g_hEditJitter, g_hEditEase, g_hEditCooldown;
static HWND g_hTabMisc, g_hTabConfig;
static HWND g_hChkCrosshair, g_hChkGameInfo, g_hChkBombTimer, g_hChkDamageLog;
static HWND g_hBtnSave, g_hBtnLoad, g_hBtnReset, g_hConfigPath;
bool g_ListeningForKey = false;
static bool g_KeyCaptureArmed = false;
static std::wstring AppFolder();
static std::filesystem::path ConfigFolder();
static void RefreshConfigList();

static void EspLog(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    static std::recursive_mutex logMtx;
    std::lock_guard<std::recursive_mutex> logLock(logMtx);
    static std::filesystem::path logPath;
    static bool pathReady = false;
    if (!pathReady) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p(exe);
        const size_t slash = p.find_last_of(L'\\');
        logPath = (slash == std::wstring::npos) ?
            std::filesystem::path(L"esp.log") :
            std::filesystem::path(p.substr(0, slash + 1)) / L"esp.log";
        pathReady = true;
    }
    FILE* f = _wfopen(logPath.c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fclose(f);
}
static void CreateEspControls(HWND parent) {
    auto mkchk = [&](const wchar_t* t, int id, int x, int y) {
        return CreateWindow(L"BUTTON", t, WS_CHILD | BS_AUTOCHECKBOX | WS_VISIBLE,
            x, y, 220, 24, parent, (HMENU)id, 0, 0);
    };
    constexpr int left = 34;
    constexpr int right = 360;
    CreateWindow(L"STATIC", L"PLAYER ESP", WS_CHILD | WS_VISIBLE,
        left, 20, 240, 24, parent, 0, 0, 0);
    CreateWindow(L"STATIC", L"WORLD ESP", WS_CHILD | WS_VISIBLE,
        right, 20, 240, 24, parent, 0, 0, 0);

    g_hChkEspEnable = mkchk(L"Enable ESP", 105, left, 52);
    SendMessage(g_hChkEspEnable, BM_SETCHECK, g_EspSettings.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkBox = mkchk(L"Box ESP", 100, left, 84);
    SendMessage(g_hChkBox, BM_SETCHECK, g_EspSettings.box ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkSkel = mkchk(L"Skeleton", 101, left, 112);
    SendMessage(g_hChkSkel, BM_SETCHECK, g_EspSettings.skeleton ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkHead = mkchk(L"Head ESP", 107, left, 140);
    SendMessage(g_hChkHead, BM_SETCHECK, g_EspSettings.headEsp ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkHealth = mkchk(L"Health bar", 102, left, 168);
    SendMessage(g_hChkHealth, BM_SETCHECK, g_EspSettings.health ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkShield = mkchk(L"Shield / Armor", 113, left, 196);
    SendMessage(g_hChkShield, BM_SETCHECK, g_EspSettings.shield ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkName = mkchk(L"Player name", 103, left, 224);
    SendMessage(g_hChkName, BM_SETCHECK, g_EspSettings.name ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkVisible = mkchk(L"Visible only", 104, left, 252);
    SendMessage(g_hChkVisible, BM_SETCHECK, g_EspSettings.visibleOnly ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindow(L"STATIC", L"Box style:", WS_CHILD | WS_VISIBLE,
        left, 298, 100, 22, parent, 0, 0, 0);
    g_hComboStyle = CreateWindow(WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        left + 100, 296, 155, 150, parent, (HMENU)106, 0, 0);
    SendMessage(g_hComboStyle, CB_ADDSTRING, 0, (LPARAM)L"2D Corner");
    SendMessage(g_hComboStyle, CB_ADDSTRING, 0, (LPARAM)L"2D Full");
    SendMessage(g_hComboStyle, CB_ADDSTRING, 0, (LPARAM)L"3D Full");
    SendMessage(g_hComboStyle, CB_ADDSTRING, 0, (LPARAM)L"3D Corner");
    SendMessage(g_hComboStyle, CB_ADDSTRING, 0, (LPARAM)L"2D Rounded");
    SendMessage(g_hComboStyle, CB_ADDSTRING, 0, (LPARAM)L"2D Filled");
    SendMessage(g_hComboStyle, CB_ADDSTRING, 0, (LPARAM)L"2D Circle");
    SendMessage(g_hComboStyle, CB_SETCURSEL, g_EspSettings.boxStyle, 0);
    CreateWindow(L"STATIC", L"Box color:", WS_CHILD | WS_VISIBLE,
        left, 334, 100, 22, parent, 0, 0, 0);
    g_hBtnColorBox = CreateWindow(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        left + 100, 332, 40, 24, parent, (HMENU)200, 0, 0);
    CreateWindow(L"STATIC", L"Visible color:", WS_CHILD | WS_VISIBLE,
        left, 366, 110, 22, parent, 0, 0, 0);
    g_hBtnSkelVisible = CreateWindow(L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        left + 110, 364, 40, 24, parent, (HMENU)203, 0, 0);
    CreateWindow(L"STATIC", L"Hidden color:", WS_CHILD | WS_VISIBLE,
        left, 398, 110, 22, parent, 0, 0, 0);
    g_hBtnSkelHidden = CreateWindow(L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        left + 110, 396, 40, 24, parent, (HMENU)204, 0, 0);

    g_hChkWeapons = mkchk(L"Weapon ESP", 109, right, 52);
    SendMessage(g_hChkWeapons, BM_SETCHECK,
        g_EspSettings.showWeapons ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkGrenades = mkchk(L"Grenade ESP", 110, right, 80);
    SendMessage(g_hChkGrenades, BM_SETCHECK,
        g_EspSettings.showGrenades ? BST_CHECKED : BST_UNCHECKED, 0);
    g_hChkBombEsp = mkchk(L"Bomb ESP", 111, right, 108);
    SendMessage(g_hChkBombEsp, BM_SETCHECK,
        g_EspSettings.showBomb ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindow(L"STATIC", L"AIM VISUALS", WS_CHILD | WS_VISIBLE,
        right, 164, 240, 24, parent, 0, 0, 0);
    g_hChkFovCircle = mkchk(L"Show FOV circle", 108, right, 196);
    SendMessage(g_hChkFovCircle, BM_SETCHECK,
        g_EspSettings.showFovCircle ? BST_CHECKED : BST_UNCHECKED, 0);
    CreateWindow(L"STATIC", L"FOV color:", WS_CHILD | WS_VISIBLE,
        right, 234, 100, 22, parent, 0, 0, 0);
    g_hBtnColorFov = CreateWindow(L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        right + 100, 232, 40, 24, parent, (HMENU)202, 0, 0);
}

static COLORREF PickColor(HWND hParent, COLORREF cur) {
    CHOOSECOLORW cc = {sizeof(cc), hParent};
    static COLORREF cust[16] = {};
    cc.rgbResult = cur;
    cc.lpCustColors = cust;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&cc)) return cc.rgbResult;
    return cur;
}


static const wchar_t* KeyName(int vk) {
    switch (vk) {
    case 0x01: return L"Mouse1";
    case 0x02: return L"Mouse2";
    case 0x04: return L"Mouse3";
    case 0x05: return L"Mouse4";
    case 0x06: return L"Mouse5";
    case 0x09: return L"Tab";
    case 0x10: return L"Shift";
    case 0x11: return L"Ctrl";
    case 0x12: return L"Alt";
    case 0x14: return L"CapsLock";
    case 0x1B: return L"Esc";
    case 0x20: return L"Space";
    case 0x30: return L"0";  case 0x31: return L"1";  case 0x32: return L"2";
    case 0x33: return L"3";  case 0x34: return L"4";  case 0x35: return L"5";
    case 0x36: return L"6";  case 0x37: return L"7";  case 0x38: return L"8";  case 0x39: return L"9";
    case 0x41: return L"A";  case 0x42: return L"B";  case 0x43: return L"C";  case 0x44: return L"D";
    case 0x45: return L"E";  case 0x46: return L"F";  case 0x47: return L"G";  case 0x48: return L"H";
    case 0x49: return L"I";  case 0x4A: return L"J";  case 0x4B: return L"K";  case 0x4C: return L"L";
    case 0x4D: return L"M";  case 0x4E: return L"N";  case 0x4F: return L"O";  case 0x50: return L"P";
    case 0x51: return L"Q";  case 0x52: return L"R";  case 0x53: return L"S";  case 0x54: return L"T";
    case 0x55: return L"U";  case 0x56: return L"V";  case 0x57: return L"W";  case 0x58: return L"X";
    case 0x59: return L"Y";  case 0x5A: return L"Z";
    case VK_F1: return L"F1";  case VK_F2: return L"F2";  case VK_F3: return L"F3";
    case VK_F4: return L"F4";  case VK_F5: return L"F5";  case VK_F6: return L"F6";
    case VK_F7: return L"F7";  case VK_F8: return L"F8";  case VK_F9: return L"F9";
    case VK_F10: return L"F10"; case VK_F11: return L"F11"; case VK_F12: return L"F12";
    default: return L"?";
    }
}

static void UpdateKeyButton() {
    SetWindowText(g_hBtnKey, KeyName(g_AimSettings.aimKey));
}

static bool IsCaptureKeyDown(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

static bool IsAnyCaptureKeyDown() {
    for (int virtualKey = 1; virtualKey < 256; ++virtualKey) {
        if (IsCaptureKeyDown(virtualKey)) return true;
    }
    return false;
}

static int FindCaptureKeyDown() {
    static constexpr int mouseKeys[] = {
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (const int virtualKey : mouseKeys) {
        if (IsCaptureKeyDown(virtualKey)) return virtualKey;
    }
    for (int virtualKey = VK_BACK; virtualKey < 256; ++virtualKey) {
        if (virtualKey == VK_LBUTTON || virtualKey == VK_RBUTTON ||
            virtualKey == VK_MBUTTON || virtualKey == VK_XBUTTON1 ||
            virtualKey == VK_XBUTTON2) continue;
        if (IsCaptureKeyDown(virtualKey)) return virtualKey;
    }
    return 0;
}

static void FinishKeyCapture(int virtualKey) {
    if (virtualKey > 0) g_AimSettings.aimKey = virtualKey;
    g_ListeningForKey = false;
    g_KeyCaptureArmed = false;
    KillTimer(g_hWnd, 0x4B);
    UpdateKeyButton();
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

static void BeginKeyCapture() {
    g_ListeningForKey = true;
    g_KeyCaptureArmed = false;
    SetWindowText(g_hBtnKey, L"Press a key...");
    SetFocus(g_hWnd);
    SetTimer(g_hWnd, 0x4B, 10, nullptr);
    InvalidateRect(g_hWnd, nullptr, FALSE);
}


static void CreateAimControls(HWND parent) {
    int y = 28;
    auto mkchk = [&](const wchar_t* t, int id, int& yy) {
        return CreateWindow(L"BUTTON", t, WS_CHILD | BS_AUTOCHECKBOX | WS_VISIBLE,
            36, yy, 260, 26, parent, (HMENU)id, 0, 0);
    };
    CreateWindow(L"STATIC", L"AIM ASSIST", WS_CHILD | WS_VISIBLE,
        36, y, 280, 24, parent, 0, 0, 0);
    y += 38;

    g_hChkAim = mkchk(L"Enable aim", 300, y); y += 38;
    SendMessage(g_hChkAim, BM_SETCHECK, g_AimSettings.enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    g_hChkAimTeam = mkchk(L"Team check", 305, y); y += 38;
    SendMessage(g_hChkAimTeam, BM_SETCHECK,
        g_AimSettings.teamCheck ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindow(L"STATIC", L"Aim key", WS_CHILD | WS_VISIBLE,
        36, y + 3, 120, 22, parent, 0, 0, 0);
    g_hBtnKey = CreateWindow(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        170, y - 2, 150, 28, parent, (HMENU)301, 0, 0);
    UpdateKeyButton();
    y += 42;

    g_hChkAimVisible = mkchk(L"Visible targets only", 311, y); y += 42;
    SendMessage(g_hChkAimVisible, BM_SETCHECK, g_AimSettings.visibleOnly ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindow(L"STATIC", L"FOV", WS_CHILD | WS_VISIBLE,
        36, y + 3, 120, 22, parent, 0, 0, 0);
    g_hEditFov = CreateWindow(WC_EDITW, L"8",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        170, y - 2, 80, 28, parent, (HMENU)303, 0, 0);
    {
        wchar_t buf[16]; swprintf_s(buf, L"%.0f", g_AimSettings.aimFov);
        SetWindowText(g_hEditFov, buf);
    }
    y += 42;

    CreateWindow(L"STATIC", L"Smooth", WS_CHILD | WS_VISIBLE,
        36, y + 3, 120, 22, parent, 0, 0, 0);
    g_hEditSmooth = CreateWindow(WC_EDITW, L"5",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        170, y - 2, 80, 28, parent, (HMENU)304, 0, 0);
    {
        wchar_t buf[16]; swprintf_s(buf, L"%.0f", g_AimSettings.aimSmooth);
        SetWindowText(g_hEditSmooth, buf);
    }

    CreateWindow(L"STATIC", L"F3  Show / hide menu", WS_CHILD | WS_VISIBLE,
        36, y + 58, 260, 22, parent, 0, 0, 0);
}


static void CreateMiscControls(HWND parent) {
    int y = 20;
    auto mkchk = [&](const wchar_t* t, int id, int& yy) {
        return CreateWindow(L"BUTTON", t, WS_CHILD | BS_AUTOCHECKBOX | WS_VISIBLE,
            30, yy, 200, 24, parent, (HMENU)id, 0, 0);
    };

    g_hChkCrosshair = mkchk(L"Crosshair", 402, y); y += 28;
    SendMessage(g_hChkCrosshair, BM_SETCHECK, g_MiscSettings.showCrosshair ? BST_CHECKED : BST_UNCHECKED, 0);

    g_hChkGameInfo = mkchk(L"Game Info / FPS", 403, y); y += 28;
    SendMessage(g_hChkGameInfo, BM_SETCHECK, g_MiscSettings.showGameInfo ? BST_CHECKED : BST_UNCHECKED, 0);

    g_hChkBombTimer = mkchk(L"Bomb Timer", 404, y); y += 28;
    SendMessage(g_hChkBombTimer, BM_SETCHECK, g_MiscSettings.showBombTimer ? BST_CHECKED : BST_UNCHECKED, 0);

    g_hChkDamageLog = mkchk(L"Damage Log", 405, y); y += 28;
    SendMessage(g_hChkDamageLog, BM_SETCHECK, g_MiscSettings.showDamageLog ? BST_CHECKED : BST_UNCHECKED, 0);
}


static void CreateConfigControls(HWND parent) {
    int y = 20;
    CreateWindow(L"STATIC", L"Configuration profile", WS_CHILD | WS_VISIBLE,
        30, y, 220, 22, parent, 0, 0, 0);
    y += 26;
    g_hConfigPath = CreateWindow(WC_COMBOBOXW, L"lks_config.cfg",
        WS_CHILD | WS_VISIBLE | WS_BORDER | CBS_DROPDOWN |
            CBS_AUTOHSCROLL | WS_VSCROLL,
        30, y, 360, 220, parent, (HMENU)504, 0, 0);
    y += 38;
    CreateWindow(
        L"STATIC",
        L"Saved in Documents\\Lks667\\Configs (type a name or select one).",
        WS_CHILD | WS_VISIBLE,
        30, y, 480, 22, parent, 0, 0, 0);
    y += 36;
    g_hBtnSave = CreateWindow(L"BUTTON", L"Save Config",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        30, y, 120, 28, parent, (HMENU)500, 0, 0);
    y += 40;
    g_hBtnLoad = CreateWindow(L"BUTTON", L"Load Config",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        30, y, 120, 28, parent, (HMENU)501, 0, 0);
    y += 40;
    g_hBtnReset = CreateWindow(L"BUTTON", L"Reset Defaults",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        30, y, 120, 28, parent, (HMENU)502, 0, 0);
    y += 40;
    CreateWindow(L"BUTTON", L"Clear Offset Cache",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        30, y, 140, 28, parent, (HMENU)503, 0, 0);
    y += 30;
    CreateWindow(L"STATIC", L"Force a full offset dump on next connect.",
        WS_CHILD | WS_VISIBLE,
        30, y, 300, 22, parent, 0, 0, 0);
}

static std::wstring SelectedConfigName() {
    wchar_t value[MAX_PATH] = {};
    if (g_hConfigPath) GetWindowTextW(g_hConfigPath, value, MAX_PATH);
    std::wstring name = value;
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    for (wchar_t& ch : name) {
        if (wcschr(L"<>:\"/\\|?*", ch) || ch < 32) ch = L'_';
    }
    if (name.empty()) name = L"lks_config";
    if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".cfg") != 0)
        name += L".cfg";
    return name;
}

static std::filesystem::path ConfigPath() {
    const std::wstring name = SelectedConfigName();
    const std::filesystem::path path =
        ConfigFolder() / name;
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required > 0 && required <= static_cast<int>(sizeof(g_ConfigSettings.lastPath))) {
        WideCharToMultiByte(
            CP_UTF8, 0, name.c_str(), -1,
            g_ConfigSettings.lastPath,
            static_cast<int>(sizeof(g_ConfigSettings.lastPath)),
            nullptr, nullptr);
    }
    return path;
}

static void RefreshConfigList() {
    if (!g_hConfigPath) return;
    const std::wstring selected = SelectedConfigName();
    SendMessage(g_hConfigPath, CB_RESETCONTENT, 0, 0);
    WIN32_FIND_DATAW data = {};
    const std::wstring pattern =
        (ConfigFolder() / L"*.cfg").wstring();
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                SendMessageW(
                    g_hConfigPath, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(data.cFileName));
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    SetWindowTextW(g_hConfigPath, selected.c_str());
}

std::vector<std::wstring> ModernConfigNames() {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = (ConfigFolder() / L"*.cfg").wstring();
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                names.emplace_back(data.cFileName);
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::wstring ModernSelectedConfig() { return SelectedConfigName(); }

void ModernSelectConfig(const std::wstring& name) {
    if (g_hConfigPath) SetWindowTextW(g_hConfigPath, name.c_str());
}

static void SaveConfig() {
    const auto path = ConfigPath();
    std::ofstream f(path);
    if (!f) { SetStatus(L"[-] Failed to save config"); return; }
    f << "[ESP]\n";
    f << "enabled=" << g_EspSettings.enabled << "\n";
    f << "box=" << g_EspSettings.box << "\n";
    f << "skeleton=" << g_EspSettings.skeleton << "\n";
    f << "headEsp=" << g_EspSettings.headEsp << "\n";
    f << "health=" << g_EspSettings.health << "\n";
    f << "shield=" << g_EspSettings.shield << "\n";
    f << "name=" << g_EspSettings.name << "\n";
    f << "visibleOnly=" << g_EspSettings.visibleOnly << "\n";
    f << "showFovCircle=" << g_EspSettings.showFovCircle << "\n";
    f << "showWeapons=" << g_EspSettings.showWeapons << "\n";
    f << "showGrenades=" << g_EspSettings.showGrenades << "\n";
    f << "showBomb=" << g_EspSettings.showBomb << "\n";
    f << "showChickens=" << g_EspSettings.showChickens << "\n";
    f << "showTeamCT=" << g_EspSettings.showTeamCT << "\n";
    f << "showTeamT=" << g_EspSettings.showTeamT << "\n";
    f << "espTeamCheck=" << g_EspSettings.teamCheck << "\n";
    f << "boxStyle=" << g_EspSettings.boxStyle << "\n";
    f << "boxColor=" << static_cast<unsigned long>(g_EspSettings.boxColor) << "\n";
    f << "skeletonColor=" << static_cast<unsigned long>(g_EspSettings.skeletonColor) << "\n";
    f << "skeletonVisibleColor=" << static_cast<unsigned long>(g_EspSettings.skeletonVisibleColor) << "\n";
    f << "skeletonHiddenColor=" << static_cast<unsigned long>(g_EspSettings.skeletonHiddenColor) << "\n";
    f << "fovCircleColor=" << static_cast<unsigned long>(g_EspSettings.fovCircleColor) << "\n";
    f << "[Aim]\n";
    f << "enabled=" << g_AimSettings.enabled << "\n";
    f << "aimKey=" << g_AimSettings.aimKey << "\n";
    f << "aimBone=" << g_AimSettings.aimBone << "\n";
    f << "aimFov=" << g_AimSettings.aimFov << "\n";
    f << "aimSmooth=" << g_AimSettings.aimSmooth << "\n";
    f << "teamCheck=" << g_AimSettings.teamCheck << "\n";
    f << "visibleOnly=" << g_AimSettings.visibleOnly << "\n";
    f << "humanize=" << g_AimSettings.humanize << "\n";
    f << "reactionTime=" << g_AimSettings.reactionTime << "\n";
    f << "jitter=" << g_AimSettings.jitter << "\n";
    f << "ease=" << g_AimSettings.ease << "\n";
    f << "targetCooldown=" << g_AimSettings.targetCooldown << "\n";
    f << "[Misc]\n";
    f << "showCrosshair=" << g_MiscSettings.showCrosshair << "\n";
    f << "showGameInfo=" << g_MiscSettings.showGameInfo << "\n";
    f << "showBombTimer=" << g_MiscSettings.showBombTimer << "\n";
    f << "showDamageLog=" << g_MiscSettings.showDamageLog << "\n";
    SetStatus(L"[+] Config saved");
    RefreshConfigList();
}

static void LoadConfig() {
    const auto path = ConfigPath();
    std::ifstream f(path);
    if (!f) { SetStatus(L"[-] Failed to load config"); return; }
    std::string line;
    std::string section;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (section == "ESP") {
            if (key == "enabled") g_EspSettings.enabled = (val == "1");
            else if (key == "box") g_EspSettings.box = (val == "1");
            else if (key == "skeleton") g_EspSettings.skeleton = (val == "1");
            else if (key == "headEsp") g_EspSettings.headEsp = (val == "1");
            else if (key == "health") g_EspSettings.health = (val == "1");
            else if (key == "shield") g_EspSettings.shield = (val == "1");
            else if (key == "name") g_EspSettings.name = (val == "1");
            else if (key == "visibleOnly") g_EspSettings.visibleOnly = (val == "1");
            else if (key == "showFovCircle") g_EspSettings.showFovCircle = (val == "1");
            else if (key == "showWeapons") g_EspSettings.showWeapons = (val == "1");
            else if (key == "showGrenades") g_EspSettings.showGrenades = (val == "1");
            else if (key == "showBomb") g_EspSettings.showBomb = (val == "1");
            else if (key == "showChickens") g_EspSettings.showChickens = (val == "1");
            else if (key == "showTeamCT") g_EspSettings.showTeamCT = (val == "1");
            else if (key == "showTeamT") g_EspSettings.showTeamT = (val == "1");
            else if (key == "espTeamCheck") g_EspSettings.teamCheck = (val == "1");
            else if (key == "boxStyle")
                g_EspSettings.boxStyle =
                    std::clamp(std::stoi(val), 0, BOX_STYLE_COUNT - 1);
            else if (key == "boxColor")
                g_EspSettings.boxColor =
                    static_cast<COLORREF>(std::stoul(val));
            else if (key == "skeletonColor")
                g_EspSettings.skeletonColor =
                    static_cast<COLORREF>(std::stoul(val));
            else if (key == "skeletonVisibleColor")
                g_EspSettings.skeletonVisibleColor =
                    static_cast<COLORREF>(std::stoul(val));
            else if (key == "skeletonHiddenColor")
                g_EspSettings.skeletonHiddenColor =
                    static_cast<COLORREF>(std::stoul(val));
            else if (key == "fovCircleColor")
                g_EspSettings.fovCircleColor =
                    static_cast<COLORREF>(std::stoul(val));
        } else if (section == "Aim") {
            if (key == "enabled") g_AimSettings.enabled = (val == "1");
            else if (key == "aimKey") g_AimSettings.aimKey = std::stoi(val);
            else if (key == "aimBone") g_AimSettings.aimBone = std::stoi(val);
            else if (key == "aimFov") g_AimSettings.aimFov = std::stof(val);
            else if (key == "aimSmooth") g_AimSettings.aimSmooth = std::stof(val);
            else if (key == "teamCheck") g_AimSettings.teamCheck = (val == "1");
            else if (key == "visibleOnly") g_AimSettings.visibleOnly = (val == "1");
            else if (key == "humanize") g_AimSettings.humanize = (val == "1");
            else if (key == "reactionTime") g_AimSettings.reactionTime = std::stof(val);
            else if (key == "jitter") g_AimSettings.jitter = std::stof(val);
            else if (key == "ease") g_AimSettings.ease = std::stof(val);
            else if (key == "targetCooldown") g_AimSettings.targetCooldown = std::stoi(val);
        } else if (section == "Misc") {
            if (key == "showCrosshair") g_MiscSettings.showCrosshair = (val == "1");
            else if (key == "showGameInfo") g_MiscSettings.showGameInfo = (val == "1");
            else if (key == "showBombTimer") g_MiscSettings.showBombTimer = (val == "1");
            else if (key == "showDamageLog") g_MiscSettings.showDamageLog = (val == "1");
        }
    }
    SetStatus(L"[+] Config loaded");
    HudRefresh();
}


static void SyncControlsFromSettings() {
    const auto setCheck = [](HWND control, bool checked) {
        if (control)
            SendMessage(
                control,
                BM_SETCHECK,
                checked ? BST_CHECKED : BST_UNCHECKED,
                0);
    };

    setCheck(g_hChkEspEnable, g_EspSettings.enabled);
    setCheck(g_hChkBox, g_EspSettings.box);
    setCheck(g_hChkSkel, g_EspSettings.skeleton);
    setCheck(g_hChkHead, g_EspSettings.headEsp);
    setCheck(g_hChkHealth, g_EspSettings.health);
    setCheck(g_hChkShield, g_EspSettings.shield);
    setCheck(g_hChkName, g_EspSettings.name);
    setCheck(g_hChkVisible, g_EspSettings.visibleOnly);
    setCheck(g_hChkFovCircle, g_EspSettings.showFovCircle);
    setCheck(g_hChkWeapons, g_EspSettings.showWeapons);
    setCheck(g_hChkGrenades, g_EspSettings.showGrenades);
    setCheck(g_hChkBombEsp, g_EspSettings.showBomb);
    g_EspSettings.boxStyle = std::clamp(
        g_EspSettings.boxStyle, 0, BOX_STYLE_COUNT - 1);
    if (g_hComboStyle)
        SendMessage(g_hComboStyle, CB_SETCURSEL, g_EspSettings.boxStyle, 0);

    setCheck(g_hChkAim, g_AimSettings.enabled);
    setCheck(g_hChkAimTeam, g_AimSettings.teamCheck);
    setCheck(g_hChkAimVisible, g_AimSettings.visibleOnly);
    setCheck(g_hChkHumanize, g_AimSettings.humanize);
    UpdateKeyButton();
    int boneSelection = 0;
    if (g_AimSettings.aimBone == BONE_NECK) boneSelection = 1;
    else if (g_AimSettings.aimBone == BONE_CHEST) boneSelection = 2;
    else if (g_AimSettings.aimBone == BONE_PELVIS) boneSelection = 3;
    if (g_hComboBone)
        SendMessage(g_hComboBone, CB_SETCURSEL, boneSelection, 0);

    setCheck(g_hChkCrosshair, g_MiscSettings.showCrosshair);
    setCheck(g_hChkGameInfo, g_MiscSettings.showGameInfo);
    setCheck(g_hChkBombTimer, g_MiscSettings.showBombTimer);
    setCheck(g_hChkDamageLog, g_MiscSettings.showDamageLog);

    wchar_t value[32] = {};
    swprintf_s(value, L"%.0f", g_AimSettings.aimFov);
    SetWindowText(g_hEditFov, value);
    swprintf_s(value, L"%.0f", g_AimSettings.aimSmooth);
    SetWindowText(g_hEditSmooth, value);
    swprintf_s(value, L"%.0f", g_AimSettings.reactionTime);
    SetWindowText(g_hEditReaction, value);
    swprintf_s(value, L"%.1f", g_AimSettings.jitter);
    SetWindowText(g_hEditJitter, value);
    swprintf_s(value, L"%.2f", g_AimSettings.ease);
    SetWindowText(g_hEditEase, value);
    swprintf_s(value, L"%d", g_AimSettings.targetCooldown);
    SetWindowText(g_hEditCooldown, value);

    g_TempBoxCol = g_EspSettings.boxColor;
    g_TempSkelCol = g_EspSettings.skeletonColor;
    g_TempSkelVisibleCol = g_EspSettings.skeletonVisibleColor;
    g_TempSkelHiddenCol = g_EspSettings.skeletonHiddenColor;
    g_TempFovCol = g_EspSettings.fovCircleColor;
    if (g_hBtnColorBox) InvalidateRect(g_hBtnColorBox, nullptr, TRUE);
    if (g_hBtnColorSkel) InvalidateRect(g_hBtnColorSkel, nullptr, TRUE);

    wchar_t configPath[260] = {};
    MultiByteToWideChar(
        CP_UTF8,
        0,
        g_ConfigSettings.lastPath,
        -1,
        configPath,
        static_cast<int>(_countof(configPath)));
    if (g_hConfigPath) {
        SetWindowText(g_hConfigPath, configPath);
        RefreshConfigList();
    }
}

static std::wstring AppFolder() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    const size_t slash = path.find_last_of(L'\\');
    return (slash == std::wstring::npos) ?
        L"" : path.substr(0, slash + 1);
}

static std::filesystem::path ConfigFolder() {
    PWSTR documents = nullptr;
    std::filesystem::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)) &&
        documents) {
        folder = std::filesystem::path(documents) / L"Lks667" / L"Configs";
        CoTaskMemFree(documents);
    } else {
        if (documents) CoTaskMemFree(documents);
        folder = std::filesystem::path(AppFolder()) / L"Configs";
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        folder = std::filesystem::path(AppFolder()) / L"Configs";
        error.clear();
        std::filesystem::create_directories(folder, error);
    }
    return folder;
}

static std::wstring OffsetCachePath() {
    return AppFolder() + L"lks_offsets.dat";
}

static uint32_t GetClientFingerprint(HANDLE hProc) {
    if (!hProc) return 0;
    const HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE, GetProcessId(hProc));
    if (snap == INVALID_HANDLE_VALUE) return 0;
    uintptr_t base = 0;
    MODULEENTRY32W me = {sizeof(me)};
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"client.dll") == 0) {
                base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    if (!base) return 0;
    DWORD peOff = 0;
    SIZE_T read = 0;
    if (!ReadProcessMemory(
            hProc,
            reinterpret_cast<LPCVOID>(base + 0x3C),
            &peOff,
            sizeof(peOff),
            &read) ||
        read != sizeof(peOff))
        return 0;
    DWORD timestamp = 0;
    if (!ReadProcessMemory(
            hProc,
            reinterpret_cast<LPCVOID>(base + peOff + 8),
            &timestamp,
            sizeof(timestamp),
            &read) ||
        read != sizeof(timestamp))
        return 0;
    return timestamp;
}

static bool SaveOffsetCache(uint32_t fingerprint) {
    std::ofstream f(OffsetCachePath(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const uint32_t magic = 0x4F534B4C;
    const uint32_t version = 1;
    const uint32_t count = static_cast<uint32_t>(g_Offsets.size());
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&version), 4);
    f.write(reinterpret_cast<const char*>(&fingerprint), 4);
    f.write(reinterpret_cast<const char*>(&count), 4);
    for (const auto& o : g_Offsets) {
        const uint16_t ml = static_cast<uint16_t>(o.mod.size());
        const uint16_t cl = static_cast<uint16_t>(o.className.size());
        const uint16_t nl = static_cast<uint16_t>(o.name.size());
        f.write(reinterpret_cast<const char*>(&ml), 2);
        f.write(o.mod.data(), ml);
        f.write(reinterpret_cast<const char*>(&cl), 2);
        f.write(o.className.data(), cl);
        f.write(reinterpret_cast<const char*>(&nl), 2);
        f.write(o.name.data(), nl);
        f.write(reinterpret_cast<const char*>(&o.value), 4);
    }
    EspLog("[Cache] saved %zu offsets to %ls",
        g_Offsets.size(), OffsetCachePath().c_str());
    return true;
}

static bool LoadOffsetCache(uint32_t fingerprint) {
    std::ifstream f(OffsetCachePath(), std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0, version = 0, cachedFp = 0, count = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&cachedFp), 4);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (magic != 0x4F534B4C || version != 1 ||
        cachedFp != fingerprint || count == 0 || count > 100000)
        return false;
    std::vector<OffsetEntry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t ml = 0, cl = 0, nl = 0;
        f.read(reinterpret_cast<char*>(&ml), 2);
        if (ml > 255) return false;
        std::string mod(ml, '\0');
        f.read(mod.data(), ml);
        f.read(reinterpret_cast<char*>(&cl), 2);
        if (cl > 255) return false;
        std::string className(cl, '\0');
        f.read(className.data(), cl);
        f.read(reinterpret_cast<char*>(&nl), 2);
        if (nl > 255) return false;
        std::string name(nl, '\0');
        f.read(name.data(), nl);
        uint32_t value = 0;
        f.read(reinterpret_cast<char*>(&value), 4);
        entries.push_back({
            std::move(mod),
            std::move(className),
            std::move(name),
            value});
    }
    if (!f.good()) return false;
    g_Offsets = std::move(entries);
    EspLog("[Cache] loaded %zu offsets (fp=0x%08X)",
        g_Offsets.size(), fingerprint);
    return true;
}

static bool WaitWhileRunning(DWORD milliseconds) {
    const DWORD started = GetTickCount();
    while (g_Running.load(std::memory_order_acquire)) {
        const DWORD elapsed = GetTickCount() - started;
        if (elapsed >= milliseconds) return true;
        Sleep(std::min<DWORD>(50, milliseconds - elapsed));
    }
    return false;
}

static void RegisterControllerWithDriver() {
    const HWND hwnd = g_hWnd;
    if (!hwnd || !IsWindow(hwnd) || !g_Client.GetProcessHandle()) return;
    DWORD controllerPid = 0;
    const DWORD controllerTid = GetWindowThreadProcessId(hwnd, &controllerPid);
    if (controllerTid && controllerPid &&
        g_Client.SendTargetThread(controllerPid, controllerTid)) {
        EspLog("[Bootstrap] controller registered: PID=%u TID=%u",
               controllerPid, controllerTid);
    } else {
        EspLog("[Bootstrap] controller registration failed: PID=%u TID=%u error=%u",
               controllerPid, controllerTid, GetLastError());
    }
}

static void DumpThread() {
    while (g_Running.load(std::memory_order_acquire)) {
        
        
        EspLog("[Bootstrap] waiting for cs2.exe");
        SetStatus(L"[*] Waiting for cs2.exe...");
        while (g_Running.load(std::memory_order_acquire) &&
            g_Client.GetProcessIdByName(L"cs2.exe") == 0)
            WaitWhileRunning(2000);
        if (!g_Running.load(std::memory_order_acquire)) break;

        
        if (!g_Client.Initialize(XorWideString(L"cs2.exe"))) {
            
            
            
            if (ProbeDriverAlive(L"cs2.exe")) {
                EspLog("[Bootstrap] live driver present but unreachable; aborting map");
                SetStatus(L"[-] Old driver still loaded. Close CS2 once (or reboot) and relaunch.");
                WaitWhileRunning(5000);
                continue;
            }
            EspLog("[Bootstrap] kernel transport unavailable, mapping driver");
            SetStatus(L"[*] Mapping kernel driver...");
            std::wstring error;
            if (!MapKernelDriver(AppFolder() + L"Lks_KernelDriver.sys", error)) {
                EspLog("[Bootstrap] driver mapping failed: %ls", error.c_str());
                SetStatus(L"[-] Driver mapping failed; retrying in 5s...");
                WaitWhileRunning(5000);
                continue;
            }
            EspLog("[Bootstrap] driver mapped, waiting for transport...");
            SetStatus(L"[*] Waiting for kernel transport...");
            WaitWhileRunning(1500);
            if (!g_Client.Initialize(XorWideString(L"cs2.exe"))) {
                EspLog("[Bootstrap] transport init failed after mapping");
                SetStatus(L"[-] Kernel transport failed; retrying in 5s...");
                WaitWhileRunning(5000);
                continue;
            }
        }
        EspLog("[Bootstrap] connected. hProcess=0x%llX inContext=%d",
            (uintptr_t)g_Client.GetProcessHandle(), g_Client.IsInContext());
        RegisterControllerWithDriver();

        
        const HANDLE hProc = g_Client.GetProcessHandle();
        g_UseKernelRead.store(true, std::memory_order_release);
        uint32_t fingerprint = 0;
        for (int attempt = 0;
            g_Running.load(std::memory_order_acquire) &&
            !fingerprint && attempt < 10;
            ++attempt) {
            fingerprint = GetClientFingerprint(hProc);
            if (!fingerprint) WaitWhileRunning(2000);
        }
        bool offsetsReady = false;
        if (fingerprint && LoadOffsetCache(fingerprint)) {
            offsetsReady = true;
        } else {
            SetStatus(L"[*] Dumping offsets...");
            while (g_Running.load(std::memory_order_acquire) &&
                !offsetsReady) {
                try {
                    cs2dumper::memory::ProcessMemory process;
                    process.attach("cs2.exe");
                    EspLog("[Bootstrap] process attached, starting analysis...");
                    auto result =
                        cs2dumper::analysis::analyze_all(process);
                    EspLog("[Bootstrap] analysis complete, sending offsets...");

                    g_Offsets.clear();
                    size_t nStatic = 0, nSchema = 0;
                    for (const auto& [mod, offsets] : result.offsets) {
                        EspLog("[Bootstrap] static module '%s': %zu offsets",
                            mod.c_str(), offsets.size());
                        for (const auto& [name, value] : offsets) {
                            EspLog("[Bootstrap]   static: %s = 0x%X",
                                name.c_str(), value);
                            g_Offsets.push_back({mod, {}, name, value});
                            nStatic++;
                        }
                    }
                    for (const auto& [mod, schemas] : result.schemas) {
                        nSchema += schemas.first.size();
                        EspLog("[Bootstrap] schema module '%s': %zu classes",
                            mod.c_str(), schemas.first.size());
                        for (const auto& cls : schemas.first) {
                            EspLog("[Bootstrap]   class '%s' fields=%zu parent=%s",
                                cls.name.c_str(), cls.fields.size(),
                                cls.parent_name.value_or("(none)").c_str());
                            for (const auto& field : cls.fields)
                                if (field.offset > 0) {
                                    g_Offsets.push_back({
                                        mod,
                                        cls.name,
                                        field.name,
                                        (uint32_t)field.offset});
                                }
                        }
                    }
                    EspLog("[Bootstrap] DONE: static=%zu schemas(chunks)=%zu total_entries=%zu",
                        nStatic, nSchema, g_Offsets.size());
                    if (fingerprint)
                        SaveOffsetCache(fingerprint);
                    offsetsReady = true;
                } catch (const std::exception& e) {
                    EspLog("[Bootstrap] dump failed: %s", e.what());
                    SetStatus(L"[-] Offset dump failed; retrying in 5s...");
                    WaitWhileRunning(5000);
                }
            }
            if (!g_Running.load(std::memory_order_acquire)) break;
        }
        if (!offsetsReady) break;
        {
            wchar_t buf[128];
            swprintf_s(buf, L"[+] %zu total offsets.", g_Offsets.size());
            SetStatus(buf);
        }

        
        const HWND hwnd = g_hWnd;
        if (hwnd && IsWindow(hwnd) && g_Client.GetProcessHandle()) {
            DWORD controllerPid = 0;
            const DWORD controllerTid =
                GetWindowThreadProcessId(hwnd, &controllerPid);
            if (controllerTid != 0 && controllerPid != 0 &&
                g_Client.SendTargetThread(controllerPid, controllerTid)) {
                EspLog(
                    "[Bootstrap] sent controller GUI PID=%u TID=%u to kernel",
                    controllerPid,
                    controllerTid);
            } else {
                EspLog(
                    "[Bootstrap] controller GUI target failed: PID=%u TID=%u error=%u",
                    controllerPid,
                    controllerTid,
                    GetLastError());
            }
        }

        if (!g_Client.IsInContext()) {
            EspLog("[Bootstrap] refusing overlay start without local shared transport");
            SetStatus(L"[-] Local kernel transport mapping failed; retrying in 5s...");
            WaitWhileRunning(5000);
            continue;
        }

        bool launchOverlay = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
            launchOverlay =
                g_EspSettings.enabled || g_AimSettings.enabled;
        }
        if (launchOverlay) {
            EspLog("[Bootstrap] starting overlay...");
            StartOverlay(g_Client.GetProcessHandle());
            SetStatus(L"[+] ESP overlay thread launched");
        } else {
            EspLog("[Bootstrap] ESP/aim disabled in settings, overlay not started");
        }

        
        const HANDLE watchProc = g_Client.GetProcessHandle();
        while (g_Running.load(std::memory_order_acquire) &&
            watchProc && WaitForSingleObject(watchProc, 2000) == WAIT_TIMEOUT) {
            bool wantOverlay = false;
            {
                std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
                wantOverlay =
                    g_EspSettings.enabled || g_AimSettings.enabled;
            }
            
            
            
            if (wantOverlay) StartOverlay(g_Client.GetProcessHandle());
        }
        EspLog("[Bootstrap] cs2.exe exited; tearing down overlay");
        SetStatus(L"[*] cs2.exe closed; waiting for restart...");
        StopOverlay();
        if (!g_Running.load(std::memory_order_acquire) &&
            g_Client.RequestDriverStop()) {
            EspLog("[Bootstrap] app closing while game alive; driver unload requested");
            Sleep(750);
        }
        g_Client.Shutdown();
        g_UseKernelRead.store(false, std::memory_order_release);
    }
    
    
    if (!g_Running.load(std::memory_order_acquire) &&
        g_Client.RequestDriverStop()) {
        EspLog("[Bootstrap] final driver unload requested");
        Sleep(750);
    }
    EspLog("[Bootstrap] thread exit");
}


static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (w) {
            g_Running.store(false, std::memory_order_release);
            DestroyWindow(h);
        }
        return 0;

    case WM_CLOSE:
        if (!g_Closing) {
            g_Closing = true;
            HudDestroy();
            KillTimer(h, FADE_TIMER_ID);
            KillTimer(h, STARTUP_SOUND_TIMER_ID);
            g_ExitInitialAlpha = g_CurrentAlpha;
            g_ExitStarted = GetTickCount();
            PlayStartupSound(true);
            SetTimer(h, EXIT_FADE_TIMER_ID, 16, nullptr);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(h, EXIT_FADE_TIMER_ID);
        UnregisterHotKey(h, MENU_HOTKEY_ID);
        g_Running.store(false, std::memory_order_release);
        PostQuitMessage(0);
        return 0;

    case WM_CREATE: {
        g_BgBrush = CreateSolidBrush(Ui::Canvas);
        g_InputBrush = CreateSolidBrush(Ui::Surface);
        g_FontCons = CreateFont(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        g_FontSegoe = CreateFont(26, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        g_FontBody = CreateFontW(-16, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI Variable Text");
        g_FontSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI Variable Text");
        g_FontSection = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI Variable Display");

        RECT rc; GetClientRect(h, &rc);
        RegisterHotKey(h, MENU_HOTKEY_ID, MOD_NOREPEAT, VK_F3);

        WNDCLASS wc = {};
        wc.lpfnWndProc = BtnProc; wc.hInstance = GetModuleHandle(0);
        wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
        wc.lpszClassName = L"BtnC"; RegisterClass(&wc);
        wc.lpfnWndProc = MinProc; wc.lpszClassName = L"BtnM"; RegisterClass(&wc);

        g_hClose = CreateWindow(L"BtnC", 0, WS_CHILD | WS_VISIBLE, rc.right - 34, 5, 28, 24, h, (HMENU)0, 0, 0);
        g_hMin = CreateWindow(L"BtnM", 0, WS_CHILD | WS_VISIBLE, rc.right - 66, 5, 28, 24, h, 0, 0, 0);
        ShowWindow(g_hClose, SW_HIDE);
        ShowWindow(g_hMin, SW_HIDE);

        INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icex);

        g_hTab = CreateWindow(WC_TABCONTROLW, 0, WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH | TCS_OWNERDRAWFIXED,
            0, 40, rc.right, rc.bottom - 80, h, 0, 0, 0);
        SetWindowSubclass(g_hTab, TabSubclass, 0, 0);

        TCITEMW ti = {TCIF_TEXT};
        ti.pszText = (LPWSTR)L"Aim"; TabCtrl_InsertItem(g_hTab, 0, &ti);
        ti.pszText = (LPWSTR)L"ESP"; TabCtrl_InsertItem(g_hTab, 1, &ti);
        ti.pszText = (LPWSTR)L"Misc"; TabCtrl_InsertItem(g_hTab, 2, &ti);
        ti.pszText = (LPWSTR)L"Config"; TabCtrl_InsertItem(g_hTab, 3, &ti);

        RECT tr; GetClientRect(g_hTab, &tr);
        TabCtrl_AdjustRect(g_hTab, FALSE, &tr);
        int tx = tr.left, ty = tr.top, tw = tr.right - tr.left, th = tr.bottom - tr.top;

        g_hTabAim = CreateWindow(L"STATIC", L"", WS_CHILD, tx, ty, tw, th, g_hTab, 0, 0, 0);
        SetWindowSubclass(g_hTabAim, EspPageSubclass, 0, 0);
        SetWindowTheme(g_hTabAim, L"", L"");
        CreateAimControls(g_hTabAim);

        g_hTabEsp = CreateWindow(L"STATIC", L"", WS_CHILD, tx, ty, tw, th, g_hTab, 0, 0, 0);
        SetWindowSubclass(g_hTabEsp, EspPageSubclass, 0, 0);
        SetWindowTheme(g_hTabEsp, L"", L"");
        CreateEspControls(g_hTabEsp);

        g_hTabMisc = CreateWindow(L"STATIC", L"", WS_CHILD, tx, ty, tw, th, g_hTab, 0, 0, 0);
        SetWindowSubclass(g_hTabMisc, EspPageSubclass, 0, 0);
        SetWindowTheme(g_hTabMisc, L"", L"");
        CreateMiscControls(g_hTabMisc);

        g_hTabConfig = CreateWindow(L"STATIC", L"", WS_CHILD, tx, ty, tw, th, g_hTab, 0, 0, 0);
        SetWindowSubclass(g_hTabConfig, EspPageSubclass, 0, 0);
        SetWindowTheme(g_hTabConfig, L"", L"");
        CreateConfigControls(g_hTabConfig);
        SyncControlsFromSettings();
        Ui::StyleChildren(g_hTabAim);
        Ui::StyleChildren(g_hTabEsp);
        Ui::StyleChildren(g_hTabMisc);
        Ui::StyleChildren(g_hTabConfig);

        ShowWindow(g_hTabAim, SW_SHOW);
        ShowWindow(g_hTabEsp, SW_HIDE);
        ShowWindow(g_hTabMisc, SW_HIDE);
        ShowWindow(g_hTabConfig, SW_HIDE);

        g_hStatus = CreateWindow(L"STATIC", L"[*] Loading...",
            WS_CHILD | SS_CENTERIMAGE,
            15, rc.bottom - 30, rc.right - 30, 22, h, 0, 0, 0);
        SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_FontCons, TRUE);
        ShowWindow(g_hTab, SW_HIDE);
        if (!lks::modern_ui::initialize(h)) return -1;

        g_DumpWorker = std::thread(DumpThread);
    } return 0;

    case WM_HOTKEY:
        if (w == MENU_HOTKEY_ID) {
            if (IsWindowVisible(h)) {
                ShowWindow(h, SW_HIDE);
            } else {
                ShowWindow(h, SW_SHOW);
                ShowWindow(h, SW_RESTORE);
                SetForegroundWindow(h);
            }
            return 0;
        }
        break;

    case WM_SIZE:
        lks::modern_ui::resize(LOWORD(l), HIWORD(l));
        return 0;

    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
            lks::modern_ui::mouse_move(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        } else {
            lks::modern_ui::mouse_move(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        }
        return 0;
    case WM_MOUSELEAVE:
        lks::modern_ui::mouse_leave();
        return 0;

    case WM_COMMAND: {
        std::lock_guard<std::recursive_mutex> settingsLock(g_SettingsMutex);
        int id = LOWORD(w);
        if (id == 200) {
            COLORREF c = PickColor(g_hTabEsp, g_TempBoxCol);
            g_TempBoxCol = c; g_EspSettings.boxColor = c;
            InvalidateRect(g_hBtnColorBox, 0, TRUE);
        }
        if (id == 201) {
            COLORREF c = PickColor(g_hTabEsp, g_TempSkelCol);
            g_TempSkelCol = c; g_EspSettings.skeletonColor = c;
            InvalidateRect(g_hBtnColorSkel, 0, TRUE);
        }
        if (id == 202) {
            COLORREF c = PickColor(g_hTabEsp, g_TempFovCol);
            g_TempFovCol = c; g_EspSettings.fovCircleColor = c;
            InvalidateRect(g_hBtnColorFov, 0, TRUE);
        }
        if (id == 203) {
            COLORREF c = PickColor(g_hTabEsp, g_TempSkelVisibleCol);
            g_TempSkelVisibleCol = c;
            g_EspSettings.skeletonVisibleColor = c;
            InvalidateRect(g_hBtnSkelVisible, 0, TRUE);
        }
        if (id == 204) {
            COLORREF c = PickColor(g_hTabEsp, g_TempSkelHiddenCol);
            g_TempSkelHiddenCol = c;
            g_EspSettings.skeletonHiddenColor = c;
            InvalidateRect(g_hBtnSkelHidden, 0, TRUE);
        }
        if (HIWORD(w) == BN_CLICKED) {
            if (id == 301 && !g_ListeningForKey) {
                BeginKeyCapture();
                return 0;
            }
            if ((id >= 100 && id <= 105) ||
                (id >= 107 && id <= 111)) {
                bool chk = SendMessage(GetDlgItem(g_hTabEsp, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
                switch (id) {
                case 100: g_EspSettings.box = chk; break;
                case 101: g_EspSettings.skeleton = chk; break;
                case 102: g_EspSettings.health = chk; break;
                case 113: g_EspSettings.shield = chk; break;
                case 103: g_EspSettings.name = chk; break;
                case 104: g_EspSettings.visibleOnly = chk; break;
                case 105: g_EspSettings.enabled = chk; break;
                case 107: g_EspSettings.headEsp = chk; break;
                case 108: g_EspSettings.showFovCircle = chk; break;
                case 109: g_EspSettings.showWeapons = chk; break;
                case 110: g_EspSettings.showGrenades = chk; break;
                case 111: g_EspSettings.showBomb = chk; break;
                }
            }
            if (id >= 300 && id <= 311) {
                if (id == 301) return 0;
                bool chk = SendMessage(GetDlgItem(g_hTabAim, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
                switch (id) {
                case 300: g_AimSettings.enabled = chk; break;
                case 305: g_AimSettings.teamCheck = chk; break;
                case 306: g_AimSettings.humanize = chk; break;
                case 311: g_AimSettings.visibleOnly = chk; break;
                }
            }
            if (id >= 402 && id <= 405) {
                bool chk = SendMessage(GetDlgItem(g_hTabMisc, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
                switch (id) {
                case 402: g_MiscSettings.showCrosshair = chk; break;
                case 403: g_MiscSettings.showGameInfo = chk; break;
                case 404: g_MiscSettings.showBombTimer = chk; break;
                case 405: g_MiscSettings.showDamageLog = chk; break;
                }
                HudRefresh();
            }
            if (id == 500) SaveConfig();
            if (id == 501) {
                LoadConfig();
                SyncControlsFromSettings();
            }
            if (id == 502) {
                g_EspSettings = EspSettings{};
                g_AimSettings = AimSettings{};
                g_MiscSettings = MiscSettings{};
                SyncControlsFromSettings();
                HudRefresh();
                SetStatus(L"[+] Defaults reset");
            }
            if (id == 503) {
                if (DeleteFileW(OffsetCachePath().c_str())) {
                    EspLog("[Config] offset cache cleared: %ls",
                        OffsetCachePath().c_str());
                    SetStatus(L"[+] Offset cache cleared; re-dump on next connect");
                } else {
                    SetStatus(L"[-] No offset cache to clear");
                }
            }
        }
        if (HIWORD(w) == CBN_SELCHANGE) {
            if (id == 106) {
                const LRESULT selection =
                    SendMessage(g_hComboStyle, CB_GETCURSEL, 0, 0);
                if (selection >= 0 && selection < BOX_STYLE_COUNT)
                    g_EspSettings.boxStyle = static_cast<int>(selection);
            }
            if (id == 302) {
                int bones[] = {7, 6, 23, 1};
                g_AimSettings.aimBone = bones[(int)SendMessage(g_hComboBone, CB_GETCURSEL, 0, 0)];
            }
        }
        if (HIWORD(w) == EN_CHANGE) {
            if (id == 303 || id == 304 || id == 307 || id == 308 || id == 309 || id == 310) {
                wchar_t buf[16];
                GetWindowText((id == 303) ? g_hEditFov : (id == 304) ? g_hEditSmooth : (id == 307) ? g_hEditReaction : (id == 308) ? g_hEditJitter : (id == 309) ? g_hEditEase : g_hEditCooldown, buf, 16);
                float v = (float)_wtof(buf);
                switch (id) {
                case 303: g_AimSettings.aimFov = v; if (g_AimSettings.aimFov < 1.f) g_AimSettings.aimFov = 1.f; break;
                case 304: g_AimSettings.aimSmooth = v; if (g_AimSettings.aimSmooth < 1.f) g_AimSettings.aimSmooth = 1.f; break;
                case 307: g_AimSettings.reactionTime = v; if (g_AimSettings.reactionTime < 0.f) g_AimSettings.reactionTime = 0.f; break;
                case 308: g_AimSettings.jitter = v; if (g_AimSettings.jitter < 0.f) g_AimSettings.jitter = 0.f; break;
                case 309: g_AimSettings.ease = v; if (g_AimSettings.ease < 0.01f) g_AimSettings.ease = 0.01f; if (g_AimSettings.ease > 0.95f) g_AimSettings.ease = 0.95f; break;
                case 310: g_AimSettings.targetCooldown = (int)v; if (g_AimSettings.targetCooldown < 0) g_AimSettings.targetCooldown = 0; break;
                }
            }
        }
    } return 0;

    case WM_TIMER:
        if (w == EXIT_FADE_TIMER_ID) {
            const float elapsed = static_cast<float>(GetTickCount() - g_ExitStarted);
            const float t = std::clamp(elapsed / 520.0f, 0.0f, 1.0f);
            const float eased = t * t * (3.0f - 2.0f * t);
            g_CurrentAlpha = static_cast<BYTE>(std::round(
                static_cast<float>(g_ExitInitialAlpha) * (1.0f - eased)));
            SetLayeredWindowAttributes(h, 0, g_CurrentAlpha, LWA_ALPHA);
            if (elapsed >= 760.0f) {
                KillTimer(h, EXIT_FADE_TIMER_ID);
                DestroyWindow(h);
            }
            return 0;
        }
        if (w == STARTUP_SOUND_TIMER_ID) {
            KillTimer(h, STARTUP_SOUND_TIMER_ID);
            PlayStartupSound();
            return 0;
        }
        if (w == FADE_TIMER_ID) {
            const float elapsed = static_cast<float>(GetTickCount() - g_FadeStarted);
            const float t = std::clamp(elapsed / 460.0f, 0.0f, 1.0f);
            const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            g_CurrentAlpha = static_cast<BYTE>(std::round(eased * 255.0f));
            SetLayeredWindowAttributes(h, 0, g_CurrentAlpha, LWA_ALPHA);
            if (t >= 1.0f) KillTimer(h, FADE_TIMER_ID);
            return 0;
        }
        if (w == 0x4B) {
            if (!g_ListeningForKey) {
                KillTimer(h, 0x4B);
                return 0;
            }
            if (!g_KeyCaptureArmed) {
                if (!IsAnyCaptureKeyDown()) g_KeyCaptureArmed = true;
                return 0;
            }
            const int virtualKey = FindCaptureKeyDown();
            if (virtualKey != 0)
                FinishKeyCapture(
                    virtualKey == VK_ESCAPE ? 0 : virtualKey);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (g_ListeningForKey) {
            FinishKeyCapture(w == VK_ESCAPE ? 0 : static_cast<int>(w));
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
        if (g_ListeningForKey) {
            FinishKeyCapture(w == VK_ESCAPE ? 0 : static_cast<int>(w));
            return 0;
        }
        break;

    case WM_XBUTTONDOWN:
        if (g_ListeningForKey) {
            FinishKeyCapture(
                GET_XBUTTON_WPARAM(w) == 1 ? VK_XBUTTON1 : VK_XBUTTON2);
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
        if (g_ListeningForKey) {
            FinishKeyCapture(VK_LBUTTON);
            return 0;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
            EspLog("[UI] down x=%d y=%d", GET_X_LPARAM(l), GET_Y_LPARAM(l));
            lks::modern_ui::mouse_down(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        }
        return 0;

    case WM_LBUTTONUP:
        {
            std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
            EspLog("[UI] up x=%d y=%d", GET_X_LPARAM(l), GET_Y_LPARAM(l));
            lks::modern_ui::mouse_up(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (g_ListeningForKey) {
            FinishKeyCapture(VK_RBUTTON);
            return 0;
        }
        break;

    case WM_MBUTTONDOWN:
        if (g_ListeningForKey) {
            FinishKeyCapture(VK_MBUTTON);
            return 0;
        }
        break;

    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)l;
        if (hdr->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(g_hTab);

            ShowWindow(g_hTabAim, sel == 0 ? SW_SHOW : SW_HIDE);
            ShowWindow(g_hTabEsp, sel == 1 ? SW_SHOW : SW_HIDE);
            ShowWindow(g_hTabMisc, sel == 2 ? SW_SHOW : SW_HIDE);
            ShowWindow(g_hTabConfig, sel == 3 ? SW_SHOW : SW_HIDE);
            InvalidateRect(g_hTab, nullptr, TRUE);
        }
        if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == g_hComboStyle) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                cd->clrText = RGB(200, 200, 200);
                return CDRF_NEWFONT;
            }
        }
    } return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Ui::Text);
        return (LRESULT)g_BgBrush;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w; SetBkColor(dc, Ui::Surface);
        SetTextColor(dc, Ui::TextHi);
        return (LRESULT)g_InputBrush;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (Ui::DrawControl(di)) return TRUE;
        if (di->CtlType == ODT_TAB) {
            BOOL sel = di->itemState & ODS_SELECTED;
            RECT r = di->rcItem;
            InflateRect(&r, -5, -5);
            Ui::RoundFill(di->hDC, r, 12, sel ? Ui::SurfaceHover : Ui::Canvas);
            if (sel) {
                RECT marker{r.left + 18, r.bottom - 3, r.right - 18, r.bottom};
                Ui::RoundFill(di->hDC, marker, 3, Ui::Accent);
            }
            wchar_t text[64] = {};
            TCITEMW ti = {TCIF_TEXT};
            ti.pszText = text;
            ti.cchTextMax = 64;
            TabCtrl_GetItem(g_hTab, di->itemID, &ti);
            SelectObject(di->hDC, g_FontBody);
            SetTextColor(di->hDC, sel ? Ui::TextHi : Ui::TextDim);
            SetBkMode(di->hDC, TRANSPARENT);
            DrawText(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
    } return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
        lks::modern_ui::paint();
        EndPaint(h, &ps);
    } return 0;

    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(h, &p);
        if (p.y < 40 && !lks::modern_ui::hit_caption_button(p.x, p.y)) return HTCAPTION;
        return HTCLIENT;
    }

    default: return DefWindowProc(h, m, w, l);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LksMenu";
    RegisterClass(&wc);

    const UINT dpi = GetDpiForSystem();
    int w = MulDiv(1100, static_cast<int>(dpi), 96);
    int h = MulDiv(880, static_cast<int>(dpi), 96);
    w = std::min(w, GetSystemMetrics(SM_CXSCREEN) - 80);
    h = std::min(h, GetSystemMetrics(SM_CYSCREEN) - 80);
    g_hWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        L"LksMenu",
        L"Lks667 Kernel RO CS2",
        WS_POPUP,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
        w, h, 0, 0, hInst, 0);
    if (!g_hWnd) return 0;

    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corners, sizeof(corners));

    SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA);
    ShowWindow(g_hWnd, show);
    UpdateWindow(g_hWnd);
    g_FadeStarted = GetTickCount();
    SetTimer(g_hWnd, FADE_TIMER_ID, 16, nullptr);
    SetTimer(g_hWnd, STARTUP_SOUND_TIMER_ID, 1300, nullptr);

    HudCreate(hInst, ConfigFolder().wstring());

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    g_Running.store(false, std::memory_order_release);
    if (g_DumpWorker.joinable()) g_DumpWorker.join();
    StopOverlay();
    g_UseKernelRead.store(false, std::memory_order_release);
    g_Client.Shutdown();
    HudDestroy();
    lks::modern_ui::shutdown();
    if (g_FontCons) DeleteObject(g_FontCons);
    if (g_FontSegoe) DeleteObject(g_FontSegoe);
    if (g_FontBody) DeleteObject(g_FontBody);
    if (g_FontSmall) DeleteObject(g_FontSmall);
    if (g_FontSection) DeleteObject(g_FontSection);
    if (g_InputBrush) DeleteObject(g_InputBrush);
    if (g_BgBrush) DeleteObject(g_BgBrush);
    return 0;
}
