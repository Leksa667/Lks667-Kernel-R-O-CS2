// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#include "overlay.hpp"
#include "LksClient.hpp"
#include <tlhelp32.h>
#include <dwmapi.h>
#include <thread>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <random>
#include <cctype>
#pragma comment(lib, "dwmapi.lib")

extern LksClient g_Client;
EspSettings g_EspSettings;
AimSettings g_AimSettings;
MiscSettings g_MiscSettings;
ConfigSettings g_ConfigSettings;
std::recursive_mutex g_SettingsMutex;
std::vector<DamageEvent> g_DamageLog;
static std::mutex g_DamageLogMutex;
pNtReadVM g_NtReadVM = nullptr;
pNtWriteVM g_NtWriteVM = nullptr;
static std::thread g_OverlayThread;

static void OvStatus(const wchar_t* s) {
    if (g_hStatus) SetWindowText(g_hStatus, s);
}

void EspLog(const char* fmt, ...) {
    UNREFERENCED_PARAMETER(fmt);
}

static void InitNtRead() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        g_NtReadVM = (pNtReadVM)GetProcAddress(ntdll, "NtReadVirtualMemory");
        g_NtWriteVM = (pNtWriteVM)GetProcAddress(ntdll, "NtWriteVirtualMemory");
        EspLog("NtRead=0x%llX NtWrite=0x%llX", (uintptr_t)(void*)g_NtReadVM, (uintptr_t)(void*)g_NtWriteVM);
    }
}

static uintptr_t FindStaticOff(const char* mod, const char* name) {
    for (const auto& o : g_Offsets)
        if (o.mod == mod && o.className.empty() && o.name == name)
            return o.value;
    return 0;
}

static uintptr_t FindFieldOff(
    const char* mod,
    const char* className,
    const char* name) {
    for (const auto& o : g_Offsets)
        if (o.mod == mod && o.className == className && o.name == name)
            return o.value;
    return 0;
}

static void LogMissingFieldOff(
    const char* mod,
    const char* className,
    const char* name,
    uintptr_t value) {
    if (!value) {
        EspLog("[Offsets] MISSING %s!%s::%s",
            mod, className, name);
    }
}

struct ExactReadJob {
    uintptr_t address;
    void* destination;
    uint16_t size;
};

static size_t ExecuteExactReads(const std::vector<ExactReadJob>& jobs) {
    
    constexpr size_t batchSize = 96;
    size_t base = 0;
    size_t completed = 0;
    while (base < jobs.size()) {
        uint64_t addresses[LKS_MAX_BATCH_READS] = {};
        uint16_t sizes[LKS_MAX_BATCH_READS] = {};
        size_t predictedResponse = sizeof(LKS_BATCH_RESPONSE_HEADER);
        uint16_t count = 0;
        while (base + count < jobs.size() && count < batchSize) {
            const auto& job = jobs[base + count];
            const size_t nextSize = predictedResponse +
                sizeof(LKS_BATCH_READ_RESULT) + job.size;
            if (nextSize > LKS_MAX_FRAME_PAYLOAD) break;
            predictedResponse = nextSize;
            ++count;
        }
        if (count == 0) {
            ++base;
            continue;
        }

        for (uint16_t i = 0; i < count; ++i) {
            addresses[i] = jobs[base + i].address;
            sizes[i] = jobs[base + i].size;
        }

        auto results =
            g_Client.KernelReadBatch(addresses, sizes, count);
        for (uint16_t i = 0; i < count; ++i) {
            const auto& job = jobs[base + i];
            if (results.size() > i &&
                results[i].size() == job.size) {
                memcpy(job.destination, results[i].data(), job.size);
                ++completed;
            }
        }
        base += count;
    }
    return completed;
}

static uintptr_t GetModuleBase(HANDLE hProc, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(hProc));
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me = {sizeof(me)};
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) do {
        if (_wcsicmp(me.szModule, name) == 0) { base = (uintptr_t)me.modBaseAddr; break; }
    } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    return base;
}

static void DrawBox(HDC dc, int x, int y, int w, int h, COLORREF col, int style) {
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const auto drawPath = [&]() {
        if (style == BOX_CORNER_2D) {
            const int corner = std::clamp(std::min(w, h) / 4, 7, 16);
            for (int i = 0; i < 4; ++i) {
                const int cx = (i & 2) ? x + w : x;
                const int cy = (i & 1) ? y + h : y;
                const int dx = (i & 2) ? -corner : corner;
                const int dy = (i & 1) ? -corner : corner;
                MoveToEx(dc, cx, cy + dy, nullptr);
                LineTo(dc, cx, cy);
                LineTo(dc, cx + dx, cy);
            }
        } else if (style == BOX_ROUNDED_2D) {
            RoundRect(dc, x, y, x + w, y + h, 10, 10);
        } else {
            Rectangle(dc, x, y, x + w, y + h);
        }
    };

    HPEN shadow = CreatePen(PS_SOLID, 4, RGB(12, 12, 15));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, shadow));
    drawPath();
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    SelectObject(dc, pen);
    drawPath();
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(shadow);
    DeleteObject(pen);
}

static bool Draw3DBox(
    HDC dc,
    const Vec3& origin,
    float topZ,
    const Matrix4x4& vm,
    int sw,
    int sh,
    COLORREF col,
    bool cornersOnly) {
    constexpr float halfWidth = 16.f;
    const float bottomZ = origin.z;
    if (topZ <= bottomZ + 8.f) topZ = bottomZ + 72.f;

    const Vec3 world[8] = {
        {origin.x - halfWidth, origin.y - halfWidth, bottomZ},
        {origin.x + halfWidth, origin.y - halfWidth, bottomZ},
        {origin.x + halfWidth, origin.y + halfWidth, bottomZ},
        {origin.x - halfWidth, origin.y + halfWidth, bottomZ},
        {origin.x - halfWidth, origin.y - halfWidth, topZ},
        {origin.x + halfWidth, origin.y - halfWidth, topZ},
        {origin.x + halfWidth, origin.y + halfWidth, topZ},
        {origin.x - halfWidth, origin.y + halfWidth, topZ}
    };
    Vec2 screen[8] = {};
    for (int i = 0; i < 8; ++i) {
        screen[i] = W2S(world[i], vm, sw, sh);
        if (screen[i].x < 0.f || screen[i].y < 0.f) return false;
    }

    static constexpr int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    const auto drawEdges = [&]() {
        for (const auto& edge : edges) {
            const Vec2& a = screen[edge[0]];
            const Vec2& b = screen[edge[1]];
            if (!cornersOnly) {
                MoveToEx(dc, static_cast<int>(a.x), static_cast<int>(a.y), nullptr);
                LineTo(dc, static_cast<int>(b.x), static_cast<int>(b.y));
                continue;
            }

            constexpr float part = 0.27f;
            const Vec2 fromA = {
                a.x + (b.x - a.x) * part,
                a.y + (b.y - a.y) * part};
            const Vec2 fromB = {
                b.x + (a.x - b.x) * part,
                b.y + (a.y - b.y) * part};
            MoveToEx(dc, static_cast<int>(a.x), static_cast<int>(a.y), nullptr);
            LineTo(dc, static_cast<int>(fromA.x), static_cast<int>(fromA.y));
            MoveToEx(dc, static_cast<int>(b.x), static_cast<int>(b.y), nullptr);
            LineTo(dc, static_cast<int>(fromB.x), static_cast<int>(fromB.y));
        }
    };

    HPEN shadow = CreatePen(PS_SOLID, 4, RGB(12, 12, 15));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, shadow));
    drawEdges();
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    SelectObject(dc, pen);
    drawEdges();
    SelectObject(dc, oldPen);
    DeleteObject(shadow);
    DeleteObject(pen);
    return true;
}

static void DrawSkeleton(HDC dc, const std::vector<Vec3>& bones, const Matrix4x4& vm, int sw, int sh, COLORREF col) {
    if (bones.size() <= BONE_CHEST) return;
    const Vec2 headScale = W2S(bones[BONE_HEAD], vm, sw, sh);
    const Vec2 pelvisScale = W2S(bones[BONE_PELVIS], vm, sw, sh);
    if (headScale.x < 0.f || headScale.y < 0.f ||
        pelvisScale.x < 0.f || pelvisScale.y < 0.f) return;
    const float projectedHeight = std::abs(pelvisScale.y - headScale.y);
    const auto drawBones = [&]() {
        for (const auto& connection : g_BoneConnections) {
            const Vec2 p1 = W2S(bones[connection.b1], vm, sw, sh);
            const Vec2 p2 = W2S(bones[connection.b2], vm, sw, sh);
            if (p1.x < 0.f || p1.y < 0.f ||
                p2.x < 0.f || p2.y < 0.f) continue;
            MoveToEx(dc, static_cast<int>(p1.x), static_cast<int>(p1.y), nullptr);
            LineTo(dc, static_cast<int>(p2.x), static_cast<int>(p2.y));
        }
    };

    const int shadowWidth = projectedHeight > 70.f ? 3 : 2;
    HPEN shadow = CreatePen(PS_SOLID, shadowWidth, RGB(8, 10, 14));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, shadow));
    drawBones();
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    SelectObject(dc, pen);
    drawBones();

    static constexpr BoneID joints[] = {
        BONE_L_ELBOW, BONE_R_ELBOW,
        BONE_L_KNEE, BONE_R_KNEE
    };
    HBRUSH jointBrush = CreateSolidBrush(col);
    HGDIOBJ oldBrush = SelectObject(dc, jointBrush);
    
    if (projectedHeight >= 55.f) {
        for (const BoneID joint : joints) {
            const Vec2 point = W2S(bones[joint], vm, sw, sh);
            if (point.x < 0.f || point.y < 0.f) continue;
            const int px = static_cast<int>(point.x);
            const int py = static_cast<int>(point.y);
            Ellipse(dc, px - 1, py - 1, px + 2, py + 2);
        }
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(jointBrush);
    DeleteObject(shadow);
    DeleteObject(pen);
}

static void DrawHeadEsp(
    HDC dc,
    const std::vector<Vec3>& bones,
    const Matrix4x4& vm,
    int sw,
    int sh,
    COLORREF col) {
    if (bones.size() <= BONE_HEAD) return;
    const Vec2 head = W2S(bones[BONE_HEAD], vm, sw, sh);
    const Vec2 neck = W2S(bones[BONE_NECK], vm, sw, sh);
    if (head.x < 0.f || head.y < 0.f || neck.x < 0.f || neck.y < 0.f)
        return;

    const float dx = head.x - neck.x;
    const float dy = head.y - neck.y;
    const float headNeckDistance = std::sqrt(dx * dx + dy * dy);
    const int radius = std::clamp(
        static_cast<int>(std::lround(headNeckDistance * 0.58f)),
        2,
        10);
    const int cx = static_cast<int>(head.x);
    const int cy = static_cast<int>(head.y);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN shadow = CreatePen(
        PS_SOLID, radius >= 5 ? 3 : 2, RGB(8, 10, 14));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, shadow));
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    SelectObject(dc, pen);
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(shadow);
    DeleteObject(pen);
}

static void DrawHealthBar(HDC dc, int x, int y, int h, int hp, int maxHp) {
    if (maxHp <= 0) return;
    float pct = (float)hp / maxHp;
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;
    int bh = (int)(h * pct);
    constexpr int barWidth = 5;
    RECT track{ x, y, x + barWidth, y + h };
    HBRUSH bg = CreateSolidBrush(RGB(18, 20, 25));
    FillRect(dc, &track, bg);
    DeleteObject(bg);
    if (bh > 0) {
        RECT fill{ x, y + h - bh, x + barWidth, y + h };
        HBRUSH fg = CreateSolidBrush(RGB(245, 45, 58));
        FillRect(dc, &fill, fg);
        DeleteObject(fg);
    }
}

static void DrawArmorBar(HDC dc, int x, int y, int h, int armor) {
    if (armor <= 0) return;
    float pct = (float)armor / 100.f;
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;
    int bh = (int)(h * pct);
    constexpr int barWidth = 5;
    RECT track{ x, y, x + barWidth, y + h };
    HBRUSH bg = CreateSolidBrush(RGB(18, 20, 25));
    FillRect(dc, &track, bg);
    DeleteObject(bg);
    if (bh > 0) {
        RECT fill{ x, y + h - bh, x + barWidth, y + h };
        HBRUSH fg = CreateSolidBrush(RGB(35, 135, 255));
        FillRect(dc, &fill, fg);
        DeleteObject(fg);
    }
}

static void DrawText(HDC dc, int x, int y, COLORREF col, const wchar_t* txt) {
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    TextOut(dc, x, y, txt, (int)wcslen(txt));
}

static void DrawWorldLabel(
    HDC dc, const Vec2& point, COLORREF color, const std::string& text) {
    wchar_t label[64] = {};
    MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), -1, label,
        static_cast<int>(std::size(label)));
    const int x = static_cast<int>(point.x);
    const int y = static_cast<int>(point.y);
    HPEN markerPen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, markerPen));
    MoveToEx(dc, x - 3, y, nullptr); LineTo(dc, x + 4, y);
    MoveToEx(dc, x, y - 3, nullptr); LineTo(dc, x, y + 4);
    SelectObject(dc, oldPen);
    DeleteObject(markerPen);
    DrawText(dc, x + 6, y - 7, RGB(8, 10, 14), label);
    DrawText(dc, x + 5, y - 8, color, label);
}

#if 0
static void ReadPlayersLegacy(HANDLE hProc, uintptr_t client, Matrix4x4& vmOut, std::vector<PlayerEnt>& out) {
    out.clear();
    if (!hProc || !client) {
        EspLog("[ReadPlayers] hProc=%p client=0x%llX – abort", hProc, client);
        return;
    }

    uintptr_t eListOff = FindOff("client.dll", "dwEntityList");
    uintptr_t lPawnOff = FindOff("client.dll", "dwLocalPlayerPawn");
    uintptr_t vmOff    = FindOff("client.dll", "dwViewMatrix");
    EspLog("[ReadPlayers] offsets: dwEntityList=0x%llX dwLocalPlayerPawn=0x%llX dwViewMatrix=0x%llX",
        eListOff, lPawnOff, vmOff);
    if (!eListOff || !lPawnOff || !vmOff) {
        EspLog("[ReadPlayers] MISSING critical offsets");
        OvStatus(L"[-] Missing critical ESP offsets");
        return;
    }

    uintptr_t gsOff       = FindOff("client.dll", "m_pGameSceneNode");
    uintptr_t msOff       = FindOff("client.dll", "m_modelState");
    uintptr_t hpOff       = FindOff("client.dll", "m_iHealth");
    uintptr_t teamOff     = FindOff("client.dll", "m_iTeamNum");
    uintptr_t posOff      = FindOff("client.dll", "m_vOldOrigin");
    uintptr_t nameOff     = FindOff("client.dll", "m_iszPlayerName");
    uintptr_t pawnHandleOff = FindOff("client.dll", "m_hPlayerPawn");
    uintptr_t armorOff    = FindOff("client.dll", "m_ArmorValue");
    uintptr_t espOff      = FindOff("client.dll", "m_entitySpottedState");
    uintptr_t bspOff      = FindOff("client.dll", "m_bSpotted");

    auto batch8 = [&](const uint64_t* addrs, const uint16_t* sizes) -> std::vector<std::vector<uint8_t>> {
        return g_Client.KernelReadBatch(addrs, sizes, 16);
    };
    auto batchN = [&](const uint64_t* addrs, const uint16_t* sizes, uint8_t n) -> std::vector<std::vector<uint8_t>> {
        return g_Client.KernelReadBatch(addrs, sizes, n);
    };
    auto readPtr = [&](uint64_t addr) -> uintptr_t {
        uint64_t a = addr;
        uint16_t s = 8;
        auto r = g_Client.KernelReadBatch(&a, &s, 1);
        if (!r.empty() && r[0].size() == 8) {
            uintptr_t v;
            memcpy(&v, r[0].data(), 8);
            return v;
        }
        return Read<uintptr_t>(hProc, (uintptr_t)addr);
    };

    static const int MAX_ENT = 64;
    static const int BATCH_SZ = 16;
    uint64_t addrs[BATCH_SZ];
    uint16_t szs[BATCH_SZ];
    std::vector<std::vector<uint8_t>> batch;

    
    addrs[0] = client + eListOff;
    addrs[1] = client + lPawnOff;
    addrs[2] = client + vmOff;
    addrs[3] = client + eListOff + 0x10;
    szs[0] = szs[1] = szs[2] = szs[3] = 8;
    batch = g_Client.KernelReadBatch(addrs, szs, 4);
    uintptr_t entityList  = batch.size() > 0 && batch[0].size() == 8 ? *(uintptr_t*)batch[0].data() : 0;
    uintptr_t localPawn   = batch.size() > 1 && batch[1].size() == 8 ? *(uintptr_t*)batch[1].data() : 0;
    memcpy(&vmOut, batch.size() > 2 && batch[2].size() == 64 ? batch[2].data() : (uint8_t*)&vmOut, 64);
    uintptr_t listBase    = batch.size() > 3 && batch[3].size() == 8 ? *(uintptr_t*)batch[3].data() : 0;

    EspLog("[ReadPlayers] entityList=0x%llX listBase=0x%llX localPawn=0x%llX",
        entityList, listBase, localPawn);

    if (!entityList || !listBase) {
        EspLog("[ReadPlayers] entityList/listBase == NULL");
        OvStatus(L"[-] entityList == NULL");
        return;
    }

    
    uintptr_t controllers[MAX_ENT] = {};
    for (int b = 0; b < 4; b++) {
        int base = b * BATCH_SZ;
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j + 1;
            addrs[j] = listBase + 0x70ULL * (idx & 0x1FF);
            szs[j] = 8;
        }
        batch = g_Client.KernelReadBatch(addrs, szs, BATCH_SZ);
        for (int j = 0; j < BATCH_SZ; j++) {
            if (batch.size() > j && batch[j].size() == 8)
                controllers[base + j] = *(uintptr_t*)batch[j].data();
        }
    }

    
    uint32_t pawnHandles[MAX_ENT] = {};
    for (int b = 0; b < 4; b++) {
        int base = b * BATCH_SZ;
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            if (!controllers[idx]) { addrs[j] = 0; szs[j] = 0; continue; }
            addrs[j] = controllers[idx] + pawnHandleOff;
            szs[j] = 4;
        }
        batch = g_Client.KernelReadBatch(addrs, szs, BATCH_SZ);
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            if (!controllers[idx]) continue;
            if (batch.size() > j && batch[j].size() == 4)
                pawnHandles[idx] = *(uint32_t*)batch[j].data();
        }
    }

    
    uintptr_t pawnPtrs[MAX_ENT] = {};
    for (int b = 0; b < 4; b++) {
        int base = b * BATCH_SZ;
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            uint32_t ph = pawnHandles[idx];
            if (!ph || ph == 0xFFFFFFFF) { addrs[j] = 0; szs[j] = 0; continue; }
            int entIdx = ph & 0x7FFF;
            addrs[j] = entityList + 0x10 + 8 * ((entIdx & 0x7FFF) >> 9) + 0x70 * (entIdx & 0x1FF);
            szs[j] = 8;
        }
        batch = g_Client.KernelReadBatch(addrs, szs, BATCH_SZ);
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            if (!pawnHandles[idx] || pawnHandles[idx] == 0xFFFFFFFF) continue;
            if (batch.size() > j && batch[j].size() == 8)
                pawnPtrs[idx] = *(uintptr_t*)batch[j].data();
        }
    }

    
    const int PAWN_BLOCK = 256;
    uint8_t pawnBlocks[MAX_ENT][PAWN_BLOCK];
    memset(pawnBlocks, 0, sizeof(pawnBlocks));
    for (int b = 0; b < 4; b++) {
        int base = b * BATCH_SZ;
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            addrs[j] = pawnPtrs[idx];
            szs[j] = PAWN_BLOCK;
        }
        batch = g_Client.KernelReadBatch(addrs, szs, BATCH_SZ);
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            if (!pawnPtrs[idx]) continue;
            if (batch.size() > j && !batch[j].empty())
                memcpy(pawnBlocks[idx], batch[j].data(), std::min<int>(batch[j].size(), PAWN_BLOCK));
        }
    }

    
    char pnames[MAX_ENT][64];
    memset(pnames, 0, sizeof(pnames));
    for (int b = 0; b < 4; b++) {
        int base = b * BATCH_SZ;
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            if (!controllers[idx]) { szs[j] = 0; continue; }
            addrs[j] = controllers[idx] + nameOff;
            szs[j] = 64;
        }
        batch = g_Client.KernelReadBatch(addrs, szs, BATCH_SZ);
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            if (!controllers[idx]) continue;
            if (batch.size() > j && !batch[j].empty())
                memcpy(pnames[idx], batch[j].data(), std::min<size_t>(batch[j].size(), 63));
        }
    }

    
    
    const int BONE_BATCH = 8;
    const int BONE_SZ = 28 * sizeof(Vec3); 
    uint8_t boneBuf[MAX_ENT][BONE_SZ];
    memset(boneBuf, 0, sizeof(boneBuf));
    uintptr_t gsnAddrs[MAX_ENT], boneAddrs[MAX_ENT];
    for (int i = 0; i < MAX_ENT; i++) {
        gsnAddrs[i] = pawnPtrs[i] ? pawnPtrs[i] + gsOff : 0;
    }

    
    for (int b = 0; b < 4; b++) {
        int base = b * BATCH_SZ;
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            addrs[j] = gsnAddrs[idx];
            szs[j] = 8;
        }
        batch = g_Client.KernelReadBatch(addrs, szs, BATCH_SZ);
        for (int j = 0; j < BATCH_SZ; j++) {
            int idx = base + j;
            if (!gsnAddrs[idx] || !gsnAddrs[idx]) continue;
            uintptr_t gsn = batch.size() > j && batch[j].size() == 8 ? *(uintptr_t*)batch[j].data() : 0;
            boneAddrs[idx] = gsn ? gsn + msOff + 0x80 : 0;
        }
    }

    
    for (int b = 0; b < 8; b++) {
        int base = b * BONE_BATCH;
        for (int j = 0; j < BONE_BATCH; j++) {
            int idx = base + j;
            addrs[j] = boneAddrs[idx];
            szs[j] = BONE_SZ;
        }
        batch = g_Client.KernelReadBatch(addrs, szs, BONE_BATCH);
        for (int j = 0; j < BONE_BATCH; j++) {
            int idx = base + j;
            if (!boneAddrs[idx]) continue;
            if (batch.size() > j && batch[j].size() == BONE_SZ)
                memcpy(boneBuf[idx], batch[j].data(), BONE_SZ);
        }
    }

    
    int count = 0, ctrlFound = 0;
    for (int i = 1; i <= MAX_ENT; i++) {
        int idx = i - 1;
        uintptr_t controller = controllers[idx];
        if (!controller) continue;
        ctrlFound++;

        uint32_t pawnHandle = pawnHandles[idx];
        if (!pawnHandle || pawnHandle == (uint32_t)-1) continue;
        uintptr_t pawnPtr = pawnPtrs[idx];
        if (!pawnPtr || pawnPtr == localPawn) continue;

        PlayerEnt p;
        p.pawn = pawnPtr;
        p.controller = controller;
        const uint8_t* pb = pawnBlocks[idx];
        p.health = pb[hpOff] | (pb[hpOff+1] << 8) | (pb[hpOff+2] << 16) | (pb[hpOff+3] << 24);
        p.health = *(int*)(pb + hpOff);
        p.maxHealth = 100;
        p.team = *(uint8_t*)(pb + teamOff);
        memcpy(&p.origin, pb + posOff, sizeof(Vec3));
        p.alive = (p.health > 0 && p.health <= 100);
        p.armor = *(int*)(pb + armorOff);
        p.visible = true;
        if (espOff && bspOff)
            p.visible = *(bool*)(pb + espOff + bspOff);
        p.flashDuration = 0.f;
        p.name = pnames[idx];
        if (boneAddrs[idx]) {
            p.bones.resize(28);
            memcpy(p.bones.data(), boneBuf[idx], BONE_SZ);
        }

        EspLog("[ReadPlayers] #%d '%s' pawn=0x%llX ctrl=0x%llX hp=%d/%d team=%d "
            "alive=%d vis=%d armor=%d origin=(%.0f %.0f %.0f) bones=%zu",
            i, p.name.c_str(), pawnPtr, controller, p.health, p.maxHealth, p.team,
            p.alive, p.visible, p.armor, p.origin.x, p.origin.y, p.origin.z, p.bones.size());

        out.push_back(p);
        count++;
    }

    EspLog("[ReadPlayers] DONE: %d players, %d controllers found", count, ctrlFound);
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[+] ESP: %d players", count);
        wchar_t wb[128];
        swprintf_s(wb, L"%hs", buf);
        OvStatus(wb);
    }
}
#endif

static bool ReadPlayers(
    HANDLE hProc,
    uintptr_t client,
    Matrix4x4& viewMatrix,
    std::vector<PlayerEnt>& output,
    uintptr_t& localPawnOut,
    int& localTeamOut) {
    output.clear();
    localPawnOut = 0;
    localTeamOut = 0;
    ZeroMemory(&viewMatrix, sizeof(viewMatrix));
    if (!hProc || !client) return false;

    constexpr int maxEntities = 64;
    constexpr uintptr_t entityStride = 0x70;

    const uintptr_t entityListOff =
        FindStaticOff("client.dll", "dwEntityList");
    const uintptr_t localPawnOff =
        FindStaticOff("client.dll", "dwLocalPlayerPawn");
    const uintptr_t viewMatrixOff =
        FindStaticOff("client.dll", "dwViewMatrix");
    const uintptr_t sceneNodeOff =
        FindFieldOff(
            "client.dll", "C_BaseEntity", "m_pGameSceneNode");
    const uintptr_t modelStateOff =
        FindFieldOff(
            "client.dll", "CSkeletonInstance", "m_modelState");
    const uintptr_t healthOff =
        FindFieldOff("client.dll", "C_BaseEntity", "m_iHealth");
    const uintptr_t lifeStateOff =
        FindFieldOff("client.dll", "C_BaseEntity", "m_lifeState");
    const uintptr_t teamOff =
        FindFieldOff("client.dll", "C_BaseEntity", "m_iTeamNum");
    const uintptr_t originOff =
        FindFieldOff(
            "client.dll", "C_BasePlayerPawn", "m_vOldOrigin");
    const uintptr_t nameOff =
        FindFieldOff(
            "client.dll", "CBasePlayerController", "m_iszPlayerName");
    const uintptr_t pawnHandleOff =
        FindFieldOff(
            "client.dll", "CCSPlayerController", "m_hPlayerPawn");
    const uintptr_t controllerAliveOff =
        FindFieldOff(
            "client.dll", "CCSPlayerController", "m_bPawnIsAlive");
    const uintptr_t armorOff =
        FindFieldOff(
            "client.dll", "C_CSPlayerPawn", "m_ArmorValue");
    const uintptr_t spottedStateOff =
        FindFieldOff(
            "client.dll", "C_CSPlayerPawn", "m_entitySpottedState");
    const uintptr_t spottedMaskOff =
        FindFieldOff(
            "client.dll", "EntitySpottedState_t", "m_bSpottedByMask");
    const uintptr_t observerServicesOff = FindFieldOff(
        "client.dll", "C_BasePlayerPawn", "m_pObserverServices");
    const uintptr_t observerTargetOff = FindFieldOff(
        "client.dll", "CPlayer_ObserverServices", "m_hObserverTarget");

    if (!entityListOff || !localPawnOff || !viewMatrixOff ||
        !sceneNodeOff || !modelStateOff || !healthOff || !teamOff ||
        !originOff || !nameOff || !pawnHandleOff || !armorOff) {
        static bool loggedMissingOffsets = false;
        if (!loggedMissingOffsets) {
            loggedMissingOffsets = true;
            LogMissingFieldOff("client.dll", "C_BaseEntity", "m_pGameSceneNode", sceneNodeOff);
            LogMissingFieldOff("client.dll", "CSkeletonInstance", "m_modelState", modelStateOff);
            LogMissingFieldOff("client.dll", "C_BaseEntity", "m_iHealth", healthOff);
            LogMissingFieldOff("client.dll", "C_BaseEntity", "m_iTeamNum", teamOff);
            LogMissingFieldOff("client.dll", "C_BasePlayerPawn", "m_vOldOrigin", originOff);
            LogMissingFieldOff("client.dll", "CBasePlayerController", "m_iszPlayerName", nameOff);
            LogMissingFieldOff("client.dll", "CCSPlayerController", "m_hPlayerPawn", pawnHandleOff);
            LogMissingFieldOff("client.dll", "C_CSPlayerPawn", "m_ArmorValue", armorOff);
            if (!entityListOff) EspLog("[Offsets] MISSING client.dll!dwEntityList");
            if (!localPawnOff) EspLog("[Offsets] MISSING client.dll!dwLocalPlayerPawn");
            if (!viewMatrixOff) EspLog("[Offsets] MISSING client.dll!dwViewMatrix");
        }
        OvStatus(L"[-] Missing class-qualified ESP offsets");
        return false;
    }

    uintptr_t entityList = 0;
    uintptr_t localPawn = 0;
    const size_t criticalReads = ExecuteExactReads({
        {client + entityListOff, &entityList, sizeof(entityList)},
        {client + localPawnOff, &localPawn, sizeof(localPawn)},
        {client + viewMatrixOff, &viewMatrix, sizeof(viewMatrix)}
    });
    if (criticalReads != 3) return false;
    localPawnOut = localPawn;
    if (!entityList) return false;

    static uintptr_t cachedEntityList = 0;
    static std::chrono::steady_clock::time_point nextPointerRefresh = {};
    static std::array<uintptr_t, maxEntities> controllers = {};
    static std::array<uintptr_t, maxEntities> pawns = {};
    static std::array<uintptr_t, maxEntities> sceneNodes = {};
    static std::array<uintptr_t, maxEntities> boneArrays = {};
    static std::array<std::array<char, 128>, maxEntities> names = {};

    thread_local std::vector<ExactReadJob> jobs;
    jobs.clear();
    if (jobs.capacity() < maxEntities * 7)
        jobs.reserve(maxEntities * 7);
    const auto now = std::chrono::steady_clock::now();
    const bool refreshPointers =
        cachedEntityList != entityList || now >= nextPointerRefresh;
    if (refreshPointers) {
        cachedEntityList = entityList;
        nextPointerRefresh = now + std::chrono::milliseconds(500);
        controllers.fill(0);
        pawns.fill(0);
        sceneNodes.fill(0);
        boneArrays.fill(0);
        for (auto& name : names) name.fill('\0');

        uintptr_t controllerList = 0;
        const size_t controllerRootReads = ExecuteExactReads({{
            entityList + 0x10,
            &controllerList,
            sizeof(controllerList)}});

        if (controllerRootReads != 1 || !controllerList)
            return false;

        if (controllerList) {
            jobs.clear();
            for (int i = 0; i < maxEntities; ++i) {
                const uintptr_t index = static_cast<uintptr_t>(i + 1);
                jobs.push_back({
                    controllerList + entityStride * (index & 0x1FF),
                    &controllers[i],
                    sizeof(controllers[i])});
            }
            ExecuteExactReads(jobs);

            std::array<uint32_t, maxEntities> pawnHandles = {};
            jobs.clear();
            for (int i = 0; i < maxEntities; ++i) {
                if (controllers[i]) {
                    jobs.push_back({
                        controllers[i] + pawnHandleOff,
                        &pawnHandles[i],
                        sizeof(pawnHandles[i])});
                }
            }
            ExecuteExactReads(jobs);

            std::array<uintptr_t, maxEntities> pawnLists = {};
            jobs.clear();
            for (int i = 0; i < maxEntities; ++i) {
                const uint32_t handle = pawnHandles[i];
                if (!handle || handle == 0xFFFFFFFFu) continue;
                const uintptr_t index = handle & 0x7FFFu;
                if ((index >> 9) == 0) {
                    pawnLists[i] = controllerList;
                } else {
                    jobs.push_back({
                        entityList + 0x10 + 8 * (index >> 9),
                        &pawnLists[i],
                        sizeof(pawnLists[i])});
                }
            }
            ExecuteExactReads(jobs);

            jobs.clear();
            for (int i = 0; i < maxEntities; ++i) {
                if (pawnLists[i]) {
                    const uintptr_t index = pawnHandles[i] & 0x7FFFu;
                    jobs.push_back({
                        pawnLists[i] + entityStride * (index & 0x1FF),
                        &pawns[i],
                        sizeof(pawns[i])});
                }
            }
            ExecuteExactReads(jobs);

            jobs.clear();
            for (int i = 0; i < maxEntities; ++i) {
                if (controllers[i]) {
                    jobs.push_back({
                        controllers[i] + nameOff,
                        names[i].data(),
                        static_cast<uint16_t>(names[i].size())});
                }
                if (pawns[i]) {
                    jobs.push_back({
                        pawns[i] + sceneNodeOff,
                        &sceneNodes[i],
                        sizeof(sceneNodes[i])});
                }
            }
            ExecuteExactReads(jobs);

            jobs.clear();
            for (int i = 0; i < maxEntities; ++i) {
                if (sceneNodes[i]) {
                    jobs.push_back({
                        sceneNodes[i] + modelStateOff + 0x80,
                        &boneArrays[i],
                        sizeof(boneArrays[i])});
                }
            }
            ExecuteExactReads(jobs);
        }
    }

    uintptr_t viewedPawn = localPawn;
    if (localPawn && observerServicesOff && observerTargetOff) {
        uintptr_t observerServices = 0;
        uint32_t observerTarget = 0xFFFFFFFFu;
        if (ExecuteExactReads({{
                localPawn + observerServicesOff,
                &observerServices,
                sizeof(observerServices)}}) == 1 && observerServices &&
            ExecuteExactReads({{
                observerServices + observerTargetOff,
                &observerTarget,
                sizeof(observerTarget)}}) == 1 &&
            observerTarget != 0 && observerTarget != 0xFFFFFFFFu) {
            const uintptr_t targetIndex = observerTarget & 0x7FFFu;
            uintptr_t targetBucket = 0;
            uintptr_t targetPawn = 0;
            if (ExecuteExactReads({{
                    entityList + 0x10 + sizeof(uintptr_t) * (targetIndex >> 9),
                    &targetBucket,
                    sizeof(targetBucket)}}) == 1 && targetBucket &&
                ExecuteExactReads({{
                    targetBucket + entityStride * (targetIndex & 0x1FF),
                    &targetPawn,
                    sizeof(targetPawn)}}) == 1 && targetPawn)
                viewedPawn = targetPawn;
        }
    }

    std::array<int, maxEntities> health = {};
    std::array<uint8_t, maxEntities> lifeStates = {};
    std::array<uint8_t, maxEntities> controllerAlive = {};
    controllerAlive.fill(1);
    std::array<uint8_t, maxEntities> teams = {};
    std::array<Vec3, maxEntities> origins = {};
    std::array<int, maxEntities> armor = {};
    std::array<uint64_t, maxEntities> spottedMasks = {};
    int localControllerSlot = -1;
    for (int i = 0; i < maxEntities; ++i) {
        if (pawns[i] == viewedPawn) {
            
            
            localControllerSlot = i;
            break;
        }
    }

    jobs.clear();
    for (int i = 0; i < maxEntities; ++i) {
        if (!pawns[i]) continue;

        jobs.push_back({
            pawns[i] + healthOff, &health[i], sizeof(health[i])});
        if (lifeStateOff) {
            jobs.push_back({
                pawns[i] + lifeStateOff,
                &lifeStates[i],
                sizeof(lifeStates[i])});
        }
        if (controllerAliveOff && controllers[i]) {
            jobs.push_back({
                controllers[i] + controllerAliveOff,
                &controllerAlive[i],
                sizeof(controllerAlive[i])});
        }
        jobs.push_back({
            pawns[i] + teamOff, &teams[i], sizeof(teams[i])});
        jobs.push_back({
            pawns[i] + originOff, &origins[i], sizeof(origins[i])});
        jobs.push_back({
            pawns[i] + armorOff, &armor[i], sizeof(armor[i])});
        if (spottedStateOff && spottedMaskOff) {
            jobs.push_back({
                pawns[i] + spottedStateOff + spottedMaskOff,
                &spottedMasks[i],
                sizeof(spottedMasks[i])});
        }
    }
    struct BoneJointData {
        Vec3 position;
        uint8_t padding[20];
    };
    static_assert(sizeof(BoneJointData) == 32);
    
    
    thread_local std::array<
        std::array<BoneJointData, BONE_CHEST + 1>, maxEntities> bones;
    memset(bones.data(), 0, sizeof(bones));
    for (int i = 0; i < maxEntities; ++i) {
        if (boneArrays[i]) {
            jobs.push_back({
                boneArrays[i],
                bones[i].data(),
            static_cast<uint16_t>(sizeof(bones[i]))});
        }
    }
    
    
    const size_t statsJobCount = jobs.size();
    const size_t completedJobs = ExecuteExactReads(jobs);

    
    
    
    if (statsJobCount > 0 && completedJobs * 10 < statsJobCount * 7)
        return false;

    
    
    
    if (completedJobs < statsJobCount) {
        jobs.clear();
        for (int i = 0; i < maxEntities; ++i) {
            if (!pawns[i] || pawns[i] == viewedPawn) continue;
            const bool looksDead =
                health[i] <= 0 || health[i] > 100 ||
                (lifeStateOff && lifeStates[i] != 0) ||
                (controllerAliveOff && controllerAlive[i] == 0);
            if (!looksDead) continue;
            jobs.push_back({
                pawns[i] + healthOff, &health[i], sizeof(health[i])});
            if (lifeStateOff) {
                jobs.push_back({
                    pawns[i] + lifeStateOff,
                    &lifeStates[i],
                    sizeof(lifeStates[i])});
            }
            if (controllerAliveOff && controllers[i]) {
                jobs.push_back({
                    controllers[i] + controllerAliveOff,
                    &controllerAlive[i],
                    sizeof(controllerAlive[i])});
            }
        }
        if (!jobs.empty())
            ExecuteExactReads(jobs);
    }

    for (int i = 0; i < maxEntities; ++i) {
        if (!pawns[i]) continue;
        if (pawns[i] == viewedPawn) {
            localTeamOut = teams[i];
            continue;
        }

        PlayerEnt player = {};
        player.pawn = pawns[i];
        player.controller = controllers[i];
        player.health = health[i];
        player.maxHealth = 100;
        player.team = teams[i];
        player.origin = origins[i];
        player.armor = armor[i];
        player.alive =
            player.health > 0 && player.health <= 100 &&
            (!lifeStateOff || lifeStates[i] == 0) &&
            (!controllerAliveOff || controllerAlive[i] != 0);
        player.visible = localControllerSlot >= 0 &&
            localControllerSlot < 64 &&
            (spottedMasks[i] & (1ULL << localControllerSlot)) != 0;
        names[i].back() = '\0';
        player.name = names[i].data();
        if (boneArrays[i]) {
            player.bones.reserve(bones[i].size());
            for (const auto& bone : bones[i])
                player.bones.push_back(bone.position);
        }
        output.push_back(std::move(player));
    }
    
    
    
    return g_Client.IsTransportHealthy();
}

static std::string FriendlyWorldName(std::string name) {
    static const std::pair<const char*, const char*> aliases[] = {
        {"weapon_ak47", "AK-47"}, {"weapon_m4a1_silencer", "M4A1-S"},
        {"weapon_m4a1", "M4A4"}, {"weapon_awp", "AWP"},
        {"weapon_deagle", "Deagle"}, {"weapon_glock", "Glock"},
        {"weapon_hkp2000", "P2000"}, {"weapon_usp_silencer", "USP-S"},
        {"weapon_c4", "C4"}, {"weapon_hegrenade", "HE Grenade"},
        {"weapon_flashbang", "Flashbang"}, {"weapon_smokegrenade", "Smoke"},
        {"weapon_molotov", "Molotov"}, {"weapon_incgrenade", "Incendiary"},
        {"weapon_decoy", "Decoy"}, {"hegrenade_projectile", "HE Grenade"},
        {"flashbang_projectile", "Flashbang"},
        {"smokegrenade_projectile", "Smoke"},
        {"molotov_projectile", "Molotov"},
        {"decoy_projectile", "Decoy"}
    };
    for (const auto& [raw, friendly] : aliases)
        if (name == raw) return friendly;
    constexpr const char* prefix = "weapon_";
    if (name.rfind(prefix, 0) == 0) name.erase(0, strlen(prefix));
    if (!name.empty()) name[0] = static_cast<char>(toupper(name[0]));
    return name;
}

static bool ReadWorldEntities(
    uintptr_t client,
    const EspSettings& settings,
    const std::vector<PlayerEnt>& players,
    std::vector<WorldEnt>& output) {
    output.clear();
    if (!client ||
        (!settings.showWeapons && !settings.showGrenades && !settings.showBomb))
        return true;

    const uintptr_t entityListOff =
        FindStaticOff("client.dll", "dwEntityList");
    const uintptr_t gameSystemOff =
        FindStaticOff("client.dll", "dwGameEntitySystem");
    const uintptr_t highestOff =
        FindStaticOff("client.dll", "dwGameEntitySystem_highestEntityIndex");
    const uintptr_t identityOff = FindFieldOff(
        "client.dll", "CEntityInstance", "m_pEntity");
    const uintptr_t designerNameOff = FindFieldOff(
        "client.dll", "CEntityIdentity", "m_designerName");
    const uintptr_t sceneNodeOff = FindFieldOff(
        "client.dll", "C_BaseEntity", "m_pGameSceneNode");
    const uintptr_t originOff = FindFieldOff(
        "client.dll", "CGameSceneNode", "m_vecAbsOrigin");
    const uintptr_t parentOff = FindFieldOff(
        "client.dll", "CGameSceneNode", "m_pParent");
    const uintptr_t ownerOff = FindFieldOff(
        "client.dll", "C_BaseEntity", "m_hOwnerEntity");
    const uintptr_t ownerPawnOff = FindFieldOff(
        "client.dll", "C_BasePlayerWeapon", "m_hOwnerPawn");
    if (!entityListOff || !gameSystemOff || !highestOff || !identityOff ||
        !designerNameOff || !sceneNodeOff || !originOff || !ownerOff)
        return false;

    uintptr_t entityList = 0;
    uintptr_t gameSystem = 0;
    if (ExecuteExactReads({
            {client + entityListOff, &entityList, sizeof(entityList)},
            {client + gameSystemOff, &gameSystem, sizeof(gameSystem)}}) != 2 ||
        !entityList || !gameSystem)
        return false;

    int highest = 0;
    if (ExecuteExactReads({{
            gameSystem + highestOff, &highest, sizeof(highest)}}) != 1)
        return false;
    highest = std::clamp(highest, 64, 2048);

    const int bucketCount = (highest >> 9) + 1;
    std::vector<uintptr_t> buckets(bucketCount);
    std::vector<ExactReadJob> jobs;
    jobs.reserve(highest + 1);
    for (int bucket = 0; bucket < bucketCount; ++bucket)
        jobs.push_back({
            entityList + 0x10 + sizeof(uintptr_t) * bucket,
            &buckets[bucket], sizeof(uintptr_t)});
    ExecuteExactReads(jobs);

    std::vector<uintptr_t> entities(highest + 1);
    jobs.clear();
    for (int index = 1; index <= highest; ++index) {
        const uintptr_t bucket = buckets[index >> 9];
        if (!bucket) continue;
        jobs.push_back({
            bucket + 0x70ULL * (index & 0x1FF),
            &entities[index], sizeof(uintptr_t)});
    }
    ExecuteExactReads(jobs);

    std::vector<uintptr_t> identities(highest + 1);
    jobs.clear();
    for (int index = 1; index <= highest; ++index)
        if (entities[index]) jobs.push_back({
            entities[index] + identityOff,
            &identities[index], sizeof(uintptr_t)});
    ExecuteExactReads(jobs);

    std::vector<uintptr_t> designerNames(highest + 1);
    jobs.clear();
    for (int index = 1; index <= highest; ++index)
        if (identities[index]) jobs.push_back({
            identities[index] + designerNameOff,
            &designerNames[index], sizeof(uintptr_t)});
    ExecuteExactReads(jobs);

    std::vector<std::array<char, 64>> names(highest + 1);
    jobs.clear();
    for (int index = 1; index <= highest; ++index)
        if (designerNames[index]) jobs.push_back({
            designerNames[index], names[index].data(),
            static_cast<uint16_t>(names[index].size() - 1)});
    ExecuteExactReads(jobs);

    struct Candidate {
        int index;
        EntityType type;
        std::string name;
        uintptr_t sceneNode = 0;
        uintptr_t sceneParent = 0;
        Vec3 origin = {};
        uint32_t owner = 0xFFFFFFFFu;
        uint32_t ownerPawn = 0xFFFFFFFFu;
    };
    std::vector<Candidate> candidates;
    for (int index = 1; index <= highest; ++index) {
        if (!entities[index] || names[index][0] == '\0') continue;
        std::string name(names[index].data());
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(tolower(c)); });
        EntityType type = ENT_UNKNOWN;
        const bool projectile = name.find("_projectile") != std::string::npos;
        
        
        if (projectile) continue;
        const bool grenade = projectile ||
            name == "weapon_hegrenade" || name == "weapon_flashbang" ||
            name == "weapon_smokegrenade" || name == "weapon_molotov" ||
            name == "weapon_incgrenade" || name == "weapon_decoy";
        if (name == "weapon_c4") type = ENT_BOMB;
        else if (grenade) type = ENT_GRENADE;
        else if (name.rfind("weapon_", 0) == 0) type = ENT_WEAPON;
        if (type == ENT_UNKNOWN) continue;
        if (type == ENT_WEAPON && !settings.showWeapons) continue;
        if (type == ENT_GRENADE && !settings.showGrenades) continue;
        if (type == ENT_BOMB && !settings.showBomb) continue;
        candidates.push_back({index, type, std::move(name)});
    }

    jobs.clear();
    for (auto& candidate : candidates) {
        const uintptr_t entity = entities[candidate.index];
        jobs.push_back({entity + sceneNodeOff, &candidate.sceneNode,
            sizeof(candidate.sceneNode)});
        jobs.push_back({entity + ownerOff, &candidate.owner,
            sizeof(candidate.owner)});
        if (ownerPawnOff && candidate.type != ENT_GRENADE)
            jobs.push_back({entity + ownerPawnOff, &candidate.ownerPawn,
                sizeof(candidate.ownerPawn)});
    }
    ExecuteExactReads(jobs);

    jobs.clear();
    for (auto& candidate : candidates) {
        if (!candidate.sceneNode) continue;
        if (parentOff)
            jobs.push_back({candidate.sceneNode + parentOff,
                &candidate.sceneParent, sizeof(candidate.sceneParent)});
        jobs.push_back({candidate.sceneNode + originOff,
            &candidate.origin, sizeof(candidate.origin)});
    }
    ExecuteExactReads(jobs);

    output.reserve(candidates.size());
    for (auto& candidate : candidates) {
        const bool projectile =
            candidate.name.find("_projectile") != std::string::npos;
        const auto validOwnerHandle = [highest](uint32_t handle) {
            const uint32_t index = handle & 0x7FFFu;
            return handle != 0 && handle != 0xFFFFFFFFu &&
                index > 0 && index <= static_cast<uint32_t>(highest);
        };
        const bool hasPawnOwner = validOwnerHandle(candidate.ownerPawn);
        const bool hasEntityOwner = validOwnerHandle(candidate.owner);
        const bool hasOwner = hasPawnOwner || hasEntityOwner;
        const uint32_t ownerIndex = hasPawnOwner
            ? (candidate.ownerPawn & 0x7FFFu)
            : (candidate.owner & 0x7FFFu);
        
        
        const bool attachedToSceneParent = candidate.sceneParent != 0;
        if (candidate.type != ENT_BOMB && !projectile &&
            (hasOwner || attachedToSceneParent))
            continue;
        WorldEnt world;
        world.ptr = entities[candidate.index];
        world.owner = hasOwner ? entities[ownerIndex] : 0;
        world.type = candidate.type;
        world.name = FriendlyWorldName(candidate.name);
        if (world.owner && candidate.type == ENT_BOMB) {
            output.push_back(std::move(world));
            continue;
        }
        if (!candidate.sceneNode) continue;
        world.origin = candidate.origin;
        if (!std::isfinite(world.origin.x) ||
            !std::isfinite(world.origin.y) ||
            !std::isfinite(world.origin.z)) continue;
        if (candidate.type != ENT_BOMB) {
            bool overlapsInventory = false;
            for (const auto& player : players) {
                const float dx = world.origin.x - player.origin.x;
                const float dy = world.origin.y - player.origin.y;
                const float dz = world.origin.z - player.origin.z;
                if (dx * dx + dy * dy <= 144.f && dz >= -8.f && dz <= 88.f) {
                    overlapsInventory = true;
                    break;
                }
            }
            if (overlapsInventory) continue;
        }
        output.push_back(std::move(world));
    }
    return g_Client.IsTransportHealthy();
}

static void DoSoftAim(HANDLE hProc, uintptr_t client, const std::vector<PlayerEnt>& players,
    uintptr_t localPawn, int localTeam, const Matrix4x4& vm, int sw, int sh,
    const AimSettings& aimSettings) {
    UNREFERENCED_PARAMETER(hProc);
    UNREFERENCED_PARAMETER(client);

    static float g_SmoothVelX = 0.f, g_SmoothVelY = 0.f;
    static float g_ResidualX = 0.f, g_ResidualY = 0.f;
    static uintptr_t g_LastTargetPawn = 0;
    static bool g_HasLocked = false;
    static bool g_WasKeyDown = false;
    static uint32_t g_DeadConfirmations = 0;
    static std::chrono::steady_clock::time_point g_TargetSeenAt = {};
    static std::chrono::steady_clock::time_point g_TargetLastValidAt = {};
    static std::chrono::steady_clock::time_point g_TargetHiddenAt = {};
    static uint32_t g_TargetSwitchFrames = 0;
    static std::chrono::steady_clock::time_point g_LastDiagnostic = {};
    static std::chrono::steady_clock::time_point g_LastAimUpdate = {};
    static float g_CurrentReactionDelayMs = 0.f;
    static std::mt19937 g_ReactionRng(
        static_cast<unsigned int>(GetTickCount64()));

    const auto resetMotion = [&]() {
        g_SmoothVelX = g_SmoothVelY = 0.f;
        g_ResidualX = g_ResidualY = 0.f;
    };
    const auto releaseTarget = [&](const char* reason) {
        if (g_LastTargetPawn) {
            EspLog(
                "[Aim] unlock target=0x%llX reason=%s",
                g_LastTargetPawn,
                reason);
        }
        g_LastTargetPawn = 0;
        g_HasLocked = false;
        g_DeadConfirmations = 0;
        g_TargetSeenAt = {};
        g_TargetLastValidAt = {};
        g_TargetHiddenAt = {};
        g_TargetSwitchFrames = 0;
        g_CurrentReactionDelayMs = 0.f;
        resetMotion();
    };

    if (!aimSettings.enabled) {
        static bool loggedOff = false;
        if (!loggedOff) { EspLog("[Aim] softaim disabled"); loggedOff = true; }
        releaseTarget("disabled");
        g_WasKeyDown = false;
        return;
    }
    if (!localPawn) {
        const auto now = std::chrono::steady_clock::now();
        if (g_LastDiagnostic.time_since_epoch().count() == 0 ||
            now - g_LastDiagnostic >= std::chrono::seconds(1)) {
            EspLog("[Aim] localPawn unavailable; transportHealthy=%d timeouts=%u",
                g_Client.IsTransportHealthy(),
                g_Client.GetConsecutiveTimeouts());
            g_LastDiagnostic = now;
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool down =
        (GetAsyncKeyState(aimSettings.aimKey) & 0x8000) != 0;
    if (!down) {
        resetMotion();
        if (g_WasKeyDown)
            EspLog(
                "[Aim] key 0x%02X released; target retained=0x%llX",
                aimSettings.aimKey,
                g_LastTargetPawn);
        g_WasKeyDown = false;
    } else {
        if (!g_WasKeyDown)
            EspLog("[Aim] key 0x%02X pressed", aimSettings.aimKey);
        g_WasKeyDown = true;
    }

    const int bestBoneIdx = aimSettings.aimBone;
    const float centerX = sw * 0.5f;
    const float centerY = sh * 0.5f;
    const float fovDegrees = std::clamp(aimSettings.aimFov, 1.f, 45.f);
    const float fovRadius =
        tanf(fovDegrees * 0.5f * (3.14159265f / 180.f)) * centerX;
    float bestScreenDistance = fovRadius;
    Vec2 bestScreen = {};
    const PlayerEnt* bestTarget = nullptr;
    size_t eligibleTargets = 0;

    
    
    if (g_LastTargetPawn) {
        const PlayerEnt* lockedTarget = nullptr;
        for (const auto& p : players) {
            if (p.pawn == g_LastTargetPawn) {
                lockedTarget = &p;
                break;
            }
        }

        if (!lockedTarget) {
            if (g_TargetLastValidAt.time_since_epoch().count() == 0)
                g_TargetLastValidAt = now;
            if (now - g_TargetLastValidAt < std::chrono::milliseconds(500)) {
                resetMotion();
                return;
            }
            releaseTarget("pawn missing");
        } else if (!lockedTarget->alive ||
            (aimSettings.teamCheck && lockedTarget->team == localTeam)) {
            if (++g_DeadConfirmations <
                (uint32_t)std::max(1, aimSettings.targetCooldown)) {
                resetMotion();
                return;
            }
            releaseTarget(
                lockedTarget->alive ? "team changed" : "death confirmed");
        } else {
            g_DeadConfirmations = 0;
            g_TargetLastValidAt = now;
            
            
            
            if (aimSettings.visibleOnly && !lockedTarget->visible) {
                if (g_TargetHiddenAt.time_since_epoch().count() == 0)
                    g_TargetHiddenAt = now;
                if (now - g_TargetHiddenAt >=
                    std::chrono::milliseconds(300)) {
                    releaseTarget("not visible");
                } else {
                    resetMotion();
                    return;
                }
            } else {
                g_TargetHiddenAt = {};
                if ((size_t)bestBoneIdx >= lockedTarget->bones.size()) {
                    resetMotion();
                    return;
                }
                const Vec2 screen =
                    W2S(lockedTarget->bones[bestBoneIdx], vm, sw, sh);
                if (screen.x < 0.f || screen.y < 0.f ||
                    screen.x >= sw || screen.y >= sh) {
                    resetMotion();
                    return;
                }
                const float dx = screen.x - centerX;
                const float dy = screen.y - centerY;
                const float lockedDistance = sqrtf(dx * dx + dy * dy);

                
                
                
                
                if (down) {
                    float bestCandidateDistance = lockedDistance;
                    for (const auto& p : players) {
                        if (!p.alive) continue;
                        if (aimSettings.teamCheck && p.team == localTeam)
                            continue;
                        if (aimSettings.visibleOnly && !p.visible) continue;
                        if (p.pawn == lockedTarget->pawn) continue;
                        if ((size_t)bestBoneIdx >= p.bones.size()) continue;
                        const Vec2 cand =
                            W2S(p.bones[bestBoneIdx], vm, sw, sh);
                        if (cand.x < 0.f || cand.y < 0.f ||
                            cand.x >= sw || cand.y >= sh) continue;
                        const float cdx = cand.x - centerX;
                        const float cdy = cand.y - centerY;
                        const float cd = sqrtf(cdx * cdx + cdy * cdy);
                        if (cd < bestCandidateDistance)
                            bestCandidateDistance = cd;
                    }
                    if (bestCandidateDistance < lockedDistance * 0.5f) {
                        if (++g_TargetSwitchFrames >= 2) {
                            g_TargetSwitchFrames = 0;
                            releaseTarget("better target");
                        }
                    } else {
                        g_TargetSwitchFrames = 0;
                    }
                }

                if (g_LastTargetPawn != 0) {
                    bestScreenDistance = lockedDistance;
                    bestScreen = screen;
                    bestTarget = lockedTarget;
                }
            }
        }
    }

    if (!down) return;

    if (!bestTarget) {
        for (const auto& p : players) {
            if (!p.alive) continue;
            if (aimSettings.teamCheck && p.team == localTeam) continue;
            if (aimSettings.visibleOnly && !p.visible) continue;
            if ((size_t)bestBoneIdx >= p.bones.size()) continue;
            const Vec2 screen = W2S(p.bones[bestBoneIdx], vm, sw, sh);
            if (screen.x < 0.f || screen.y < 0.f ||
                screen.x >= sw || screen.y >= sh) continue;
            ++eligibleTargets;
            const float dx = screen.x - centerX;
            const float dy = screen.y - centerY;
            const float distance = sqrtf(dx * dx + dy * dy);
            if (distance < bestScreenDistance) {
                bestScreenDistance = distance;
                bestScreen = screen;
                bestTarget = &p;
            }
        }
    }

    if (!bestTarget)
    {
        resetMotion();
        if (g_LastDiagnostic.time_since_epoch().count() == 0 ||
            now - g_LastDiagnostic >= std::chrono::seconds(1)) {
            EspLog(
                "[Aim] no screen target: players=%zu eligible=%zu radius=%.1fpx",
                players.size(), eligibleTargets, fovRadius);
            g_LastDiagnostic = now;
        }
        return;
    }

    const bool newTarget = g_LastTargetPawn != bestTarget->pawn;
    if (newTarget)
    {
        g_LastTargetPawn = bestTarget->pawn;
        g_TargetSeenAt = now;
        g_TargetLastValidAt = now;
        g_DeadConfirmations = 0;
        resetMotion();
        g_HasLocked = false;
        g_CurrentReactionDelayMs = std::max(0.f, aimSettings.reactionTime);
        if (aimSettings.visibleOnly) {
            static std::uniform_real_distribution<float> reactionJitter(
                20.f, 55.f);
            g_CurrentReactionDelayMs += reactionJitter(g_ReactionRng);
        }
        EspLog("[Aim] lock target=0x%llX distance=%.1fpx",
            bestTarget->pawn, bestScreenDistance);
    }

    if (!g_HasLocked &&
        now - g_TargetSeenAt <
            std::chrono::duration<float, std::milli>(
                g_CurrentReactionDelayMs))
        return;
    g_HasLocked = true;

    const float pixelDx = bestScreen.x - centerX;
    const float pixelDy = bestScreen.y - centerY;
    const float pixelDistance =
        sqrtf(pixelDx * pixelDx + pixelDy * pixelDy);

    
    
    if (pixelDistance <= 1.25f) {
        resetMotion();
        return;
    }

    
    
    
    const auto aimNow = std::chrono::steady_clock::now();
    float aimDt = 1.f;
    if (g_LastAimUpdate.time_since_epoch().count() != 0) {
        const float aimElapsedMs =
            std::chrono::duration<float, std::milli>(
                aimNow - g_LastAimUpdate).count();
        aimDt = std::clamp(aimElapsedMs / 16.666f, 0.0625f, 4.f);
    }
    g_LastAimUpdate = aimNow;

    const float smoothFactor =
        std::clamp(aimSettings.aimSmooth, 1.f, 50.f);
    float rawDx = pixelDx / smoothFactor * aimDt;
    float rawDy = pixelDy / smoothFactor * aimDt;
    const float nearScale = std::clamp(pixelDistance / 16.f, 0.2f, 1.f);
    rawDx *= nearScale;
    rawDy *= nearScale;

    const float ease = std::clamp(aimSettings.ease, 0.01f, 0.95f);
    const float easeStep = 1.f - powf(1.f - ease, aimDt);

    if (aimSettings.humanize) {
        const float jitter = std::clamp(aimSettings.jitter, 0.f, 8.f);
        if (jitter > 0.f) {
            static uint32_t g_JitterSeed = 0x9E3779B9u;
            g_JitterSeed = g_JitterSeed * 1664525u + 1013904223u;
            rawDx += ((g_JitterSeed >> 16) & 0xFFFFu) / 32767.5f - 1.f;
            g_JitterSeed = g_JitterSeed * 1664525u + 1013904223u;
            rawDy += ((g_JitterSeed >> 16) & 0xFFFFu) / 32767.5f - 1.f;
            rawDx *= jitter;
            rawDy *= jitter;
        }
    }

    if (rawDx * g_SmoothVelX < 0.f) g_SmoothVelX = 0.f;
    if (rawDy * g_SmoothVelY < 0.f) g_SmoothVelY = 0.f;

    g_SmoothVelX += (rawDx - g_SmoothVelX) * easeStep;
    g_SmoothVelY += (rawDy - g_SmoothVelY) * easeStep;

    g_ResidualX += g_SmoothVelX;
    g_ResidualY += g_SmoothVelY;
    int moveX = static_cast<int>(roundf(g_ResidualX));
    int moveY = static_cast<int>(roundf(g_ResidualY));
    if (moveX != 0 || moveY != 0) {
        moveX = std::clamp(moveX, -32, 32);
        moveY = std::clamp(moveY, -32, 32);
        g_ResidualX -= static_cast<float>(moveX);
        g_ResidualY -= static_cast<float>(moveY);
        if (abs(moveX) == 32) g_ResidualX = 0.f;
        if (abs(moveY) == 32) g_ResidualY = 0.f;
        const bool sent = g_Client.SendMouseMove(moveX, moveY);

        if (!sent || g_LastDiagnostic.time_since_epoch().count() == 0 ||
            now - g_LastDiagnostic >= std::chrono::seconds(1)) {
            const LksTransportSnapshot transport =
                g_Client.GetTransportSnapshot();
            EspLog(
                "[Aim] move=(%d,%d) command=%d status=%ld queued=%u dispatched=%u normal=%u rundown=%u irql=%u target=%u:%u",
                moveX,
                moveY,
                sent,
                transport.mouseStatus,
                transport.mouseQueued,
                transport.mouseDispatched,
                transport.mouseApcNormal,
                transport.mouseApcRundown,
                transport.mouseLastIrql,
                transport.mouseTargetProcessId,
                transport.mouseTargetThreadId);
            g_LastDiagnostic = now;
        }
    }
}

static void OverlayLoop(HANDLE hProc) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    OvStatus(L"[*] ESP: getting module base...");
    EspLog("[OverlayLoop] Started, hProc=0x%llX", (uintptr_t)hProc);

    uintptr_t clientBase = GetModuleBase(hProc, L"client.dll");
    if (!clientBase) {
        OvStatus(L"[-] ESP: client.dll not found");
        EspLog("[OverlayLoop] client.dll not found");
        return;
    }
    EspLog("[OverlayLoop] client.dll base = 0x%llX", clientBase);
    InitNtRead();

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    EspLog("[OverlayLoop] screen=%dx%d", sw, sh);

    WNDCLASS wc = {};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM w, LPARAM l) -> LRESULT {
        switch (msg) {
            case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); } return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        } return DefWindowProc(hwnd, msg, w, l);
    };
    wc.hInstance = GetModuleHandle(0);
    wc.lpszClassName = L"LksOverlay";
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClass(&wc);

    HWND ov = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"LksOverlay", L"", WS_POPUP,
        0, 0, sw, sh, 0, 0, wc.hInstance, 0);
    if (!ov) {
        EspLog("[OverlayLoop] CreateWindowEx FAILED, error=%u", GetLastError());
        OvStatus(L"[-] Overlay window creation failed");
        return;
    }
    EspLog("[OverlayLoop] overlay HWND=0x%llX", (uintptr_t)ov);

    SetWindowPos(ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g_hWnd) SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(ov, SW_SHOW);
    EspLog("[OverlayLoop] overlay shown, menu raised");

    HDC hdcScreen = GetDC(0);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmp = CreateCompatibleBitmap(hdcScreen, sw, sh);
    if (!hbmp) {
        EspLog("[OverlayLoop] CreateCompatibleBitmap FAILED, error=%u", GetLastError());
    }
    HGDIOBJ oldBitmap = SelectObject(hdcMem, hbmp);
    HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

    OvStatus(L"[+] ESP overlay running");

    int frame = 0;
    EspSettings initialEspSettings;
    {
        std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
        initialEspSettings = g_EspSettings;
    }
    bool wasEnabled = initialEspSettings.enabled;
    auto nextFrame = std::chrono::steady_clock::now();
    auto statsStart = nextFrame;
    uint32_t statsFrames = 0;
    double statsPresentMs = 0.0;
    double displayedFps = 0.0;
    double displayedReadMs = 0.0;
    double displayedSnapshotHz = 0.0;
    double displayedPresentMs = 0.0;
    
    
    constexpr auto targetFrameTime = std::chrono::microseconds(3333);

    struct OverlaySnapshot {
        Matrix4x4 viewMatrix = {};
        std::vector<PlayerEnt> players;
        uintptr_t localPawn = 0;
        int localTeam = 0;
        BombInfo bomb = {};
        std::vector<WorldEnt> worldEntities;
        uint64_t generation = 0;
    };

    std::atomic<std::shared_ptr<const OverlaySnapshot>> publishedSnapshot;
    std::atomic_bool acquisitionRunning{true};
    std::atomic_bool transportHealthy{true};
    std::atomic<double> latestReadMs{0.0};
    std::atomic<double> latestSnapshotHz{0.0};

    DWORD mouseProcess = 0;
    const DWORD mouseThread =
        GetWindowThreadProcessId(ov, &mouseProcess);

    
    
    std::thread acquisitionThread([&, hProc, clientBase, sw, sh]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        bool transportWasHealthy = true;
        uint64_t generation = 0;
        uint32_t freshSnapshots = 0;
        BombInfo latestBomb = {};
        std::vector<WorldEnt> latestWorldEntities;
        auto rateStart = std::chrono::steady_clock::now();
        auto nextWorldScan = rateStart;
        auto nextMouseThreadRefresh = rateStart;

        while (g_Running.load(std::memory_order_acquire) &&
            acquisitionRunning.load(std::memory_order_acquire)) {
            if (WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0) {
                EspLog(
                    "[OverlayLoop] target process exited, stopping acquisition");
                break;
            }
            const auto cycleStart = std::chrono::steady_clock::now();
            AimSettings aimSettings;
            EspSettings espSettings;
            MiscSettings miscSettings;
            {
                std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
                aimSettings = g_AimSettings;
                espSettings = g_EspSettings;
                miscSettings = g_MiscSettings;
            }

            const auto readStart = std::chrono::steady_clock::now();
            if (readStart >= nextMouseThreadRefresh) {
                const bool targetSent =
                    mouseProcess != 0 && mouseThread != 0 &&
                    g_Client.SendTargetThread(mouseProcess, mouseThread);
                EspLog(
                    "[Aim] GUI target PID=%u TID=%u sent=%d",
                    mouseProcess,
                    mouseThread,
                    targetSent);
                nextMouseThreadRefresh =
                    readStart + std::chrono::seconds(2);
            }

            if (!espSettings.enabled && !aimSettings.enabled) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            auto next = std::make_shared<OverlaySnapshot>();
            const bool freshFrame = ReadPlayers(
                hProc,
                clientBase,
                next->viewMatrix,
                next->players,
                next->localPawn,
                next->localTeam);
            const auto readFinished = std::chrono::steady_clock::now();
            latestReadMs.store(
                std::chrono::duration<double, std::milli>(
                    readFinished - readStart).count(),
                std::memory_order_relaxed);

            const bool healthy = g_Client.IsTransportHealthy();
            transportHealthy.store(healthy, std::memory_order_release);
            if (healthy != transportWasHealthy) {
                const LksTransportSnapshot transport =
                    g_Client.GetTransportSnapshot();
                EspLog(
                    "[Transport] healthy=%d local=%d heartbeat=%u state=%ld lastTxn=%u drops=%u timeouts=%u activeTxn=%u type=%u count=%u addr=0x%llX budgetTimeouts=%u controller=%u:0x%llX mapStatus=0x%08lX",
                    healthy,
                    g_Client.IsInContext(),
                    transport.heartbeat,
                    transport.driverState,
                    transport.lastCompletedTransaction,
                    transport.responseDrops,
                    g_Client.GetConsecutiveTimeouts(),
                    transport.activeTransaction,
                    transport.activeMessageType,
                    transport.activeReadCount,
                    transport.activeAddress,
                    transport.readBudgetTimeouts,
                    transport.controllerProcessId,
                    transport.controllerAddress,
                    transport.controllerMappingStatus);
                OvStatus(healthy ?
                    L"[+] Kernel transport recovered" :
                    L"[-] Kernel transport stalled; retrying");
                transportWasHealthy = healthy;
            }

            if (freshFrame && healthy) {
                if (aimSettings.enabled) {
                    DoSoftAim(
                        hProc,
                        clientBase,
                        next->players,
                        next->localPawn,
                        next->localTeam,
                        next->viewMatrix,
                        sw,
                        sh,
                        aimSettings);
                }
                if (miscSettings.showDamageLog && (generation & 3u) == 0)
                    ReadDamageLog(hProc, clientBase, next->localPawn);
                if (miscSettings.showBombTimer || espSettings.showBomb)
                    ReadBombInfo(hProc, clientBase, latestBomb);
                if (readFinished >= nextWorldScan) {
                    ReadWorldEntities(
                        clientBase,
                        espSettings,
                        next->players,
                        latestWorldEntities);
                    nextWorldScan = readFinished +
                        std::chrono::milliseconds(750);
                }

                next->bomb = latestBomb;
                next->worldEntities = latestWorldEntities;
                next->generation = ++generation;
                std::shared_ptr<const OverlaySnapshot> immutable = next;
                publishedSnapshot.store(
                    std::move(immutable), std::memory_order_release);
                ++freshSnapshots;
            }

            const auto rateNow = std::chrono::steady_clock::now();
            const auto rateElapsed = rateNow - rateStart;
            if (rateElapsed >= std::chrono::seconds(1)) {
                const double seconds =
                    std::chrono::duration<double>(rateElapsed).count();
                latestSnapshotHz.store(
                    freshSnapshots / seconds,
                    std::memory_order_relaxed);
                rateStart = rateNow;
                freshSnapshots = 0;
            }
            
            
            
            const auto cycleElapsed =
                std::chrono::steady_clock::now() - cycleStart;
            constexpr auto minAcquisitionCycle =
                std::chrono::microseconds(2000);
            if (cycleElapsed < minAcquisitionCycle)
                std::this_thread::sleep_for(
                    minAcquisitionCycle - cycleElapsed);
        }
    });

    while (g_Running.load(std::memory_order_acquire) &&
        acquisitionRunning.load(std::memory_order_acquire))
    {
        MSG overlayMessage = {};
        while (PeekMessage(&overlayMessage, nullptr, 0, 0, PM_REMOVE)) {
            if (overlayMessage.message == WM_QUIT) {
                g_Running.store(false, std::memory_order_release);
                break;
            }
            TranslateMessage(&overlayMessage);
            DispatchMessage(&overlayMessage);
        }
        if (!g_Running.load(std::memory_order_acquire)) break;

        frame++;

        EspSettings espSettings;
        AimSettings aimSettings;
        MiscSettings miscSettings;
        {
            std::lock_guard<std::recursive_mutex> lock(g_SettingsMutex);
            espSettings = g_EspSettings;
            aimSettings = g_AimSettings;
            miscSettings = g_MiscSettings;
        }

        const bool espOn = espSettings.enabled;
        if (espOn != wasEnabled)
        {
            EspLog("[OverlayLoop] ESP toggle: %d -> %d", wasEnabled, espOn);
            wasEnabled = espOn;
        }

        if (!espOn)
        {
            if (IsWindowVisible(ov))
            {
                ShowWindow(ov, SW_HIDE);
                EspLog("[OverlayLoop] overlay hidden (ESP disabled)");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (!IsWindowVisible(ov))
        {
            ShowWindow(ov, SW_SHOW);
            EspLog("[OverlayLoop] overlay shown (ESP enabled)");
        }

        RECT crect = {0, 0, sw, sh};
        FillRect(hdcMem, &crect, (HBRUSH)GetStockObject(BLACK_BRUSH));

        const auto snapshot =
            publishedSnapshot.load(std::memory_order_acquire);
        const Matrix4x4 emptyViewMatrix = {};
        const std::vector<PlayerEnt> emptyPlayers;
        const Matrix4x4& vm = snapshot ?
            snapshot->viewMatrix : emptyViewMatrix;
        const std::vector<PlayerEnt>& players = snapshot ?
            snapshot->players : emptyPlayers;
        const int localTeam = snapshot ? snapshot->localTeam : 0;

        uintptr_t bombCarrier = 0;
        if (snapshot && espSettings.showBomb) {
            for (const auto& world : snapshot->worldEntities) {
                if (world.type == ENT_BOMB && world.owner) {
                    bombCarrier = world.owner;
                    break;
                }
            }
            if (bombCarrier) {
                for (const auto& carrier : players) {
                    if (!carrier.alive || carrier.pawn != bombCarrier) continue;
                    Vec3 tagPosition = carrier.origin;
                    tagPosition.z += 82.f;
                    const Vec2 tag = W2S(tagPosition, vm, sw, sh);
                    if (tag.x >= 0.f && tag.y >= 0.f &&
                        tag.x < sw && tag.y < sh)
                        DrawWorldLabel(
                            hdcMem, tag, espSettings.bombColor, "C4 CARRIER");
                    break;
                }
            }
        }

        int rendered = 0;
        for (auto& p : players)
        {
            if (!p.alive) continue;
            if (localTeam && p.team == localTeam) continue;
            if (espSettings.visibleOnly && !p.visible) continue;

            Vec2 head, foot;
            if (!p.bones.empty())
            {
                head = W2S(p.bones[BONE_HEAD], vm, sw, sh);
                foot = W2S(p.origin, vm, sw, sh);
            }
            else
            {
                Vec3 hp = p.origin; hp.z += 72.f;
                head = W2S(hp, vm, sw, sh);
                foot = W2S(p.origin, vm, sw, sh);
            }

            if (head.x < 0 || foot.x < 0) continue;

            float height = foot.y - head.y;
            int bx = (int)(foot.x - (height * 0.3f));
            int by = (int)head.y;
            int bw = (int)(height * 0.6f);
            int bh = (int)height;
            if (bw < 4) bw = 4;

            if (espSettings.box) {
                if (espSettings.boxStyle == BOX_FULL_3D ||
                    espSettings.boxStyle == BOX_CORNER_3D) {
                    float topZ = p.origin.z + 72.f;
                    if (p.bones.size() > BONE_HEAD)
                        topZ = p.bones[BONE_HEAD].z + 5.f;
                    Draw3DBox(
                        hdcMem,
                        p.origin,
                        topZ,
                        vm,
                        sw,
                        sh,
                        espSettings.boxColor,
                        espSettings.boxStyle == BOX_CORNER_3D);
                } else {
                    DrawBox(
                        hdcMem,
                        bx,
                        by,
                        bw,
                        bh,
                        espSettings.boxColor,
                        espSettings.boxStyle);
                }
            }

            const COLORREF bodyColor = p.visible ?
                espSettings.skeletonVisibleColor :
                espSettings.skeletonHiddenColor;

            if (espSettings.skeleton && !p.bones.empty())
                DrawSkeleton(hdcMem, p.bones, vm, sw, sh, bodyColor);

            if (espSettings.headEsp && !p.bones.empty())
                DrawHeadEsp(hdcMem, p.bones, vm, sw, sh, bodyColor);

            if (espSettings.health)
            {
                int hbX = bx - 8;
                DrawHealthBar(hdcMem, hbX, by, bh, p.health, p.maxHealth);
                DrawArmorBar(hdcMem, hbX - 5, by, bh, p.armor);
            }

            if (espSettings.name && !p.name.empty())
            {
                std::wstring wn(p.name.begin(), p.name.end());
                DrawText(hdcMem, bx, by - 16, RGB(255, 255, 255), wn.c_str());
            }

            rendered++;
        }

        if (frame % 120 == 0)
        {
            EspLog("[OverlayLoop] frame #%d: %zu players in list, %d rendered", frame, players.size(), rendered);
        }

        if (snapshot) {
            for (const auto& world : snapshot->worldEntities) {
                if (world.owner) continue;
                if (world.type == ENT_WEAPON && !espSettings.showWeapons)
                    continue;
                if (world.type == ENT_GRENADE && !espSettings.showGrenades)
                    continue;
                if (world.type == ENT_BOMB && !espSettings.showBomb)
                    continue;
                const Vec2 point = W2S(world.origin, vm, sw, sh);
                if (point.x < 0.f || point.y < 0.f ||
                    point.x >= sw || point.y >= sh) continue;
                COLORREF color = espSettings.weaponColor;
                if (world.type == ENT_GRENADE)
                    color = espSettings.grenadeColor;
                else if (world.type == ENT_BOMB)
                    color = espSettings.bombColor;
                DrawWorldLabel(hdcMem, point, color, world.name);
            }
            if (espSettings.showBomb && snapshot->bomb.valid &&
                !snapshot->bomb.defused) {
                const Vec2 point = W2S(snapshot->bomb.origin, vm, sw, sh);
                if (point.x >= 0.f && point.y >= 0.f &&
                    point.x < sw && point.y < sh)
                    DrawWorldLabel(
                        hdcMem, point, espSettings.bombColor, "Planted C4");
            }
        }

        if (espSettings.showFovCircle)
        {
            const float halfWidth = sw * 0.5f;
            const float fovDegrees =
                std::clamp(aimSettings.aimFov, 1.f, 45.f);
            const int radius = static_cast<int>(
                tanf(fovDegrees * 0.5f * (3.14159265f / 180.f)) *
                halfWidth);
            HPEN fovPen = CreatePen(
                PS_SOLID, 1, espSettings.fovCircleColor);
            HGDIOBJ oldPen = SelectObject(hdcMem, fovPen);
            HGDIOBJ oldBrush = SelectObject(
                hdcMem, GetStockObject(NULL_BRUSH));
            Ellipse(
                hdcMem,
                sw / 2 - radius,
                sh / 2 - radius,
                sw / 2 + radius,
                sh / 2 + radius);
            SelectObject(hdcMem, oldBrush);
            SelectObject(hdcMem, oldPen);
            DeleteObject(fovPen);
        }

        if (miscSettings.showCrosshair)
        {
            DrawCrosshair(hdcMem, sw, sh);
        }

        if (miscSettings.showGameInfo)
        {
            const int infoX = 20;
            const int infoY = std::clamp(
                static_cast<int>(sh * 0.30f), 270, 410);
            wchar_t info[128] = {};
            swprintf_s(info, L"FPS: %.1f", displayedFps);
            DrawText(hdcMem, infoX, infoY, RGB(90, 230, 255), info);
            swprintf_s(
                info,
                L"Players: %d / %zu",
                rendered,
                players.size());
            DrawText(hdcMem, infoX, infoY + 18, RGB(225, 225, 230), info);
            swprintf_s(
                info,
                L"Kernel: %.2f ms @ %.1f Hz | Present: %.2f ms",
                displayedReadMs,
                displayedSnapshotHz,
                displayedPresentMs);
            DrawText(hdcMem, infoX, infoY + 36, RGB(170, 170, 180), info);
        }

        if (miscSettings.showBombTimer)
        {
            const BombInfo emptyBomb = {};
            DrawBombTimer(
                hdcMem,
                sw,
                sh,
                snapshot ? snapshot->bomb : emptyBomb);
        }

        if (miscSettings.showDamageLog)
        {
            std::vector<DamageEvent> damageLog;
            {
                std::lock_guard<std::mutex> lock(g_DamageLogMutex);
                damageLog = g_DamageLog;
            }
            float now = (float)GetTickCount64() / 1000.f;
            int dmgY = 60;
            for (int i = (int)damageLog.size() - 1; i >= 0; i--) {
                const DamageEvent& ev = damageLog[i];
                if (now - ev.time > 5.f) continue;
                wchar_t buf[64];
                swprintf_s(buf, L"-%d HP", ev.dmg);
                DrawText(hdcMem, sw - 80, dmgY, RGB(255, 80, 80), buf);
                dmgY += 18;
            }
        }

        POINT pt = {0, 0};
        SIZE sz = {sw, sh};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, 0};
        const auto presentStart = std::chrono::steady_clock::now();
        UpdateLayeredWindow(ov, hdcScreen, &pt, &sz, hdcMem, &pt, RGB(0,0,0), &bf, ULW_COLORKEY);
        statsPresentMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - presentStart).count();

        ++statsFrames;
        auto now = std::chrono::steady_clock::now();
        const auto statsElapsed = now - statsStart;
        if (statsElapsed >= std::chrono::seconds(1)) {
            const double seconds =
                std::chrono::duration<double>(statsElapsed).count();
            const double fps = statsFrames / seconds;
            displayedFps = fps;
            displayedReadMs =
                latestReadMs.load(std::memory_order_relaxed);
            displayedSnapshotHz =
                latestSnapshotHz.load(std::memory_order_relaxed);
            displayedPresentMs = statsPresentMs / statsFrames;
            EspLog(
                "[OverlayLoop] %.1f FPS, %.2f ms/frame, kernel %.2f ms @ %.1f snapshots/s, present %.2f ms",
                fps,
                1000.0 / fps,
                displayedReadMs,
                displayedSnapshotHz,
                displayedPresentMs);
            statsStart = now;
            statsFrames = 0;
            statsPresentMs = 0.0;
        }

        nextFrame += targetFrameTime;
        now = std::chrono::steady_clock::now();
        if (now >= nextFrame) {
            nextFrame = now;
        } else {
            while (nextFrame - now > std::chrono::microseconds(250)) {
                SwitchToThread();
                now = std::chrono::steady_clock::now();
            }
            while ((now = std::chrono::steady_clock::now()) < nextFrame)
                YieldProcessor();
        }
    }

    acquisitionRunning.store(false, std::memory_order_release);
    if (acquisitionThread.joinable()) acquisitionThread.join();

    SelectObject(hdcMem, oldFont);
    SelectObject(hdcMem, oldBitmap);
    DeleteObject(hFont);
    DeleteObject(hbmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    DestroyWindow(ov);
    OvStatus(L"[ESP] overlay stopped");
    EspLog("[OverlayLoop] thread exit");
}

static void ReadBombInfo(HANDLE hProc, uintptr_t client, BombInfo& out) {
    if (!hProc || !client) return;

    static std::chrono::steady_clock::time_point nextProbe = {};
    static uint32_t probeFailures = 0;
    static uintptr_t trackedBomb = 0;
    static float trackedTimerLength = 0.f;
    static std::chrono::steady_clock::time_point trackedAt = {};
    static bool wasBeingDefused = false;
    static float trackedDefuseLength = 0.f;
    static std::chrono::steady_clock::time_point defuseStartedAt = {};
    const auto now = std::chrono::steady_clock::now();
    if (now < nextProbe) return;
    nextProbe = now + std::chrono::milliseconds(500);

    uintptr_t plantedOff =
        FindStaticOff("client.dll", "dwPlantedC4");
    if (!plantedOff) return;

    const uint64_t plantedAddress = client + plantedOff;
    const uint16_t plantedSize = sizeof(uintptr_t);
    const auto plantedResult = g_Client.KernelReadBatch(
        &plantedAddress, &plantedSize, 1);
    if (plantedResult.size() != 1 ||
        plantedResult[0].size() != sizeof(uintptr_t))
    {
        ++probeFailures;
        nextProbe = now + std::chrono::seconds(30);
        EspLog(
            "[Bomb] quarantined dwPlantedC4 addr=0x%llX for 30s after failure #%u",
            plantedAddress,
            probeFailures);
        return;
    }
    uintptr_t planted = 0;
    memcpy(&planted, plantedResult[0].data(), sizeof(planted));
    if (!planted) {
        out = {};
        trackedBomb = 0;
        trackedTimerLength = 0.f;
        trackedAt = {};
        wasBeingDefused = false;
        trackedDefuseLength = 0.f;
        defuseStartedAt = {};
        return;
    }

    uintptr_t timerOff = FindFieldOff(
        "client.dll", "C_PlantedC4", "m_flC4Blow");
    uintptr_t defusedOff = FindFieldOff(
        "client.dll", "C_PlantedC4", "m_bBombDefused");
    uintptr_t timerLengthOff = FindFieldOff(
        "client.dll", "C_PlantedC4", "m_flTimerLength");
    uintptr_t beingDefusedOff = FindFieldOff(
        "client.dll", "C_PlantedC4", "m_bBeingDefused");
    uintptr_t defuseLengthOff = FindFieldOff(
        "client.dll", "C_PlantedC4", "m_flDefuseLength");
    uintptr_t defuserOff = FindFieldOff(
        "client.dll", "C_PlantedC4", "m_hBombDefuser");
    uintptr_t siteOff = FindFieldOff(
        "client.dll", "C_PlantedC4", "m_nBombSite");
    uintptr_t sceneNodeOff = FindFieldOff(
        "client.dll", "C_BaseEntity", "m_pGameSceneNode");
    uintptr_t absOriginOff = FindFieldOff(
        "client.dll", "CGameSceneNode", "m_vecAbsOrigin");

    BombInfo next = {};
    next.planted = true;
    float blowTime = 0.f;
    float timerLength = 0.f;
    float defuseLength = 0.f;
    uint32_t defuserHandle = 0;
    uintptr_t sceneNode = 0;
    std::vector<ExactReadJob> jobs;
    jobs.reserve(4);
    if (defusedOff)
        jobs.push_back({planted + defusedOff, &next.defused, sizeof(next.defused)});
    if (timerOff)
        jobs.push_back({planted + timerOff, &blowTime, sizeof(blowTime)});
    if (timerLengthOff)
        jobs.push_back({
            planted + timerLengthOff,
            &timerLength,
            sizeof(timerLength)});
    if (beingDefusedOff)
        jobs.push_back({
            planted + beingDefusedOff,
            &next.beingDefused,
            sizeof(next.beingDefused)});
    if (defuseLengthOff)
        jobs.push_back({
            planted + defuseLengthOff,
            &defuseLength,
            sizeof(defuseLength)});
    if (defuserOff)
        jobs.push_back({
            planted + defuserOff,
            &defuserHandle,
            sizeof(defuserHandle)});
    if (siteOff)
        jobs.push_back({planted + siteOff, &next.site, sizeof(next.site)});
    if (sceneNodeOff)
        jobs.push_back({planted + sceneNodeOff, &sceneNode, sizeof(sceneNode)});
    ExecuteExactReads(jobs);

    
    
    
    if (trackedBomb != planted || trackedAt.time_since_epoch().count() == 0) {
        trackedBomb = planted;
        trackedTimerLength =
            timerLength > 1.f && timerLength < 120.f ? timerLength : 40.f;
        trackedAt = now;
    }
    const float elapsed = std::chrono::duration<float>(now - trackedAt).count();
    next.timer = trackedTimerLength - elapsed;
    if (next.timer < 0.f) next.timer = 0.f;
    next.timerLength = trackedTimerLength;
    next.defuserEnt = static_cast<int>(defuserHandle & 0x7FFFu);
    if (next.beingDefused) {
        if (!wasBeingDefused || defuseStartedAt.time_since_epoch().count() == 0) {
            trackedDefuseLength =
                defuseLength > 1.f && defuseLength < 15.f ? defuseLength : 10.f;
            defuseStartedAt = now;
        }
        const float defuseElapsed =
            std::chrono::duration<float>(now - defuseStartedAt).count();
        next.defuseLength = trackedDefuseLength;
        next.defuseRemaining =
            std::max(0.f, trackedDefuseLength - defuseElapsed);
        next.defuseProgress = std::clamp(
            defuseElapsed / trackedDefuseLength, 0.f, 1.f);
    } else {
        trackedDefuseLength = 0.f;
        defuseStartedAt = {};
    }
    wasBeingDefused = next.beingDefused;
    if (sceneNode && absOriginOff) {
        ExecuteExactReads({{
            sceneNode + absOriginOff,
            &next.origin,
            sizeof(next.origin)}});
    }
    next.valid = true;
    next.sampledAtMs = GetTickCount64();
    out = next;
}

static void DrawBombTimer(HDC dc, int sw, int sh, const BombInfo& bomb) {
    if (!bomb.valid) return;
    if (bomb.defused) {
        DrawText(dc, sw - 120, 20, RGB(0, 200, 100), L"C4: DEFUSED");
        return;
    }
    const float sinceSample = bomb.sampledAtMs ?
        static_cast<float>(GetTickCount64() - bomb.sampledAtMs) / 1000.f : 0.f;
    const float bombRemaining = std::max(0.f, bomb.timer - sinceSample);
    const float bombRatio = std::clamp(
        bombRemaining / std::max(1.f, bomb.timerLength), 0.f, 1.f);
    wchar_t buf[96];
    if (bomb.site >= 0) {
        swprintf_s(buf, L"C4  %.1fs   SITE %c", bombRemaining, bomb.site == 0 ? 'A' : 'B');
    } else {
        swprintf_s(buf, L"C4  %.1fs", bombRemaining);
    }
    const COLORREF bombColor = bombRemaining < 5.f ? RGB(255, 65, 65) :
        bombRemaining < 10.f ? RGB(255, 190, 55) : RGB(80, 190, 255);
    constexpr int barWidth = 360;
    constexpr int barHeight = 10;
    const int x = (sw - barWidth) / 2;
    
    const int y = std::clamp(
        static_cast<int>(sh * 0.13f), 115, 175);
    DrawText(dc, x, y - 20, bombColor, buf);
    HBRUSH track = CreateSolidBrush(RGB(24, 28, 35));
    RECT trackRect = {x, y, x + barWidth, y + barHeight};
    FillRect(dc, &trackRect, track);
    DeleteObject(track);
    HBRUSH bombBrush = CreateSolidBrush(bombColor);
    RECT bombRect = {
        x, y, x + static_cast<int>(barWidth * bombRatio), y + barHeight};
    FillRect(dc, &bombRect, bombBrush);
    DeleteObject(bombBrush);

    if (bomb.beingDefused && bomb.defuseLength > 0.f) {
        const float defuseRemaining =
            std::max(0.f, bomb.defuseRemaining - sinceSample);
        const float defuseProgress = std::clamp(
            1.f - defuseRemaining / bomb.defuseLength, 0.f, 1.f);
        const bool hasTime = defuseRemaining <= bombRemaining;
        const COLORREF defuseColor = hasTime ?
            RGB(65, 225, 125) : RGB(255, 75, 75);
        const bool hasKit = bomb.defuseLength <= 5.5f;
        
        
        HBRUSH defuseBrush = CreateSolidBrush(defuseColor);
        RECT defuseRect = {
            x, y,
            x + static_cast<int>(barWidth * defuseProgress),
            y + barHeight};
        FillRect(dc, &defuseRect, defuseBrush);
        DeleteObject(defuseBrush);
        HPEN marker = CreatePen(PS_SOLID, 1, RGB(235, 235, 240));
        HPEN oldMarker = static_cast<HPEN>(SelectObject(dc, marker));
        const int bombEdge = x + static_cast<int>(barWidth * bombRatio);
        MoveToEx(dc, bombEdge, y - 2, nullptr);
        LineTo(dc, bombEdge, y + barHeight + 3);
        SelectObject(dc, oldMarker);
        DeleteObject(marker);
        swprintf_s(
            buf,
            L"DEFUSE  %.1fs   %s   %s",
            defuseRemaining,
            hasKit ? L"KIT" : L"NO KIT",
            hasTime ? L"SAFE" : L"TOO LATE");
        DrawText(dc, x, y + 14, defuseColor, buf);
    }
}

static void DrawCrosshair(HDC dc, int sw, int sh) {
    int cx = sw / 2, cy = sh / 2;
    COLORREF col = RGB(255, 50, 50);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HPEN old = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, cx - 8, cy, 0); LineTo(dc, cx + 8, cy);
    MoveToEx(dc, cx, cy - 8, 0); LineTo(dc, cx, cy + 8);
    MoveToEx(dc, cx - 2, cy - 2, 0); LineTo(dc, cx + 2, cy + 2);
    MoveToEx(dc, cx - 2, cy + 2, 0); LineTo(dc, cx + 2, cy - 2);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void ReadDamageLog(HANDLE hProc, uintptr_t client, uintptr_t localPawn) {
    UNREFERENCED_PARAMETER(client);
    static uintptr_t previousPawn = 0;
    static int previousHealth = 0;
    if (!localPawn) {
        previousPawn = 0;
        previousHealth = 0;
        return;
    }

    const uintptr_t healthOff = FindFieldOff(
        "client.dll", "C_BaseEntity", "m_iHealth");
    if (!healthOff) return;
    const int currentHealth = Read<int>(hProc, localPawn + healthOff);
    if (localPawn != previousPawn || currentHealth <= 0 || currentHealth > 100) {
        previousPawn = localPawn;
        previousHealth = currentHealth;
        return;
    }

    if (previousHealth > currentHealth) {
        DamageEvent ev;
        ev.time = (float)GetTickCount64() / 1000.f;
        ev.dmg = previousHealth - currentHealth;
        ev.from = {0,0,0};
        std::lock_guard<std::mutex> lock(g_DamageLogMutex);
        g_DamageLog.push_back(ev);
        if (g_DamageLog.size() > 20)
            g_DamageLog.erase(g_DamageLog.begin());
    }
    previousPawn = localPawn;
    previousHealth = currentHealth;
}

void StartOverlay(HANDLE hProcess) {
    if (!g_Running.load(std::memory_order_acquire) ||
        g_OverlayThread.joinable()) {
        return;
    }
    g_OverlayThread =
        std::thread([hProcess]() { OverlayLoop(hProcess); });
}

void StopOverlay() {
    if (g_OverlayThread.joinable()) g_OverlayThread.join();
}
