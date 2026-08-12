// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#include "driver_loader.hpp"
#include "overlay.hpp"

#include <tlhelp32.h>
#include <vector>

#include "kdmapper.hpp"
#include "intel_driver.hpp"
#include "utils.hpp"

#include "../Lks_KernelDriver/protocol.hpp"

void KdmLogMessage(const std::wstring& message) {
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, message.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return;
    std::string narrow(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        message.c_str(),
        -1,
        narrow.data(),
        size,
        nullptr,
        nullptr);
    EspLog("[kdmapper] %s", narrow.c_str());
}



static bool MapBaseCallback(
    ULONG64* param1,
    ULONG64* param2,
    ULONG64 allocationPtr,
    ULONG64 allocationSize) {
    (void)param2;
    (void)allocationSize;
    *param1 = allocationPtr;
    return true;
}

bool MapKernelDriver(const std::wstring& sysPath, std::wstring& error) {
    std::vector<uint8_t> rawImage;
    if (!kdmUtils::ReadFileToMemory(sysPath, &rawImage) ||
        rawImage.empty()) {
        error = L"failed to read driver image: " + sysPath;
        return false;
    }

    const NTSTATUS loadStatus = intel_driver::Load();
    if (!NT_SUCCESS(loadStatus)) {
        error = L"failed to load vulnerable driver (0x" +
            std::to_wstring(loadStatus) + L")";
        return false;
    }

    struct VulnerableDriverGuard {
        ~VulnerableDriverGuard() { intel_driver::Unload(); }
    } vulnerableDriverGuard;

    NTSTATUS exitCode = 0;
    const ULONG64 mapped = kdmapper::MapDriver(
        rawImage.data(),
        0,
        0,
        false,
        true,
        kdmapper::AllocationMode::AllocatePool,
        false,
        MapBaseCallback,
        &exitCode);

    if (!mapped || !NT_SUCCESS(exitCode)) {
        error = L"driver entry failed (0x" +
            std::to_wstring(exitCode) + L")";
        return false;
    }
    return true;
}

typedef LONG(NTAPI* pfnNtQueryInformationProcessT)(
    HANDLE, ULONG, PVOID, ULONG, PULONG);

struct ProbePbi {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
};

static DWORD FindProcessPid(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry = { sizeof(entry) };
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

bool ProbeDriverAlive(const wchar_t* processName) {
    const DWORD pid = FindProcessPid(processName);
    if (!pid) return false;

    HANDLE h = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;

    const auto ntQuery = reinterpret_cast<pfnNtQueryInformationProcessT>(
        GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess"));
    ProbePbi pbi = {};
    bool alive = false;
    if (ntQuery && ntQuery(h, 0, &pbi, sizeof(pbi), nullptr) == 0 &&
        pbi.PebBaseAddress) {
        PVOID processParameters = nullptr;
        SIZE_T read = 0;
        if (ReadProcessMemory(
                h,
                reinterpret_cast<LPCVOID>(
                    reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x20),
                &processParameters,
                sizeof(processParameters),
                &read) &&
            read == sizeof(processParameters) && processParameters) {
            PVOID shmAddress = nullptr;
            if (ReadProcessMemory(
                    h,
                    reinterpret_cast<LPCVOID>(
                        reinterpret_cast<uintptr_t>(processParameters) +
                        0x208),
                    &shmAddress,
                    sizeof(shmAddress),
                    &read) &&
                read == sizeof(shmAddress) && shmAddress) {
                struct ProbeHeader {
                    ULONG Signature;
                    USHORT Version;
                    USHORT HeaderSize;
                    ULONG TotalSize;
                    ULONG TargetProcessId;
                    ULONG DriverHeartbeat;
                    LONG DriverState;
                } header = {};
                if (ReadProcessMemory(
                        h, shmAddress, &header, sizeof(header), &read) &&
                    read == sizeof(header) &&
                    header.Signature == LKS_SHARED_SIGNATURE &&
                    header.DriverState == LksDriverRunning)
                    alive = true;
            }
        }
    }
    CloseHandle(h);
    return alive;
}
