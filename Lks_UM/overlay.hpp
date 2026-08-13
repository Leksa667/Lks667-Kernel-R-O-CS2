// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <future>
#include "LksClient.hpp"

#pragma pack(push, 1)
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };
#pragma pack(pop)

extern LksClient g_Client;
extern std::atomic_bool g_UseKernelRead;

typedef LONG(__stdcall* pNtReadVM)(HANDLE, void*, void*, SIZE_T, SIZE_T*);
typedef LONG(__stdcall* pNtWriteVM)(HANDLE, void*, void*, SIZE_T, SIZE_T*);
extern pNtReadVM g_NtReadVM;
extern pNtWriteVM g_NtWriteVM;
extern HWND g_hWnd;

void EspLog(const char* fmt, ...);

template<typename T> T Read(HANDLE h, uintptr_t addr) {
    T val = {};
    if (g_UseKernelRead.load(std::memory_order_acquire) &&
        h == g_Client.GetProcessHandle()) {
        uint64_t a = addr;
        uint16_t s = sizeof(T);
        auto r = g_Client.KernelReadBatch(&a, &s, 1);
        if (!r.empty() && r[0].size() == sizeof(T))
            memcpy(&val, r[0].data(), sizeof(T));
        else EspLog("[Read] KernelReadBatch FAILED for addr=0x%llX size=%u", addr, sizeof(T));
        return val;
    }
    EspLog("[Read] Called Read() without kernel context!");
    return val;
}

template<typename T> void ReadBuf(HANDLE h, uintptr_t addr, T* buf, size_t sz) {
    if (g_UseKernelRead.load(std::memory_order_acquire) &&
        h == g_Client.GetProcessHandle()) {
        uint64_t a = addr;
        uint16_t s = (uint16_t)sz;
        auto r = g_Client.KernelReadBatch(&a, &s, 1);
        if (!r.empty() && !r[0].empty() && r[0].size() == sz) memcpy(buf, r[0].data(), sz);
        else EspLog("[ReadBuf] KernelReadBatch FAILED for addr=0x%llX size=%zu", addr, sz);
        return;
    }
    EspLog("[ReadBuf] Called ReadBuf() without kernel context!");
}

struct OffsetEntry {
    std::string mod;
    std::string className;
    std::string name;
    uint32_t value;
};

enum BoneID : int {
    BONE_PELVIS = 1, BONE_SPINE0 = 2, BONE_SPINE1 = 3, BONE_SPINE2 = 4,
    BONE_NECK = 6, BONE_HEAD = 7,
    BONE_L_SHOULDER = 9, BONE_L_ELBOW = 10, BONE_L_HAND = 11,
    BONE_R_SHOULDER = 13, BONE_R_ELBOW = 14, BONE_R_HAND = 15,
    BONE_L_HIP = 17, BONE_L_KNEE = 18, BONE_L_FOOT = 19,
    BONE_R_HIP = 20, BONE_R_KNEE = 21, BONE_R_FOOT = 22,
    BONE_CHEST = 23
};

struct BoneConn { BoneID b1, b2; };
inline const std::vector<BoneConn> g_BoneConnections = {
    {BONE_HEAD, BONE_NECK},
    {BONE_NECK, BONE_CHEST}, {BONE_CHEST, BONE_PELVIS},
    {BONE_CHEST, BONE_L_SHOULDER}, {BONE_L_SHOULDER, BONE_L_ELBOW}, {BONE_L_ELBOW, BONE_L_HAND},
    {BONE_CHEST, BONE_R_SHOULDER}, {BONE_R_SHOULDER, BONE_R_ELBOW}, {BONE_R_ELBOW, BONE_R_HAND},
    {BONE_PELVIS, BONE_L_HIP}, {BONE_L_HIP, BONE_L_KNEE}, {BONE_L_KNEE, BONE_L_FOOT},
    {BONE_PELVIS, BONE_R_HIP}, {BONE_R_HIP, BONE_R_KNEE}, {BONE_R_KNEE, BONE_R_FOOT}
};

enum EntityType : int {
    ENT_UNKNOWN = 0,
    ENT_WEAPON = 1,
    ENT_GRENADE = 2,
    ENT_BOMB = 3,
    ENT_HOSTAGE = 4,
    ENT_CHICKEN = 5,
    ENT_DEFUSEKIT = 6,
    ENT_ITEM = 7
};

struct WorldEnt {
    uintptr_t ptr = 0;
    uintptr_t owner = 0;
    EntityType type = ENT_UNKNOWN;
    Vec3 origin = {};
    std::string name;
    int weaponId = 0;
    int ammo = 0;
    int maxAmmo = 0;
    float timer = 0.f;
    bool alive = false;
    bool visible = false;
};

struct BombInfo {
    bool planted = false;
    bool carried = false;
    bool dropped = false;
    int carrierTeam = 0;
    bool defused = false;
    int site = -1;
    float timer = 0.f;
    float timerLength = 40.f;
    float defuseProgress = 0.f;
    float defuseRemaining = 0.f;
    float defuseLength = 0.f;
    int defuserEnt = 0;
    bool beingDefused = false;
    uint64_t sampledAtMs = 0;
    Vec3 origin = {};
    bool valid = false;
};

struct GameRulesInfo {
    bool valid = false;
    bool bombPlanted = false;
    float bombTimer = 0.f;
    float roundTime = 0.f;
    float freezeTime = 0.f;
    int phase = 0;
    int scoreCT = 0;
    int scoreT = 0;
    int playersAliveCT = 0;
    int playersAliveT = 0;
};

struct FadeDot {
    Vec3 pos;
    float time;
    float duration;
};

struct DamageEvent {
    float time;
    int dmg;
    Vec3 from;
};

enum BoxStyle : int {
    BOX_CORNER_2D = 0,
    BOX_FULL_2D = 1,
    BOX_FULL_3D = 2,
    BOX_CORNER_3D = 3,
    BOX_ROUNDED_2D = 4,
    BOX_FILLED_2D = 5,
    BOX_CIRCLE_2D = 6,
    BOX_STYLE_COUNT = 7
};

struct EspSettings {
    bool enabled = true;
    bool box = true;
    bool skeleton = true;
    bool headEsp = true;
    bool health = true;
    bool shield = false;
    bool name = true;
    bool visibleOnly = false;
    bool showBomb = true;
    bool showWeapons = true;
    bool showGrenades = true;
    bool showHostages = false;
    bool showChickens = false;
    bool showDefuseKit = true;
    bool showTimer = true;
    bool showGameInfo = true;
    bool showFlashStatus = false;
    bool showAimTrace = false;
    bool showFovCircle = false;
    bool showBombBlast = true;
    bool showFootsteps = false;
    COLORREF boxColor = RGB(0, 200, 255);
    COLORREF boxColorVisible = RGB(0, 255, 100);
    COLORREF skeletonColor = RGB(255, 200, 0);
    COLORREF skeletonVisibleColor = RGB(60, 230, 110);
    COLORREF skeletonHiddenColor = RGB(235, 70, 70);
    COLORREF bombColor = RGB(255, 50, 50);
    COLORREF weaponColor = RGB(200, 200, 200);
    COLORREF grenadeColor = RGB(255, 150, 50);
    COLORREF defuseColor = RGB(0, 200, 100);
    COLORREF flashColor = RGB(255, 255, 0);
    COLORREF fovCircleColor = RGB(110, 190, 255);
    int boxStyle = 0;
};

enum AimSmoothMode : int {
    AIM_SMOOTH_EASE_OUT = 0,
    AIM_SMOOTH_SPRING_DAMPER = 1,
    AIM_SMOOTH_ONE_EURO = 2,
    AIM_SMOOTH_COMBO = 3
};

struct AimSettings {
    bool enabled = false;
    int aimKey = 0x06;
    int aimBone = 7;
    int aimMode = AIM_SMOOTH_COMBO;
    float aimFov = 8.f;
    float aimSmooth = 5.f;
    bool visibleOnly = false;
    bool teamCheck = true;
    bool humanize = true;
    float reactionTime = 35.f;
    float jitter = 1.2f;
    float ease = 0.25f;
    int targetCooldown = 6;
};

struct MiscSettings {
    bool showCrosshair = false;
    bool showGameInfo = false;
    bool showBombTimer = true;
    bool showDamageLog = false;
};

struct ConfigSettings {
    char lastPath[260] = "lks_config.cfg";
};

struct PlayerEnt {
    uintptr_t pawn = 0, controller = 0;
    Vec3 origin;
    int health = 0, maxHealth = 100, team = 0, armor = 0;
    bool alive = false, visible = false;
    bool flashed = false;
    float flashDuration = 0.f;
    std::string name;
    std::vector<Vec3> bones;
};

inline Vec2 W2S(const Vec3& pos, const Matrix4x4& vm, int sw, int sh) {
    float w = vm.m[3][0] * pos.x + vm.m[3][1] * pos.y + vm.m[3][2] * pos.z + vm.m[3][3];
    if (w < 0.001f) return {-1, -1};
    float inv = 1.f / w;
    float x = (vm.m[0][0] * pos.x + vm.m[0][1] * pos.y + vm.m[0][2] * pos.z + vm.m[0][3]) * inv;
    float y = (vm.m[1][0] * pos.x + vm.m[1][1] * pos.y + vm.m[1][2] * pos.z + vm.m[1][3]) * inv;
    return {(sw / 2.f) * (1.f + x), (sh / 2.f) * (1.f - y)};
}

template<typename T> bool Write(HANDLE h, uintptr_t addr, const T& val) {
    SIZE_T written = 0;
    if (g_NtWriteVM)
        return g_NtWriteVM(h, (void*)addr, (void*)&val, sizeof(T), &written) == 0;
    return WriteProcessMemory(h, (LPVOID)addr, &val, sizeof(T), nullptr) != 0;
}


extern std::vector<OffsetEntry> g_Offsets;
extern EspSettings g_EspSettings;
extern AimSettings g_AimSettings;
extern MiscSettings g_MiscSettings;
extern ConfigSettings g_ConfigSettings;
extern std::recursive_mutex g_SettingsMutex;
extern std::atomic_bool g_Running;
extern HWND g_hStatus;
extern std::vector<DamageEvent> g_DamageLog;

void StartOverlay(HANDLE hProcess);
void StopOverlay();
void ReadBombInfo(HANDLE hProc, uintptr_t client,
    const std::vector<PlayerEnt>& players,
    const std::vector<WorldEnt>& worldEntities, BombInfo& out);
void DrawBombTimer(HDC dc, int sw, int sh, const BombInfo& bomb);
void DrawCrosshair(HDC dc, int sw, int sh);
void ReadDamageLog(HANDLE hProc, uintptr_t client, uintptr_t localPawn);
