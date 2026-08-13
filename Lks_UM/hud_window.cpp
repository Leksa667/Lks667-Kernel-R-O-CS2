#include "hud_window.hpp"
#include "ui_render.hpp"
#include "ui_theme.hpp"
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <string>

namespace {
using namespace dvm::ui;
using namespace dvm::theme;

constexpr int kWidth = 268;
constexpr int kHeight = 274;
constexpr int kPad = 16;

struct HudData {
    bool showInfo = false;
    bool showBomb = false;
    float fps = 0.f;
    int players = 0;
    float readMs = 0.f;
    float snapshotHz = 0.f;
    float presentMs = 0.f;
    int localTeam = 0;
    BombInfo bomb;
};

HudData g_data;
std::mutex g_mutex;
HWND g_hwnd = nullptr;
Renderer g_renderer;
std::wstring g_folder;
bool g_visible = false;
bool g_french = true;

const wchar_t* Tr(const wchar_t* french, const wchar_t* english) {
    return g_french ? french : english;
}

void SavePos() {
    if (g_folder.empty() || !g_hwnd) return;
    RECT rc{};
    GetWindowRect(g_hwnd, &rc);
    std::ofstream f(g_folder + L"\\hud_pos.cfg");
    if (f) f << rc.left << " " << rc.top << "\n";
}

void LoadPos(int& x, int& y) {
    x = 60;
    y = 200;
    if (g_folder.empty()) return;
    std::ifstream f(g_folder + L"\\hud_pos.cfg");
    if (!f) return;
    f >> x >> y;
    x = std::clamp(x, 0, std::max(0, GetSystemMetrics(SM_CXSCREEN) - kWidth));
    y = std::clamp(y, 0, std::max(0, GetSystemMetrics(SM_CYSCREEN) - kHeight));
}

void ApplyVisibility() {
    if (!g_hwnd) return;
    const bool wantVisible = g_data.showInfo || g_data.showBomb;
    if (wantVisible != g_visible) {
        g_visible = wantVisible;
        ShowWindow(g_hwnd, wantVisible ? SW_SHOW : SW_HIDE);
    }
    if (wantVisible) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void DrawBomb(const BombInfo& bomb, int localTeam) {
    const float x = (float)kPad;
    const float textW = (float)(kWidth - 2 * kPad);
    g_renderer.text(Tr(L"BOMBE", L"BOMB"), D2D1::RectF(x, 20, x + 90, 36), FontCaption, accent);
    if (bomb.site >= 0) {
        wchar_t site[16]{};
        swprintf_s(site, L"SITE %c", bomb.site == 0 ? L'A' : L'B');
        g_renderer.text(site, D2D1::RectF(kWidth - kPad - 70, 20, kWidth - kPad, 36),
            FontCaption, text_dim, AlignRight);
    }

    if (!bomb.valid) {
        g_renderer.text(Tr(L"EN ATTENTE", L"WAITING"), D2D1::RectF(x, 42, textW, 72),
            FontBodyStrong, text_dim);
        return;
    }
    if (bomb.carried) {
        if (bomb.carrierTeam != 0 && bomb.carrierTeam != localTeam) {
            g_renderer.text(bomb.carrierTeam == 2 ? Tr(L"PORTÉE PAR CT", L"CARRIED BY CT") : Tr(L"PORTÉE PAR T", L"CARRIED BY T"),
                D2D1::RectF(x, 42, textW, 74), FontDisplay, accent);
        } else {
            g_renderer.text(Tr(L"EN ATTENTE", L"WAITING"), D2D1::RectF(x, 42, textW, 72),
                FontBodyStrong, text_dim);
        }
        return;
    }
    if (bomb.dropped) {
        g_renderer.text(Tr(L"AU SOL", L"DROPPED"), D2D1::RectF(x, 42, textW, 74),
            FontDisplay, accent);
        return;
    }

    wchar_t buf[64]{};
    D2D1_COLOR_F color = accent;
    if (bomb.defused) {
        swprintf_s(buf, L"%s", Tr(L"DÉSAMORCÉE", L"DEFUSED"));
        color = warn;
    } else {
        const float sinceSample = bomb.sampledAtMs ?
            (float)(GetTickCount64() - bomb.sampledAtMs) / 1000.f : 0.f;
        const float remaining = std::max(0.f, bomb.timer - sinceSample);
        swprintf_s(buf, L"%.1f s", remaining);
        color = remaining < 5.f ? danger : remaining < 10.f ? warn : accent;
    }
    g_renderer.text(buf, D2D1::RectF(x, 42, textW, 74), FontDisplay, color);

    auto track = D2D1::RectF(x, 74, (float)(kWidth - kPad), 80);
    g_renderer.fill_round(track, 3, stroke_strong);
    if (!bomb.defused) {
        const float sinceSample = bomb.sampledAtMs ?
            (float)(GetTickCount64() - bomb.sampledAtMs) / 1000.f : 0.f;
        const float remaining = std::max(0.f, bomb.timer - sinceSample);
        const float ratio = std::clamp(
            remaining / std::max(1.f, bomb.timerLength), 0.f, 1.f);
        if (ratio > 0.f)
            g_renderer.fill_round(
                D2D1::RectF(x, 74, x + (kWidth - 2 * kPad) * ratio, 80), 3, color);
    }

    if (bomb.beingDefused && bomb.defuseLength > 0.f) {
        const float sinceSample = bomb.sampledAtMs ?
            (float)(GetTickCount64() - bomb.sampledAtMs) / 1000.f : 0.f;
        const float defuseRemaining =
            std::max(0.f, bomb.defuseRemaining - sinceSample);
        const float bombRemaining = std::max(0.f, bomb.timer - sinceSample);
        const bool hasTime = defuseRemaining <= bombRemaining;
        const bool hasKit = bomb.defuseLength <= 5.5f;
        swprintf_s(buf, L"%s %.1fs %s %s", Tr(L"DÉSAMORÇAGE", L"DEFUSE"), defuseRemaining,
            hasKit ? L"KIT" : Tr(L"SANS KIT", L"NO KIT"),
            hasTime ? Tr(L"À TEMPS", L"SAFE") : Tr(L"TROP TARD", L"TOO LATE"));
        g_renderer.text(buf, D2D1::RectF(x, 88, textW, 106),
            FontCaption, hasTime ? warn : danger);
    }
}

void DrawInfo(const HudData& d) {
    const float x = (float)kPad;
    float y = 130.f;
    const auto row = [&](const wchar_t* label, const wchar_t* value,
        D2D1_COLOR_F valueColor) {
        g_renderer.text(label, D2D1::RectF(x, y, x + 92, y + 24), FontBody, text_dim);
        g_renderer.text(value, D2D1::RectF(x + 92, y, (float)(kWidth - kPad), y + 24),
            FontBodyStrong, valueColor, AlignRight);
        y += 26.f;
    };
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.0f", d.fps);
    row(L"FPS", buf, accent);
    swprintf_s(buf, L"%d", d.players);
    row(Tr(L"Joueurs", L"Players"), buf, text_hi);
    swprintf_s(buf, L"%.2f ms", d.readMs);
    row(L"Kernel", buf, text);
    swprintf_s(buf, L"%.1f Hz", d.snapshotHz);
    row(L"Snapshots", buf, text);
    swprintf_s(buf, L"%.2f ms", d.presentMs);
    row(L"Present", buf, text);
}

void Paint() {
    if (!g_renderer.ready()) return;
    HudData d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        d = g_data;
    }
    g_renderer.begin();
    const bool both = d.showBomb && d.showInfo;
    if (d.showBomb) DrawBomb(d.bomb, d.localTeam);
    if (both)
        g_renderer.fill(D2D1::RectF(kPad, 116, (float)(kWidth - kPad), 117),
            stroke);
    if (d.showInfo) DrawInfo(d);
    g_renderer.end();
}

LRESULT CALLBACK HudWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_renderer.create_device_independent();
        g_renderer.create_target(hwnd, 96);
        SetTimer(hwnd, 1, 33, nullptr);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SIZE:
        g_renderer.resize(LOWORD(l), HIWORD(l));
        return 0;
    case WM_NCHITTEST:
        return HTCAPTION;
    case WM_EXITSIZEMOVE:
        SavePos();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_renderer.destroy();
        SavePos();
        return 0;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
}
} // namespace

bool HudCreate(HINSTANCE hInst, const std::wstring& configFolder) {
    if (g_hwnd) return true;
    g_folder = configFolder;
    WNDCLASS wc = {};
    wc.lpfnWndProc = HudWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LksHud";
    RegisterClass(&wc);
    int x = 60, y = 200;
    LoadPos(x, y);
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"LksHud", L"",
        WS_POPUP,
        x, y, kWidth, kHeight, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return false;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corners, sizeof(corners));
    SetLayeredWindowAttributes(g_hwnd, 0, 130, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_HIDE);
    return true;
}

void HudDestroy() {
    HWND hwnd = g_hwnd;
    g_hwnd = nullptr;
    if (hwnd) DestroyWindow(hwnd);
}

void HudUpdate(bool showInfo, bool showBomb, float fps, int players,
    float readMs, float snapshotHz, float presentMs, const BombInfo& bomb,
    int localTeam) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_data.showInfo = showInfo;
        g_data.showBomb = showBomb;
        g_data.fps = fps;
        g_data.players = players;
        g_data.readMs = readMs;
        g_data.snapshotHz = snapshotHz;
        g_data.presentMs = presentMs;
        g_data.localTeam = localTeam;
        g_data.bomb = bomb;
    }
    ApplyVisibility();
}

void HudRefresh() {
    if (!g_hwnd) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_data.showInfo = g_MiscSettings.showGameInfo;
        g_data.showBomb = g_MiscSettings.showBombTimer;
    }
    ApplyVisibility();
}

void HudSetFrench(bool french) {
    g_french = french;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}
