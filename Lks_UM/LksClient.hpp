// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#pragma once

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../Lks_KernelDriver/protocol.hpp"
#include "../Lks_KernelDriver/xor_string.hpp"

typedef NTSTATUS(NTAPI* pfnNtQueryInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
);

struct LksTransportSnapshot {
    bool valid = false;
    uint32_t heartbeat = 0;
    LONG driverState = LksDriverStopped;
    LONG mouseStatus = LksMouseIdle;
    uint32_t mouseQueued = 0;
    uint32_t mouseDispatched = 0;
    uint32_t lastCompletedTransaction = 0;
    uint32_t responseDrops = 0;
    uint32_t mouseTargetThreadId = 0;
    uint32_t mouseApcNormal = 0;
    uint32_t mouseApcRundown = 0;
    uint32_t mouseLastIrql = 0;
    uint32_t mouseTargetProcessId = 0;
    uint32_t activeTransaction = 0;
    uint32_t activeMessageType = 0;
    uint32_t activeReadCount = 0;
    uint64_t activeAddress = 0;
    uint32_t readBudgetTimeouts = 0;
    uint32_t controllerProcessId = 0;
    uint64_t controllerAddress = 0;
    LONG controllerMappingStatus = 0;
};

class LksClient {
private:
    HANDLE m_hProcess = nullptr;
    DWORD m_TargetPid = 0;
    bool m_InContext = false;
    PLKS_SHARED_MEMORY m_pLocalShm = nullptr;
    PVOID m_pRemoteShmAddr = nullptr;

    std::mutex m_CommandMutex;
    std::mutex m_RespMutex;
    std::unordered_map<uint32_t, std::promise<std::vector<uint8_t>>> m_Promises;
    std::atomic_uint32_t m_NextTxn{1};
    std::atomic_uint32_t m_PendingResponses{0};
    std::atomic_bool m_PumpRunning{false};
    std::thread m_PumpThread;
    std::atomic_bool m_TransportHealthy{false};
    std::atomic_uint32_t m_ConsecutiveTimeouts{0};
    uint32_t m_LastHeartbeat = 0;
    std::chrono::steady_clock::time_point m_LastHeartbeatObservedAt{};
    std::chrono::steady_clock::time_point m_LastHealthPollAt{};
    std::chrono::steady_clock::time_point m_TransportBackoffUntil{};
    PVOID m_MouseInputBuffer = nullptr;

    static bool IsValidSharedHeader(
        const LKS_SHARED_HEADER& header,
        DWORD expectedPid) {
        return header.Signature == LKS_SHARED_SIGNATURE &&
               header.Version == LKS_PROTOCOL_VERSION &&
               header.HeaderSize == LKS_SHARED_HEADER_SIZE &&
               header.TotalSize == LKS_SHARED_MEMORY_SIZE &&
               header.TargetProcessId == expectedPid;
    }

    PVOID GetSharedMemoryAddress() const {
        HMODULE ntdll = GetModuleHandleA(XorString("ntdll.dll"));
        if (!ntdll || !m_hProcess) return nullptr;

        auto queryInformationProcess =
            reinterpret_cast<pfnNtQueryInformationProcess>(
                GetProcAddress(ntdll, XorString("NtQueryInformationProcess")));
        if (!queryInformationProcess) return nullptr;

        PROCESS_BASIC_INFORMATION pbi = {};
        ULONG returnLength = 0;
        if (queryInformationProcess(
                m_hProcess, 0, &pbi, sizeof(pbi), &returnLength) != 0 ||
            !pbi.PebBaseAddress) {
            return nullptr;
        }

        PVOID processParameters = nullptr;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(
                    reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x20),
                &processParameters,
                sizeof(processParameters),
                &bytesRead) ||
            bytesRead != sizeof(processParameters) ||
            !processParameters) {
            return nullptr;
        }

        PVOID sharedMemory = nullptr;
        if (!ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(
                    reinterpret_cast<uintptr_t>(processParameters) + 0x208),
                &sharedMemory,
                sizeof(sharedMemory),
                &bytesRead) ||
            bytesRead != sizeof(sharedMemory)) {
            return nullptr;
        }
        return sharedMemory;
    }

    bool ReadRemote(uintptr_t address, void* buffer, size_t size) const {
        SIZE_T bytesRead = 0;
        return m_hProcess &&
               ReadProcessMemory(
                   m_hProcess,
                   reinterpret_cast<LPCVOID>(address),
                   buffer,
                   size,
                   &bytesRead) != FALSE &&
               bytesRead == size;
    }

    bool WriteRemote(uintptr_t address, const void* buffer, size_t size) const {
        SIZE_T bytesWritten = 0;
        return m_hProcess &&
               WriteProcessMemory(
                   m_hProcess,
                   reinterpret_cast<LPVOID>(address),
                   buffer,
                   size,
                   &bytesWritten) != FALSE &&
               bytesWritten == size;
    }

    bool ReadSharedHeaderSnapshot(LKS_SHARED_HEADER& header) const {
        ZeroMemory(&header, sizeof(header));
        if (m_InContext) {
            if (!m_pLocalShm) return false;
            MemoryBarrier();
            memcpy(&header, m_pLocalShm, sizeof(header));
            MemoryBarrier();
            return IsValidSharedHeader(header, m_TargetPid);
        }
        return m_pRemoteShmAddr &&
               ReadRemote(
                   reinterpret_cast<uintptr_t>(m_pRemoteShmAddr),
                   &header,
                   sizeof(header)) &&
               IsValidSharedHeader(header, m_TargetPid);
    }

    bool RefreshTransportHealth(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < m_TransportBackoffUntil)
            return false;
        if (!force && m_LastHealthPollAt.time_since_epoch().count() != 0 &&
            now - m_LastHealthPollAt < std::chrono::milliseconds(20)) {
            return m_TransportHealthy.load(std::memory_order_acquire);
        }
        m_LastHealthPollAt = now;

        LKS_SHARED_HEADER header = {};
        if (!ReadSharedHeaderSnapshot(header) ||
            header.DriverState != LksDriverRunning) {
            m_TransportHealthy.store(false, std::memory_order_release);
            return false;
        }

        if (header.DriverHeartbeat != m_LastHeartbeat) {
            m_LastHeartbeat = header.DriverHeartbeat;
            m_LastHeartbeatObservedAt = now;
            m_TransportHealthy.store(true, std::memory_order_release);
            return true;
        }

        if (m_LastHeartbeatObservedAt.time_since_epoch().count() == 0)
            m_LastHeartbeatObservedAt = now;
        const bool alive =
            now - m_LastHeartbeatObservedAt < std::chrono::milliseconds(250);
        m_TransportHealthy.store(alive, std::memory_order_release);
        return alive;
    }

    bool CanIssueKernelRequest() {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_TransportBackoffUntil) return false;
        return RefreshTransportHealth();
    }

    void MarkTransportFailure() {
        m_TransportHealthy.store(false, std::memory_order_release);
        m_ConsecutiveTimeouts.fetch_add(1, std::memory_order_relaxed);
        m_TransportBackoffUntil =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        m_LastHealthPollAt = {};
    }

    void MarkTransportSuccess() {
        m_TransportHealthy.store(true, std::memory_order_release);
        m_ConsecutiveTimeouts.store(0, std::memory_order_release);
        m_TransportBackoffUntil = {};
    }

    bool PromoteControllerMapping(DWORD controllerProcessId) {
        if (m_InContext) return true;
        if (!m_pRemoteShmAddr ||
            controllerProcessId != GetCurrentProcessId()) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            LKS_SHARED_HEADER remoteHeader = {};
            if (ReadRemote(
                    reinterpret_cast<uintptr_t>(m_pRemoteShmAddr),
                    &remoteHeader,
                    sizeof(remoteHeader)) &&
                IsValidSharedHeader(remoteHeader, m_TargetPid) &&
                remoteHeader.ControllerProcessId == controllerProcessId &&
                remoteHeader.ControllerMappingStatus == 0) {
                const uintptr_t controllerAddress =
                    static_cast<uintptr_t>(remoteHeader.ControllerAddressLow) |
                    (static_cast<uintptr_t>(
                        remoteHeader.ControllerAddressHigh) << 32);
                if (controllerAddress) {
                    LKS_SHARED_HEADER localHeader = {};
                    SIZE_T copied = 0;
                    if (ReadProcessMemory(
                            GetCurrentProcess(),
                            reinterpret_cast<LPCVOID>(controllerAddress),
                            &localHeader,
                            sizeof(localHeader),
                            &copied) &&
                        copied == sizeof(localHeader) &&
                        IsValidSharedHeader(localHeader, m_TargetPid)) {
                        m_PumpRunning.store(false, std::memory_order_release);
                        if (m_PumpThread.joinable()) m_PumpThread.join();
                        m_pLocalShm = reinterpret_cast<PLKS_SHARED_MEMORY>(
                            controllerAddress);
                        m_InContext = true;
                        m_LastHeartbeat = localHeader.DriverHeartbeat;
                        m_LastHeartbeatObservedAt =
                            std::chrono::steady_clock::now();
                        m_LastHealthPollAt = {};
                        m_PumpRunning.store(true, std::memory_order_release);
                        m_PumpThread =
                            std::thread(&LksClient::ResponsePump, this);
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    static void CopyToRing(
        PLKS_RING_BUFFER ring,
        uint32_t absoluteIndex,
        const uint8_t* source,
        uint32_t length) {
        const uint32_t offset = absoluteIndex % LKS_RING_DATA_SIZE;
        const uint32_t first = std::min(length, LKS_RING_DATA_SIZE - offset);
        memcpy(ring->Data + offset, source, first);
        if (length > first) memcpy(ring->Data, source + first, length - first);
    }

    static void CopyFromRing(
        const LKS_RING_BUFFER* ring,
        uint32_t absoluteIndex,
        uint8_t* destination,
        uint32_t length) {
        const uint32_t offset = absoluteIndex % LKS_RING_DATA_SIZE;
        const uint32_t first = std::min(length, LKS_RING_DATA_SIZE - offset);
        memcpy(destination, ring->Data + offset, first);
        if (length > first) memcpy(destination + first, ring->Data, length - first);
    }

    bool WriteRemoteRingData(
        uintptr_t ringAddress,
        uint32_t absoluteIndex,
        const uint8_t* source,
        uint32_t length) const {
        const uint32_t offset = absoluteIndex % LKS_RING_DATA_SIZE;
        const uint32_t first = std::min(length, LKS_RING_DATA_SIZE - offset);
        const uintptr_t dataAddress =
            ringAddress + offsetof(LKS_RING_BUFFER, Data);
        if (!WriteRemote(dataAddress + offset, source, first)) return false;
        return length == first ||
               WriteRemote(dataAddress, source + first, length - first);
    }

    bool ReadRemoteRingData(
        uintptr_t ringAddress,
        uint32_t absoluteIndex,
        uint8_t* destination,
        uint32_t length) const {
        const uint32_t offset = absoluteIndex % LKS_RING_DATA_SIZE;
        const uint32_t first = std::min(length, LKS_RING_DATA_SIZE - offset);
        const uintptr_t dataAddress =
            ringAddress + offsetof(LKS_RING_BUFFER, Data);
        if (!ReadRemote(dataAddress + offset, destination, first)) return false;
        return length == first ||
               ReadRemote(dataAddress, destination + first, length - first);
    }

    bool WriteFrame(
        LKS_MESSAGE_TYPE type,
        uint32_t transactionId,
        const void* payload,
        uint16_t payloadSize) {
        if (payloadSize > LKS_MAX_FRAME_PAYLOAD ||
            (payloadSize != 0 && !payload)) {
            return false;
        }

        LKS_FRAME_HEADER header = {};
        header.Type = static_cast<USHORT>(type);
        header.PayloadSize = payloadSize;
        header.TransactionId = transactionId;

        const uint32_t frameSize =
            static_cast<uint32_t>(sizeof(header)) + payloadSize;
        std::vector<uint8_t> frame(frameSize);
        memcpy(frame.data(), &header, sizeof(header));
        if (payloadSize) {
            memcpy(frame.data() + sizeof(header), payload, payloadSize);
        }

        std::lock_guard<std::mutex> lock(m_CommandMutex);
        if (m_InContext) {
            PLKS_RING_BUFFER ring = &m_pLocalShm->Commands;
            const uint32_t write = ring->WriteIndex;
            uint32_t read = ring->ReadIndex;
            uint32_t used = write - read;
            if (used > LKS_RING_DATA_SIZE) {
                read = write;
                used = 0;
                InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
                    static_cast<LONG>(read));
            }
            if (frameSize > LKS_RING_DATA_SIZE - used) {
                return false;
            }

            CopyToRing(ring, write, frame.data(), frameSize);
            MemoryBarrier();
            InterlockedExchange(
                reinterpret_cast<volatile LONG*>(&ring->WriteIndex),
                static_cast<LONG>(write + frameSize));
            return true;
        }

        if (!m_pRemoteShmAddr) return false;
        const uintptr_t ringAddress =
            reinterpret_cast<uintptr_t>(m_pRemoteShmAddr) +
            offsetof(LKS_SHARED_MEMORY, Commands);
        uint32_t indices[2] = {};
        if (!ReadRemote(ringAddress, indices, sizeof(indices))) return false;

        const uint32_t write = indices[0];
        uint32_t read = indices[1];
        uint32_t used = write - read;
        if (used > LKS_RING_DATA_SIZE) {
            read = write;
            used = 0;
            if (!WriteRemote(
                    ringAddress + offsetof(LKS_RING_BUFFER, ReadIndex),
                    &read,
                    sizeof(read))) {
                return false;
            }
        }
        if (frameSize > LKS_RING_DATA_SIZE - used) {
            return false;
        }

        if (!WriteRemoteRingData(
                ringAddress, write, frame.data(), frameSize)) {
            return false;
        }
        const uint32_t publishedWrite = write + frameSize;
        return WriteRemote(
            ringAddress + offsetof(LKS_RING_BUFFER, WriteIndex),
            &publishedWrite,
            sizeof(publishedWrite));
    }

    bool ReadResponseFrame(
        LKS_FRAME_HEADER& header,
        std::vector<uint8_t>& payload) {
        uint32_t write = 0;
        uint32_t read = 0;

        if (m_InContext) {
            PLKS_RING_BUFFER ring = &m_pLocalShm->Responses;
            write = ring->WriteIndex;
            read = ring->ReadIndex;
            const uint32_t used = write - read;
            if (used > LKS_RING_DATA_SIZE) {
                InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
                    static_cast<LONG>(write));
                return false;
            }
            if (used < sizeof(header)) return false;

            MemoryBarrier();
            CopyFromRing(
                ring, read, reinterpret_cast<uint8_t*>(&header),
                sizeof(header));
            const uint32_t frameSize =
                static_cast<uint32_t>(sizeof(header)) + header.PayloadSize;
            if (header.PayloadSize > LKS_MAX_FRAME_PAYLOAD ||
                frameSize > LKS_RING_DATA_SIZE) {
                InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
                    static_cast<LONG>(write));
                return false;
            }
            if (used < frameSize) return false;

            payload.resize(header.PayloadSize);
            if (header.PayloadSize) {
                CopyFromRing(
                    ring,
                    read + static_cast<uint32_t>(sizeof(header)),
                    payload.data(),
                    header.PayloadSize);
            }
            MemoryBarrier();
            InterlockedExchange(
                reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
                static_cast<LONG>(read + frameSize));
            return true;
        }

        if (!m_pRemoteShmAddr) return false;
        const uintptr_t ringAddress =
            reinterpret_cast<uintptr_t>(m_pRemoteShmAddr) +
            offsetof(LKS_SHARED_MEMORY, Responses);
        uint32_t indices[2] = {};
        if (!ReadRemote(ringAddress, indices, sizeof(indices))) return false;
        write = indices[0];
        read = indices[1];

        const uint32_t used = write - read;
        if (used > LKS_RING_DATA_SIZE) {
            WriteRemote(
                ringAddress + offsetof(LKS_RING_BUFFER, ReadIndex),
                &write,
                sizeof(write));
            return false;
        }
        if (used < sizeof(header)) return false;
        if (!ReadRemoteRingData(
                ringAddress,
                read,
                reinterpret_cast<uint8_t*>(&header),
                sizeof(header))) {
            return false;
        }

        const uint32_t frameSize =
            static_cast<uint32_t>(sizeof(header)) + header.PayloadSize;
        if (header.PayloadSize > LKS_MAX_FRAME_PAYLOAD ||
            frameSize > LKS_RING_DATA_SIZE) {
            WriteRemote(
                ringAddress + offsetof(LKS_RING_BUFFER, ReadIndex),
                &write,
                sizeof(write));
            return false;
        }
        if (used < frameSize) return false;

        payload.resize(header.PayloadSize);
        if (header.PayloadSize &&
            !ReadRemoteRingData(
                ringAddress,
                read + static_cast<uint32_t>(sizeof(header)),
                payload.data(),
                header.PayloadSize)) {
            return false;
        }

        const uint32_t publishedRead = read + frameSize;
        return WriteRemote(
            ringAddress + offsetof(LKS_RING_BUFFER, ReadIndex),
            &publishedRead,
            sizeof(publishedRead));
    }

    void DispatchResponse(
        const LKS_FRAME_HEADER& header,
        std::vector<uint8_t>&& payload) {
        if (header.Type != LksMessageReadMemoryResult &&
            header.Type != LksMessageReadBatchResult) {
            return;
        }

        std::promise<std::vector<uint8_t>> promise;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_RespMutex);
            auto it = m_Promises.find(header.TransactionId);
            if (it != m_Promises.end()) {
                promise = std::move(it->second);
                m_Promises.erase(it);
                found = true;
            }
        }
        if (found) {
            m_PendingResponses.fetch_sub(1, std::memory_order_release);
            promise.set_value(std::move(payload));
        }
    }

    void ResponsePump() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        std::vector<uint8_t> payload;
        payload.reserve(LKS_MAX_FRAME_PAYLOAD);
        uint32_t activeSpins = 0;
        uint32_t lingerSpins = 0;
        while (m_PumpRunning.load(std::memory_order_acquire)) {
            LKS_FRAME_HEADER header = {};
            if (ReadResponseFrame(header, payload)) {
                DispatchResponse(header, std::move(payload));
                payload.clear();
                payload.reserve(LKS_MAX_FRAME_PAYLOAD);
                activeSpins = 0;
                lingerSpins = 8192;
                continue;
            }

            if (m_PendingResponses.load(std::memory_order_acquire) != 0 ||
                lingerSpins != 0) {
                if (m_PendingResponses.load(std::memory_order_relaxed) == 0)
                    --lingerSpins;
                YieldProcessor();
                if (++activeSpins >= 256) {
                    SwitchToThread();
                    activeSpins = 0;
                }
            } else {
                activeSpins = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    uint32_t NextTransaction() {
        uint32_t transaction =
            m_NextTxn.fetch_add(1, std::memory_order_relaxed);
        if (transaction == 0) {
            transaction =
                m_NextTxn.fetch_add(1, std::memory_order_relaxed);
        }
        return transaction;
    }

    std::future<std::vector<uint8_t>> RegisterPromise(uint32_t transaction) {
        std::promise<std::vector<uint8_t>> promise;
        auto future = promise.get_future();
        std::lock_guard<std::mutex> lock(m_RespMutex);
        const auto inserted =
            m_Promises.emplace(transaction, std::move(promise)).second;
        if (inserted)
            m_PendingResponses.fetch_add(1, std::memory_order_release);
        return future;
    }

    void CancelPromise(uint32_t transaction) {
        std::lock_guard<std::mutex> lock(m_RespMutex);
        if (m_Promises.erase(transaction) != 0)
            m_PendingResponses.fetch_sub(1, std::memory_order_release);
    }

public:
    DWORD GetProcessIdByName(const wchar_t* processName) const {
        DWORD pid = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, processName) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return pid;
    }

    LksClient() = default;
    LksClient(const LksClient&) = delete;
    LksClient& operator=(const LksClient&) = delete;

    ~LksClient() {
        Shutdown();
    }

    bool Initialize(const wchar_t* targetProcessName = L"cs2.exe") {
        Shutdown();
        if (!m_MouseInputBuffer) {
            
            
            
            m_MouseInputBuffer = VirtualAlloc(
                nullptr,
                0x1000,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE);
            if (!m_MouseInputBuffer) return false;
        }
        m_TargetPid = GetProcessIdByName(targetProcessName);
        if (!m_TargetPid) return false;

        if (GetCurrentProcessId() == m_TargetPid) {
            m_InContext = true;
#if defined(_WIN64)
            const uintptr_t peb = __readgsqword(0x60);
#else
            const uintptr_t peb = __readfsdword(0x30);
#endif
            PVOID processParameters =
                *reinterpret_cast<PVOID*>(peb + 0x20);
            if (!processParameters) return false;
            m_pLocalShm = *reinterpret_cast<PLKS_SHARED_MEMORY*>(
                reinterpret_cast<uintptr_t>(processParameters) + 0x208);
            if (!m_pLocalShm ||
                !IsValidSharedHeader(
                    *reinterpret_cast<LKS_SHARED_HEADER*>(m_pLocalShm),
                    m_TargetPid)) {
                m_pLocalShm = nullptr;
                return false;
            }
        } else {
            m_hProcess = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                    PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                FALSE,
                m_TargetPid);
            if (!m_hProcess) return false;

            m_pRemoteShmAddr = GetSharedMemoryAddress();
            if (!m_pRemoteShmAddr) {
                Shutdown();
                return false;
            }

            LKS_SHARED_HEADER header = {};
            if (!ReadRemote(
                    reinterpret_cast<uintptr_t>(m_pRemoteShmAddr),
                    &header,
                    LKS_SHARED_HEADER_SIZE) ||
                !IsValidSharedHeader(header, m_TargetPid)) {
                Shutdown();
                return false;
            }
        }

        LKS_SHARED_HEADER liveHeader = {};
        if (!ReadSharedHeaderSnapshot(liveHeader) ||
            liveHeader.DriverState != LksDriverRunning) {
            Shutdown();
            return false;
        }
        m_LastHeartbeat = liveHeader.DriverHeartbeat;
        m_LastHeartbeatObservedAt = std::chrono::steady_clock::now();
        m_LastHealthPollAt = {};
        m_TransportBackoffUntil = {};
        m_TransportHealthy.store(true, std::memory_order_release);
        m_ConsecutiveTimeouts.store(0, std::memory_order_release);

        m_PumpRunning.store(true, std::memory_order_release);
        m_PumpThread = std::thread(&LksClient::ResponsePump, this);
        return true;
    }

    void Shutdown() {
        m_PumpRunning.store(false, std::memory_order_release);
        if (m_PumpThread.joinable()) m_PumpThread.join();

        {
            std::lock_guard<std::mutex> lock(m_RespMutex);
            for (auto& [transaction, promise] : m_Promises) {
                UNREFERENCED_PARAMETER(transaction);
                try {
                    promise.set_value({});
                } catch (...) {
                }
            }
            m_Promises.clear();
            m_PendingResponses.store(0, std::memory_order_release);
        }

        if (m_hProcess) {
            CloseHandle(m_hProcess);
            m_hProcess = nullptr;
        }
        m_TargetPid = 0;
        m_InContext = false;
        m_pLocalShm = nullptr;
        m_pRemoteShmAddr = nullptr;
        m_TransportHealthy.store(false, std::memory_order_release);
        m_ConsecutiveTimeouts.store(0, std::memory_order_release);
        m_LastHeartbeat = 0;
        m_LastHeartbeatObservedAt = {};
        m_LastHealthPollAt = {};
        m_TransportBackoffUntil = {};
    }

    bool IsInContext() const { return m_InContext; }
    HANDLE GetProcessHandle() const { return m_hProcess; }
    DWORD GetTargetPid() const { return m_TargetPid; }
    bool IsTransportHealthy() { return RefreshTransportHealth(); }
    uint32_t GetConsecutiveTimeouts() const {
        return m_ConsecutiveTimeouts.load(std::memory_order_acquire);
    }

    LksTransportSnapshot GetTransportSnapshot() const {
        LksTransportSnapshot snapshot = {};
        LKS_SHARED_HEADER header = {};
        if (!ReadSharedHeaderSnapshot(header)) return snapshot;
        snapshot.valid = true;
        snapshot.heartbeat = header.DriverHeartbeat;
        snapshot.driverState = header.DriverState;
        snapshot.mouseStatus = header.LastMouseStatus;
        snapshot.mouseQueued = header.MouseQueueCount;
        snapshot.mouseDispatched = header.MouseDispatchCount;
        snapshot.lastCompletedTransaction =
            header.LastCompletedTransaction;
        snapshot.responseDrops = header.ResponseDropCount;
        snapshot.mouseTargetThreadId = header.MouseTargetThreadId;
        snapshot.mouseApcNormal = header.MouseApcNormalCount;
        snapshot.mouseApcRundown = header.MouseApcRundownCount;
        snapshot.mouseLastIrql = header.MouseLastIrql;
        snapshot.mouseTargetProcessId = header.MouseTargetProcessId;
        snapshot.activeTransaction = header.ActiveTransaction;
        snapshot.activeMessageType = header.ActiveMessageType;
        snapshot.activeReadCount = header.ActiveReadCount;
        snapshot.activeAddress =
            static_cast<uint64_t>(header.ActiveAddressLow) |
            (static_cast<uint64_t>(header.ActiveAddressHigh) << 32);
        snapshot.readBudgetTimeouts = header.ReadBudgetTimeoutCount;
        snapshot.controllerProcessId = header.ControllerProcessId;
        snapshot.controllerAddress =
            static_cast<uint64_t>(header.ControllerAddressLow) |
            (static_cast<uint64_t>(header.ControllerAddressHigh) << 32);
        snapshot.controllerMappingStatus =
            header.ControllerMappingStatus;
        return snapshot;
    }

    bool KernelRead(uintptr_t address, void* buffer, size_t size) {
        if (!buffer || size == 0 || size > LKS_MAX_READ_SIZE) return false;
        if (!CanIssueKernelRequest()) return false;

        const uint32_t transaction = NextTransaction();
        auto future = RegisterPromise(transaction);

        LKS_READ_REQUEST request = {};
        request.Address = static_cast<ULONGLONG>(address);
        request.Size = static_cast<ULONG>(size);
        if (!WriteFrame(
                LksMessageReadMemory,
                transaction,
                &request,
                sizeof(request))) {
            CancelPromise(transaction);
            return false;
        }

        if (future.wait_for(std::chrono::milliseconds(100)) !=
            std::future_status::ready) {
            CancelPromise(transaction);
            MarkTransportFailure();
            return false;
        }

        const std::vector<uint8_t> response = future.get();
        if (response.size() < sizeof(LKS_READ_RESPONSE)) return false;

        LKS_READ_RESPONSE result = {};
        memcpy(&result, response.data(), sizeof(result));
        if (result.Status != 0 ||
            result.Size != size ||
            response.size() != sizeof(result) + result.Size) {
            return false;
        }
        memcpy(buffer, response.data() + sizeof(result), size);
        MarkTransportSuccess();
        return true;
    }

    std::vector<std::vector<uint8_t>> KernelReadBatch(
        const uint64_t* addresses,
        const uint16_t* sizes,
        uint16_t count) {
        std::vector<std::vector<uint8_t>> output;
        if (!addresses || !sizes || count == 0 ||
            count > LKS_MAX_BATCH_READS) {
            return output;
        }
        output.resize(count);
        if (!CanIssueKernelRequest()) return output;

        const size_t payloadSize =
            sizeof(LKS_BATCH_REQUEST_HEADER) +
            count * sizeof(LKS_BATCH_READ_ENTRY);
        std::vector<uint8_t> payload(payloadSize);
        auto* request =
            reinterpret_cast<LKS_BATCH_REQUEST_HEADER*>(payload.data());
        request->Count = count;
        request->Reserved = 0;
        auto* entries = reinterpret_cast<LKS_BATCH_READ_ENTRY*>(
            payload.data() + sizeof(*request));
        size_t responseSize = sizeof(LKS_BATCH_RESPONSE_HEADER) +
            static_cast<size_t>(count) * sizeof(LKS_BATCH_READ_RESULT);
        for (uint16_t i = 0; i < count; ++i) {
            if (sizes[i] == 0 || sizes[i] > LKS_MAX_READ_SIZE) return {};
            responseSize += sizes[i];
            if (responseSize > LKS_MAX_FRAME_PAYLOAD) return {};
            entries[i].Address = addresses[i];
            entries[i].Size = sizes[i];
            entries[i].Reserved = 0;
        }

        const uint32_t transaction = NextTransaction();
        auto future = RegisterPromise(transaction);
        if (!WriteFrame(
                LksMessageReadBatch,
                transaction,
                payload.data(),
                static_cast<uint16_t>(payload.size()))) {
            CancelPromise(transaction);
            return output;
        }

        if (future.wait_for(std::chrono::milliseconds(100)) !=
            std::future_status::ready) {
            CancelPromise(transaction);
            MarkTransportFailure();
            return output;
        }

        const std::vector<uint8_t> response = future.get();
        if (response.size() < sizeof(LKS_BATCH_RESPONSE_HEADER)) return output;

        LKS_BATCH_RESPONSE_HEADER responseHeader = {};
        memcpy(&responseHeader, response.data(), sizeof(responseHeader));
        if (responseHeader.Count != count) return output;

        const size_t resultsSize =
            count * sizeof(LKS_BATCH_READ_RESULT);
        const size_t dataOffset =
            sizeof(LKS_BATCH_RESPONSE_HEADER) + resultsSize;
        if (response.size() < dataOffset) return output;

        const auto* results = reinterpret_cast<const LKS_BATCH_READ_RESULT*>(
            response.data() + sizeof(LKS_BATCH_RESPONSE_HEADER));
        size_t cursor = dataOffset;
        for (uint16_t i = 0; i < count; ++i) {
            if (results[i].Size > LKS_MAX_READ_SIZE ||
                cursor + results[i].Size > response.size()) {
                return std::vector<std::vector<uint8_t>>(count);
            }
            if (results[i].Status == 0 && results[i].Size == sizes[i]) {
                output[i].assign(
                    response.begin() + cursor,
                    response.begin() + cursor + results[i].Size);
            }
            cursor += results[i].Size;
        }
        MarkTransportSuccess();
        return output;
    }

    bool SendMouseMove(int dx, int dy) {
        if (!RefreshTransportHealth()) return false;
        LKS_MOUSE_MOVE move = {};
        move.DeltaX = dx;
        move.DeltaY = dy;
        return WriteFrame(LksMessageMouseMove, 0, &move, sizeof(move));
    }

    bool SendTargetThread(DWORD processId, DWORD threadId) {
        if (!processId || !threadId) return false;
        LKS_TARGET_THREAD target = {};
        target.ProcessId = processId;
        target.ThreadId = threadId;
        target.InputBuffer =
            reinterpret_cast<ULONGLONG>(m_MouseInputBuffer);
        const bool sent = WriteFrame(
            LksMessageTargetThread, 0, &target, sizeof(target));
        if (sent && processId == GetCurrentProcessId() && !m_InContext)
            return PromoteControllerMapping(processId);
        return sent;
    }

    
    
    
    bool RequestDriverStop() {
        return WriteFrame(LksMessageDriverStop, 0, nullptr, 0);
    }
};
