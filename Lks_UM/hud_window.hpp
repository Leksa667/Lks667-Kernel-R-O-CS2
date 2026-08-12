#pragma once
#include <windows.h>
#include <string>
#include "overlay.hpp"

bool HudCreate(HINSTANCE hInst, const std::wstring& configFolder);
void HudDestroy();
void HudUpdate(bool showInfo, bool showBomb, float fps, int players,
    float readMs, float snapshotHz, float presentMs, const BombInfo& bomb);
void HudRefresh();