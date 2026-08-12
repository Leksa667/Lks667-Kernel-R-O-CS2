// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#pragma once

#include <windows.h>
#include <string>

void KdmLogMessage(const std::wstring& message);




bool MapKernelDriver(const std::wstring& sysPath, std::wstring& error);




bool ProbeDriverAlive(const wchar_t* processName);
