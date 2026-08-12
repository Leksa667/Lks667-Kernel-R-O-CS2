// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#include <ntddk.h>
#include <ntimage.h>
#include <intrin.h>
#include "protocol.hpp"
#include "xor_string.hpp"

struct LKS_APC_STATE {
    ULONG_PTR Reserved[6];
};

extern "C" {
    NTKERNELAPI VOID KeStackAttachProcess(
        PRKPROCESS Process,
        struct LKS_APC_STATE* ApcState);
    NTKERNELAPI VOID KeUnstackDetachProcess(
        struct LKS_APC_STATE* ApcState);
    NTKERNELAPI NTSTATUS ZwWaitForSingleObject(
        HANDLE Handle,
        BOOLEAN Alertable,
        PLARGE_INTEGER Timeout);
}

struct LKS_PEB {
    UCHAR Reserved1[0x020];
    PVOID ProcessParameters;
};

struct LKS_PARAMS {
    UCHAR Reserved[0x208];
    PVOID Reserved2[5];
};

static volatile LONG64 g_TargetDirectoryTableBase = 0;

typedef struct _LKS_TRANSLATION_CACHE_ENTRY {
    ULONG64 VirtualPage;
    ULONG64 PhysicalPage;
} LKS_TRANSLATION_CACHE_ENTRY, *PLKS_TRANSLATION_CACHE_ENTRY;

#define LKS_TRANSLATION_CACHE_CAPACITY (LKS_MAX_BATCH_READS * 2u)
static_assert(
    (LKS_TRANSLATION_CACHE_CAPACITY &
        (LKS_TRANSLATION_CACHE_CAPACITY - 1u)) == 0,
    "The translation cache capacity must be a power of two");


NTSTATUS ReadPhysicalAddress(ULONG64 PhysicalAddress, PVOID lpBuffer, SIZE_T Size, SIZE_T* BytesRead)
{
    MM_COPY_ADDRESS addr;
    addr.PhysicalAddress.QuadPart = PhysicalAddress;
    return MmCopyMemory(lpBuffer, addr, Size, MM_COPY_MEMORY_PHYSICAL, BytesRead);
}

static ULONG64 CaptureDirectoryTableBase(PEPROCESS process)
{
    if (!process) return 0;

    LKS_APC_STATE apcState = {};
    KeStackAttachProcess(
        reinterpret_cast<PRKPROCESS>(process), &apcState);
    const ULONG64 directoryTableBase = __readcr3() & ~0xFFFULL;
    KeUnstackDetachProcess(&apcState);
    return directoryTableBase;
}


ULONG64 TranslateVirtualToPhysical(
    ULONG64 DirectoryTableBase,
    ULONG64 VirtualAddress,
    ULONGLONG deadline)
{
    if (!DirectoryTableBase || VirtualAddress > 0x00007FFFFFFFFFFFULL)
        return 0;

    ULONG64 pml4e_idx = (VirtualAddress >> 39) & 0x1FF;
    ULONG64 pdpte_idx = (VirtualAddress >> 30) & 0x1FF;
    ULONG64 pde_idx   = (VirtualAddress >> 21) & 0x1FF;
    ULONG64 pte_idx   = (VirtualAddress >> 12) & 0x1FF;
    ULONG64 page_offset = VirtualAddress & 0xFFF;

    SIZE_T read_size = 0;
    ULONG64 entry = 0;

    ULONG64 pml4_phys = DirectoryTableBase & ~0xFFFULL;
    ULONG64 pml4e_phys = pml4_phys + (pml4e_idx * sizeof(ULONG64));
    if (deadline && KeQueryInterruptTime() >= deadline) return 0;
    if (!NT_SUCCESS(ReadPhysicalAddress(pml4e_phys, &entry, sizeof(ULONG64), &read_size)) ||
        read_size != sizeof(entry) || !(entry & 1))
        return 0;

    ULONG64 pdpt_phys = entry & 0x000FFFFFFFFFF000ULL;
    ULONG64 pdpte_phys = pdpt_phys + (pdpte_idx * sizeof(ULONG64));
    if (deadline && KeQueryInterruptTime() >= deadline) return 0;
    if (!NT_SUCCESS(ReadPhysicalAddress(pdpte_phys, &entry, sizeof(ULONG64), &read_size)) ||
        read_size != sizeof(entry) || !(entry & 1))
        return 0;

    if (entry & 0x80) {
        return (entry & 0x000FFFFFC0000000ULL) + (VirtualAddress & 0x3FFFFFFF);
    }

    ULONG64 pd_phys = entry & 0x000FFFFFFFFFF000ULL;
    ULONG64 pde_phys = pd_phys + (pde_idx * sizeof(ULONG64));
    if (deadline && KeQueryInterruptTime() >= deadline) return 0;
    if (!NT_SUCCESS(ReadPhysicalAddress(pde_phys, &entry, sizeof(ULONG64), &read_size)) ||
        read_size != sizeof(entry) || !(entry & 1))
        return 0;

    if (entry & 0x80) {
        return (entry & 0x000FFFFFFFE00000ULL) + (VirtualAddress & 0x1FFFFF);
    }

    ULONG64 pt_phys = entry & 0x000FFFFFFFFFF000ULL;
    ULONG64 pte_phys = pt_phys + (pte_idx * sizeof(ULONG64));
    if (deadline && KeQueryInterruptTime() >= deadline) return 0;
    if (!NT_SUCCESS(ReadPhysicalAddress(pte_phys, &entry, sizeof(ULONG64), &read_size)) ||
        read_size != sizeof(entry) || !(entry & 1))
        return 0;

    ULONG64 page_phys = entry & 0x000FFFFFFFFFF000ULL;
    return page_phys + page_offset;
}

static ULONG64 TranslateVirtualToPhysicalCached(
    ULONG64 DirectoryTableBase,
    ULONG64 VirtualAddress,
    PLKS_TRANSLATION_CACHE_ENTRY cache,
    USHORT cacheCapacity,
    ULONGLONG deadline)
{
    const ULONG64 virtualPage = VirtualAddress & ~0xFFFULL;
    const ULONG64 pageOffset = VirtualAddress & 0xFFFULL;
    USHORT cacheIndex = 0;
    if (cache && cacheCapacity)
    {
        const ULONG64 pageNumber = virtualPage >> 12;
        cacheIndex = static_cast<USHORT>(
            (pageNumber ^ (pageNumber >> 9) ^ (pageNumber >> 18)) &
            (cacheCapacity - 1u));
        const LKS_TRANSLATION_CACHE_ENTRY cached = cache[cacheIndex];
        if (cached.VirtualPage == virtualPage && cached.PhysicalPage)
            return cached.PhysicalPage + pageOffset;
    }

    const ULONG64 physicalAddress = TranslateVirtualToPhysical(
        DirectoryTableBase, VirtualAddress, deadline);
    if (physicalAddress && cache && cacheCapacity)
    {
        cache[cacheIndex].PhysicalPage = physicalAddress & ~0xFFFULL;
        KeMemoryBarrier();
        cache[cacheIndex].VirtualPage = virtualPage;
    }
    return physicalAddress;
}

static VOID InvalidateTranslationCacheEntry(
    ULONG64 VirtualAddress,
    PLKS_TRANSLATION_CACHE_ENTRY cache,
    USHORT cacheCapacity)
{
    if (!cache || !cacheCapacity) return;

    const ULONG64 virtualPage = VirtualAddress & ~0xFFFULL;
    const ULONG64 pageNumber = virtualPage >> 12;
    const USHORT cacheIndex = static_cast<USHORT>(
        (pageNumber ^ (pageNumber >> 9) ^ (pageNumber >> 18)) &
        (cacheCapacity - 1u));
    if (cache[cacheIndex].VirtualPage == virtualPage)
    {
        cache[cacheIndex].VirtualPage = 0;
        cache[cacheIndex].PhysicalPage = 0;
    }
}


NTSTATUS ReadProcessMemoryPhysical(
    PEPROCESS Process,
    ULONG64 SourceVirtualAddress,
    PVOID TargetBuffer,
    SIZE_T Size,
    SIZE_T* BytesRead,
    PLKS_TRANSLATION_CACHE_ENTRY translationCache = NULL,
    USHORT translationCacheCapacity = 0,
    ULONGLONG deadline = 0)
{
    if (!Process || !SourceVirtualAddress || !TargetBuffer || Size == 0)
        return STATUS_INVALID_PARAMETER;

    if (SourceVirtualAddress > 0x00007FFFFFFFFFFFULL ||
        Size - 1 > 0x00007FFFFFFFFFFFULL - SourceVirtualAddress)
        return STATUS_ACCESS_VIOLATION;

    ULONG64 DirectoryTableBase = static_cast<ULONG64>(
        InterlockedCompareExchange64(
            &g_TargetDirectoryTableBase, 0, 0));
    if (!DirectoryTableBase)
    {
        DirectoryTableBase = CaptureDirectoryTableBase(Process);
        InterlockedExchange64(
            &g_TargetDirectoryTableBase,
            static_cast<LONG64>(DirectoryTableBase));
    }
    if (!DirectoryTableBase)
        return STATUS_INVALID_PARAMETER;

    SIZE_T totalBytesRead = 0;
    ULONG64 currentVA = SourceVirtualAddress;
    PUCHAR currentTarget = (PUCHAR)TargetBuffer;
    SIZE_T remainingSize = Size;
    BOOLEAN timedOut = FALSE;

    while (remainingSize > 0) {
        if (deadline && KeQueryInterruptTime() >= deadline) {
            timedOut = TRUE;
            break;
        }
        ULONG64 pageOffset = currentVA & 0xFFF;
        SIZE_T bytesToRead = 4096 - pageOffset;
        if (bytesToRead > remainingSize) {
            bytesToRead = remainingSize;
        }

        ULONG64 physicalAddress = TranslateVirtualToPhysicalCached(
            DirectoryTableBase,
            currentVA,
            translationCache,
            translationCacheCapacity,
            deadline);
        if (physicalAddress == 0) {
            if (deadline && KeQueryInterruptTime() >= deadline)
                timedOut = TRUE;
            break;
        }

        SIZE_T bytesReadThisPage = 0;
        NTSTATUS status = ReadPhysicalAddress(physicalAddress, currentTarget, bytesToRead, &bytesReadThisPage);
        if (!NT_SUCCESS(status) || bytesReadThisPage == 0) {
            InvalidateTranslationCacheEntry(
                currentVA, translationCache, translationCacheCapacity);
            break;
        }

        totalBytesRead += bytesReadThisPage;
        remainingSize -= bytesReadThisPage;
        currentVA += bytesReadThisPage;
        currentTarget += bytesReadThisPage;

        if (bytesReadThisPage < bytesToRead) {
            break;
        }
    }

    if (BytesRead) {
        *BytesRead = totalBytesRead;
    }

    if (timedOut) return STATUS_IO_TIMEOUT;
    return (totalBytesRead == Size) ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
}

#define SystemProcessInformation 5
#define SystemModuleInformation  11

typedef struct _LKS_SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR PageDirectoryBase;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} LKS_SYSTEM_PROCESS_INFORMATION, *LKS_PSYSTEM_PROCESS_INFORMATION;

typedef enum _KAPC_ENVIRONMENT {
    OriginalApcEnvironment,
    AttachedApcEnvironment,
    CurrentApcEnvironment,
    InsertApcEnvironment
} KAPC_ENVIRONMENT;

typedef VOID (*PKNORMAL_ROUTINE)(
    PVOID NormalContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
);

typedef VOID (*PKKERNEL_ROUTINE)(
    struct _KAPC *Apc,
    PKNORMAL_ROUTINE *NormalRoutine,
    PVOID *NormalContext,
    PVOID *SystemArgument1,
    PVOID *SystemArgument2
);

typedef VOID (*PKRUNDOWN_ROUTINE)(
    struct _KAPC *Apc
);

extern "C" {
    NTKERNELAPI PCSTR PsGetProcessImageFileName(PEPROCESS Process);
    NTKERNELAPI PPEB PsGetProcessPeb(PEPROCESS Process);
    NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId, PEPROCESS *Process);
    NTKERNELAPI NTSTATUS PsGetProcessExitStatus(PEPROCESS Process);
    NTSTATUS ZwQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);
    NTKERNELAPI VOID KeInitializeApc(
        struct _KAPC *Apc,
        struct _KTHREAD *Thread,
        KAPC_ENVIRONMENT Environment,
        PKKERNEL_ROUTINE KernelRoutine,
        PKRUNDOWN_ROUTINE RundownRoutine,
        PKNORMAL_ROUTINE NormalRoutine,
        KPROCESSOR_MODE ApcMode,
        PVOID NormalContext
    );
    NTKERNELAPI BOOLEAN KeInsertQueueApc(
        struct _KAPC *Apc,
        PVOID SystemArgument1,
        PVOID SystemArgument2,
        KPRIORITY Increment
    );
    NTKERNELAPI NTSTATUS PsLookupThreadByThreadId(
        HANDLE ThreadId,
        PETHREAD *Thread
    );
    NTKERNELAPI HANDLE PsGetThreadProcessId(PETHREAD Thread);
}

#define TARGET_PROCESS       L"cs2.exe"

typedef struct _SYSTEM_MODULE_ENTRY {
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} SYSTEM_MODULE_ENTRY, *PSYSTEM_MODULE_ENTRY;

typedef struct _SYSTEM_MODULE_INFORMATION {
    ULONG ModulesCount;
    SYSTEM_MODULE_ENTRY Modules[1];
} SYSTEM_MODULE_INFORMATION, *PSYSTEM_MODULE_INFORMATION;

typedef struct _SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    KPRIORITY Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    KWAIT_REASON WaitReason;
} SYSTEM_THREAD_INFORMATION, *PSYSTEM_THREAD_INFORMATION;

static VOID RingCopyIn(
    PLKS_RING_BUFFER ring,
    ULONG absoluteIndex,
    const UCHAR* source,
    ULONG length)
{
    const ULONG offset = absoluteIndex % LKS_RING_DATA_SIZE;
    const ULONG first =
        (length < LKS_RING_DATA_SIZE - offset)
            ? length
            : LKS_RING_DATA_SIZE - offset;
    RtlCopyMemory(ring->Data + offset, source, first);
    if (length > first)
        RtlCopyMemory(ring->Data, source + first, length - first);
}

static VOID RingCopyOut(
    const LKS_RING_BUFFER* ring,
    ULONG absoluteIndex,
    UCHAR* destination,
    ULONG length)
{
    const ULONG offset = absoluteIndex % LKS_RING_DATA_SIZE;
    const ULONG first =
        (length < LKS_RING_DATA_SIZE - offset)
            ? length
            : LKS_RING_DATA_SIZE - offset;
    RtlCopyMemory(destination, ring->Data + offset, first);
    if (length > first)
        RtlCopyMemory(destination + first, ring->Data, length - first);
}

static BOOLEAN RingWriteFrame(
    PLKS_RING_BUFFER ring,
    USHORT type,
    ULONG transactionId,
    const VOID* payload,
    USHORT payloadSize)
{
    if (!ring || payloadSize > LKS_MAX_FRAME_PAYLOAD ||
        (payloadSize != 0 && !payload))
        return FALSE;

    const ULONG frameSize = sizeof(LKS_FRAME_HEADER) + payloadSize;
    const ULONG write = ring->WriteIndex;
    ULONG read = ring->ReadIndex;
    ULONG used = write - read;
    if (used > LKS_RING_DATA_SIZE)
    {
        read = write;
        used = 0;
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
            static_cast<LONG>(read));
    }
    if (frameSize > LKS_RING_DATA_SIZE - used)
        return FALSE;

    LKS_FRAME_HEADER header = {};
    header.Type = type;
    header.PayloadSize = payloadSize;
    header.TransactionId = transactionId;
    RingCopyIn(
        ring,
        write,
        reinterpret_cast<const UCHAR*>(&header),
        sizeof(header));
    if (payloadSize)
        RingCopyIn(
            ring,
            write + sizeof(header),
            reinterpret_cast<const UCHAR*>(payload),
            payloadSize);

    KeMemoryBarrier();
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&ring->WriteIndex),
        static_cast<LONG>(write + frameSize));
    return TRUE;
}

static BOOLEAN RingReadFrame(
    PLKS_RING_BUFFER ring,
    PLKS_FRAME_HEADER header,
    UCHAR* payload,
    ULONG payloadCapacity)
{
    if (!ring || !header || !payload) return FALSE;

    const ULONG write = ring->WriteIndex;
    const ULONG read = ring->ReadIndex;
    const ULONG used = write - read;
    if (used > LKS_RING_DATA_SIZE)
    {
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
            static_cast<LONG>(write));
        return FALSE;
    }
    if (used < sizeof(*header)) return FALSE;

    KeMemoryBarrier();
    RingCopyOut(
        ring,
        read,
        reinterpret_cast<UCHAR*>(header),
        sizeof(*header));
    const ULONG frameSize = sizeof(*header) + header->PayloadSize;
    if (header->PayloadSize > LKS_MAX_FRAME_PAYLOAD ||
        header->PayloadSize > payloadCapacity ||
        frameSize > LKS_RING_DATA_SIZE)
    {
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
            static_cast<LONG>(write));
        return FALSE;
    }
    if (used < frameSize) return FALSE;

    if (header->PayloadSize)
        RingCopyOut(
            ring,
            read + sizeof(*header),
            payload,
            header->PayloadSize);

    KeMemoryBarrier();
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&ring->ReadIndex),
        static_cast<LONG>(read + frameSize));
    return TRUE;
}

typedef ULONG (*LKS_NT_USER_SEND_INPUT)(
    ULONG InputCount,
    PVOID Inputs,
    LONG InputSize);

typedef struct _LKS_MOUSE_INPUT_PACKET {
    LONG DeltaX;
    LONG DeltaY;
    ULONG MouseData;
    ULONG Flags;
    ULONG Time;
    ULONG_PTR ExtraInfo;
} LKS_MOUSE_INPUT_PACKET, *PLKS_MOUSE_INPUT_PACKET;


typedef struct _LKS_INPUT_PACKET {
    ULONG Type;
    ULONG Alignment;
    LKS_MOUSE_INPUT_PACKET Mouse;
} LKS_INPUT_PACKET, *PLKS_INPUT_PACKET;

static_assert(sizeof(LKS_MOUSE_INPUT_PACKET) == 32,
    "Unexpected Win64 MOUSEINPUT size");
static_assert(sizeof(LKS_INPUT_PACKET) == 40,
    "Unexpected Win64 INPUT size");

typedef struct _LKS_MOUSE_APC_BLOCK {
    KAPC Apc;
    LKS_NT_USER_SEND_INPUT SendInput;
    PETHREAD TargetThread;
    PVOID UserInputBuffer;
} LKS_MOUSE_APC_BLOCK, *PLKS_MOUSE_APC_BLOCK;

static PVOID g_NtUserSendInput = NULL;
static volatile LONG g_TargetThreadId = 0;
static volatile LONG g_MouseTargetProcessId = 0;
static volatile LONG64 g_MouseInputBuffer = 0;
static PVOID g_KernelSharedMem   = NULL;
static PVOID g_UserSharedMem     = NULL;
static PMDL g_Mdl                = NULL;
static PVOID g_ControllerSharedMem = NULL;
static PMDL g_ControllerMdl = NULL;
static PEPROCESS g_ControllerProcess = NULL;
static PEPROCESS g_TargetProcess = NULL;
static PETHREAD g_PollThread     = NULL;
static volatile LONG g_Running   = FALSE;
static volatile LONG g_Unloading = FALSE;
static volatile LONG64 g_MousePackedDelta = 0;
static volatile LONG g_MouseApcQueued = FALSE;
static volatile LONG g_ActiveMouseApcs = 0;

static PLKS_SHARED_MEMORY GetSharedTelemetry()
{
    return reinterpret_cast<PLKS_SHARED_MEMORY>(
        InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_KernelSharedMem),
            NULL,
            NULL));
}

static VOID SetMouseStatus(LONG status)
{
    PLKS_SHARED_MEMORY shared = GetSharedTelemetry();
    if (shared) InterlockedExchange(&shared->LastMouseStatus, status);
}

static VOID SetActiveReadAddress(
    PLKS_SHARED_MEMORY shared,
    ULONGLONG address)
{
    if (!shared) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(
        &shared->ActiveAddressLow), static_cast<LONG>(address));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(
        &shared->ActiveAddressHigh), static_cast<LONG>(address >> 32));
}

static BOOLEAN AsciiEquals(PCSTR left, PCSTR right)
{
    if (!left || !right) return FALSE;
    while (*left && *right)
    {
        UCHAR a = static_cast<UCHAR>(*left++);
        UCHAR b = static_cast<UCHAR>(*right++);
        if (a >= 'A' && a <= 'Z') a = static_cast<UCHAR>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<UCHAR>(b + ('a' - 'A'));
        if (a != b) return FALSE;
    }
    return *left == '\0' && *right == '\0';
}

static BOOLEAN IsImageRangeValid(ULONG rva, SIZE_T length, ULONG imageSize)
{
    return rva < imageSize &&
           length <= static_cast<SIZE_T>(imageSize - rva);
}

static PVOID FindModuleImageBase(PCSTR moduleName, PULONG imageSize)
{
    if (imageSize) *imageSize = 0;

    ULONG required = 0;
    NTSTATUS status = ZwQuerySystemInformation(
        SystemModuleInformation, NULL, 0, &required);
    if (status != STATUS_INFO_LENGTH_MISMATCH || required == 0)
        return NULL;

    auto* modules = reinterpret_cast<PSYSTEM_MODULE_INFORMATION>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, required, 'MwkL'));
    if (!modules) return NULL;

    status = ZwQuerySystemInformation(
        SystemModuleInformation, modules, required, &required);
    PVOID base = NULL;
    if (NT_SUCCESS(status))
    {
        for (ULONG i = 0; i < modules->ModulesCount; ++i)
        {
            const SYSTEM_MODULE_ENTRY& entry = modules->Modules[i];
            if (entry.OffsetToFileName >= sizeof(entry.FullPathName))
                continue;

            PCSTR fileName = reinterpret_cast<PCSTR>(
                entry.FullPathName + entry.OffsetToFileName);
            if (AsciiEquals(fileName, moduleName))
            {
                base = entry.ImageBase;
                if (imageSize) *imageSize = entry.ImageSize;
                break;
            }
        }
    }

    ExFreePoolWithTag(modules, 'MwkL');
    return base;
}

static BOOLEAN ImageExportNameEquals(
    const UCHAR* image,
    ULONG imageSize,
    ULONG nameRva,
    PCSTR expected)
{
    if (!image || !expected || nameRva >= imageSize) return FALSE;
    ULONG index = nameRva;
    while (*expected)
    {
        if (index >= imageSize ||
            image[index] != static_cast<UCHAR>(*expected++))
            return FALSE;
        ++index;
    }
    return index < imageSize && image[index] == '\0';
}

static PVOID FindImageExport(
    PVOID imageBase,
    ULONG imageSize,
    PCSTR exportName)
{
    if (!imageBase || imageSize < sizeof(IMAGE_DOS_HEADER)) return NULL;
    const auto* image = reinterpret_cast<const UCHAR*>(imageBase);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        !IsImageRangeValid(
            static_cast<ULONG>(dos->e_lfanew),
            sizeof(IMAGE_NT_HEADERS64),
            imageSize))
        return NULL;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return NULL;

    const IMAGE_DATA_DIRECTORY directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress ||
        !IsImageRangeValid(
            directory.VirtualAddress,
            sizeof(IMAGE_EXPORT_DIRECTORY),
            imageSize))
        return NULL;

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        image + directory.VirtualAddress);
    if (!exports->NumberOfNames || !exports->NumberOfFunctions ||
        !IsImageRangeValid(
            exports->AddressOfNames,
            static_cast<SIZE_T>(exports->NumberOfNames) * sizeof(ULONG),
            imageSize) ||
        !IsImageRangeValid(
            exports->AddressOfNameOrdinals,
            static_cast<SIZE_T>(exports->NumberOfNames) * sizeof(USHORT),
            imageSize) ||
        !IsImageRangeValid(
            exports->AddressOfFunctions,
            static_cast<SIZE_T>(exports->NumberOfFunctions) * sizeof(ULONG),
            imageSize))
        return NULL;

    const auto* names = reinterpret_cast<const ULONG*>(
        image + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const USHORT*>(
        image + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const ULONG*>(
        image + exports->AddressOfFunctions);

    for (ULONG i = 0; i < exports->NumberOfNames; ++i)
    {
        if (!ImageExportNameEquals(image, imageSize, names[i], exportName))
            continue;
        if (ordinals[i] >= exports->NumberOfFunctions) return NULL;

        const ULONG functionRva = functions[ordinals[i]];
        if (!IsImageRangeValid(functionRva, 1, imageSize)) return NULL;

        
        if (functionRva >= directory.VirtualAddress &&
            functionRva < directory.VirtualAddress + directory.Size)
            return NULL;
        return const_cast<UCHAR*>(image + functionRva);
    }
    return NULL;
}

static PVOID GetNtUserSendInput()
{
    PVOID current = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_NtUserSendInput), NULL, NULL);
    if (current) return current;

    static const PCSTR moduleNames[] = {
        "win32kfull.sys",
        "win32kbase.sys"
    };
    for (PCSTR moduleName : moduleNames)
    {
        ULONG imageSize = 0;
        PVOID imageBase = FindModuleImageBase(moduleName, &imageSize);
        PVOID resolved = FindImageExport(
            imageBase, imageSize, "NtUserSendInput");
        if (resolved)
        {
            current = InterlockedCompareExchangePointer(
                reinterpret_cast<PVOID volatile*>(&g_NtUserSendInput),
                resolved,
                NULL);
            return current ? current : resolved;
        }
    }
    return NULL;
}

static VOID ReleaseMouseApc(PKAPC apc)
{
    if (!apc) return;
    auto* block = CONTAINING_RECORD(
        apc, LKS_MOUSE_APC_BLOCK, Apc);
    if (block->TargetThread)
    {
        ObDereferenceObject(block->TargetThread);
        block->TargetThread = NULL;
    }
    InterlockedDecrement(&g_ActiveMouseApcs);
    ExFreePoolWithTag(block, 'AwkL');
}

static VOID MouseMoveApcKernelRoutine(
    PKAPC apc,
    PKNORMAL_ROUTINE* normalRoutine,
    PVOID* normalContext,
    PVOID* systemArgument1,
    PVOID* systemArgument2)
{
    UNREFERENCED_PARAMETER(normalContext);
    UNREFERENCED_PARAMETER(systemArgument1);
    UNREFERENCED_PARAMETER(systemArgument2);

    if (!normalRoutine || !*normalRoutine)
    {
        PLKS_SHARED_MEMORY shared = GetSharedTelemetry();
        if (shared)
        {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                &shared->MouseApcRundownCount));
        }
        SetMouseStatus(LksMouseRundown);
        InterlockedExchange(&g_MouseApcQueued, FALSE);
        InterlockedExchange64(&g_MousePackedDelta, 0);
        ReleaseMouseApc(apc);
    }
}

static VOID MouseMoveApcRundownRoutine(PKAPC apc)
{
    PLKS_SHARED_MEMORY shared = GetSharedTelemetry();
    if (shared)
    {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &shared->MouseApcRundownCount));
    }
    SetMouseStatus(LksMouseRundown);
    InterlockedExchange(&g_MouseApcQueued, FALSE);
    InterlockedExchange64(&g_MousePackedDelta, 0);
    ReleaseMouseApc(apc);
}

static VOID MouseMoveApcNormalRoutine(
    PVOID normalContext,
    PVOID systemArgument1,
    PVOID systemArgument2)
{
    UNREFERENCED_PARAMETER(systemArgument1);
    UNREFERENCED_PARAMETER(systemArgument2);

    auto* block = reinterpret_cast<PLKS_MOUSE_APC_BLOCK>(normalContext);
    if (!block) return;

    const KIRQL normalIrql = KeGetCurrentIrql();
    PLKS_SHARED_MEMORY normalTelemetry = GetSharedTelemetry();
    if (normalTelemetry)
    {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &normalTelemetry->MouseApcNormalCount));
        InterlockedExchange(reinterpret_cast<volatile LONG*>(
            &normalTelemetry->MouseLastIrql), normalIrql);
    }

    for (;;)
    {
        const ULONGLONG packed = static_cast<ULONGLONG>(
            InterlockedExchange64(&g_MousePackedDelta, 0));
        const LONG dx = static_cast<LONG>(
            static_cast<ULONG>(packed & 0xFFFFFFFFULL));
        const LONG dy = static_cast<LONG>(
            static_cast<ULONG>(packed >> 32));
        if ((dx != 0 || dy != 0) && block->SendInput)
        {
            if (normalIrql != PASSIVE_LEVEL)
            {
                SetMouseStatus(LksMouseWrongIrql);
            }
            else
            {
                LKS_INPUT_PACKET input = {};
                input.Type = 0;              
                input.Mouse.DeltaX = dx;
                input.Mouse.DeltaY = dy;
                input.Mouse.Flags = 0x0001;  

                BOOLEAN inputCopied = FALSE;
                __try
                {
                    ProbeForWrite(
                        block->UserInputBuffer,
                        sizeof(input),
                        sizeof(ULONG_PTR));
                    RtlCopyMemory(
                        block->UserInputBuffer,
                        &input,
                        sizeof(input));
                    inputCopied = TRUE;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    inputCopied = FALSE;
                }

                ULONG dispatched = 0;
                BOOLEAN raised = !inputCopied;
                if (inputCopied)
                {
                    
                    
                    KeEnterCriticalRegion();
                    __try
                    {
                        dispatched = block->SendInput(
                            1,
                            block->UserInputBuffer,
                            sizeof(input));
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        raised = TRUE;
                    }
                    KeLeaveCriticalRegion();
                }

                PLKS_SHARED_MEMORY shared = GetSharedTelemetry();
                if (shared && dispatched == 1)
                {
                    InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                        &shared->MouseDispatchCount));
                }
                SetMouseStatus(
                    raised ? LksMouseDispatchExcept :
                    (dispatched == 1 ?
                        LksMouseDispatched : LksMouseDispatchReject));
            }
        }

        InterlockedExchange(&g_MouseApcQueued, FALSE);
        KeMemoryBarrier();
        if (InterlockedCompareExchange64(
                &g_MousePackedDelta, 0, 0) == 0)
            break;

        
        
        if (InterlockedCompareExchange(
                &g_MouseApcQueued, TRUE, FALSE) != FALSE)
            break;
    }

    ReleaseMouseApc(&block->Apc);
}

static BOOLEAN QueueMouseMoveApc(LONG dx, LONG dy)
{
    const ULONG targetThreadId = static_cast<ULONG>(
        InterlockedCompareExchange(&g_TargetThreadId, 0, 0));
    const ULONG targetProcessId = static_cast<ULONG>(
        InterlockedCompareExchange(&g_MouseTargetProcessId, 0, 0));
    const ULONGLONG userInputBuffer = static_cast<ULONGLONG>(
        InterlockedCompareExchange64(&g_MouseInputBuffer, 0, 0));
    auto sendInput = reinterpret_cast<LKS_NT_USER_SEND_INPUT>(
        GetNtUserSendInput());
    if (!targetThreadId || !targetProcessId || !userInputBuffer)
    {
        SetMouseStatus(LksMouseNoTargetThread);
        return FALSE;
    }
    if (!sendInput)
    {
        SetMouseStatus(LksMouseExportMissing);
        return FALSE;
    }

    PETHREAD targetThread = NULL;
    const NTSTATUS lookupStatus = PsLookupThreadByThreadId(
        ULongToHandle(targetThreadId), &targetThread);
    if (!NT_SUCCESS(lookupStatus) || !targetThread)
    {
        SetMouseStatus(LksMouseNoTargetThread);
        return FALSE;
    }
    if (HandleToULong(PsGetThreadProcessId(targetThread)) !=
        targetProcessId)
    {
        ObDereferenceObject(targetThread);
        SetMouseStatus(LksMouseNoTargetThread);
        return FALSE;
    }

    
    
    const ULONGLONG packed =
        static_cast<ULONGLONG>(static_cast<ULONG>(dx)) |
        (static_cast<ULONGLONG>(static_cast<ULONG>(dy)) << 32);
    InterlockedExchange64(
        &g_MousePackedDelta, static_cast<LONG64>(packed));
    if (InterlockedCompareExchange(
            &g_MouseApcQueued, TRUE, FALSE) != FALSE)
    {
        ObDereferenceObject(targetThread);
        return TRUE;
    }

    auto* block = reinterpret_cast<PLKS_MOUSE_APC_BLOCK>(
        ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            sizeof(LKS_MOUSE_APC_BLOCK),
            'AwkL'));
    if (!block)
    {
        ObDereferenceObject(targetThread);
        SetMouseStatus(LksMouseAllocationFail);
        InterlockedExchange(&g_MouseApcQueued, FALSE);
        InterlockedExchange64(&g_MousePackedDelta, 0);
        return FALSE;
    }

    RtlZeroMemory(block, sizeof(*block));
    block->SendInput = sendInput;
    block->TargetThread = targetThread;
    block->UserInputBuffer = reinterpret_cast<PVOID>(userInputBuffer);
    InterlockedIncrement(&g_ActiveMouseApcs);
    KeInitializeApc(
        &block->Apc,
        reinterpret_cast<PKTHREAD>(targetThread),
        OriginalApcEnvironment,
        MouseMoveApcKernelRoutine,
        MouseMoveApcRundownRoutine,
        MouseMoveApcNormalRoutine,
        KernelMode,
        block);

    if (!KeInsertQueueApc(&block->Apc, NULL, NULL, IO_NO_INCREMENT))
    {
        SetMouseStatus(LksMouseInsertFail);
        InterlockedExchange(&g_MouseApcQueued, FALSE);
        InterlockedExchange64(&g_MousePackedDelta, 0);
        ReleaseMouseApc(&block->Apc);
        return FALSE;
    }
    PLKS_SHARED_MEMORY shared = GetSharedTelemetry();
    if (shared)
    {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &shared->MouseQueueCount));
    }
    return TRUE;
}

static BOOLEAN IsValidUserRead(ULONGLONG address, ULONG size)
{
    return address != 0 &&
           size != 0 &&
           size <= LKS_MAX_READ_SIZE &&
           address <= 0x00007FFFFFFFFFFFULL &&
           size - 1 <= 0x00007FFFFFFFFFFFULL - address;
}

static NTSTATUS EnsureControllerMapping(
    ULONG processId,
    PLKS_SHARED_MEMORY shared)
{
    if (!processId || !shared || !g_KernelSharedMem || !g_Mdl)
        return STATUS_INVALID_DEVICE_STATE;

    if (g_ControllerProcess &&
        PsGetProcessExitStatus(g_ControllerProcess) != STATUS_PENDING)
    {
        
        
        if (g_ControllerMdl) IoFreeMdl(g_ControllerMdl);
        ObDereferenceObject(g_ControllerProcess);
        g_ControllerProcess = NULL;
        g_ControllerSharedMem = NULL;
        g_ControllerMdl = NULL;
    }

    if (g_ControllerProcess && g_ControllerSharedMem && g_ControllerMdl)
    {
        if (HandleToULong(PsGetProcessId(g_ControllerProcess)) != processId)
            return STATUS_DEVICE_BUSY;
        const ULONGLONG address = reinterpret_cast<ULONGLONG>(
            g_ControllerSharedMem);
        shared->ControllerAddressLow = static_cast<ULONG>(address);
        shared->ControllerAddressHigh = static_cast<ULONG>(address >> 32);
        shared->ControllerProcessId = processId;
        shared->ControllerMappingStatus = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }

    PEPROCESS controllerProcess = NULL;
    NTSTATUS status = PsLookupProcessByProcessId(
        ULongToHandle(processId), &controllerProcess);
    if (!NT_SUCCESS(status) || !controllerProcess)
        return status;

    PMDL controllerMdl = IoAllocateMdl(
        g_KernelSharedMem,
        LKS_SHARED_MEMORY_SIZE,
        FALSE,
        FALSE,
        NULL);
    if (!controllerMdl)
    {
        ObDereferenceObject(controllerProcess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(controllerMdl);

    PVOID controllerVa = NULL;
    LKS_APC_STATE apc = {};
    KeStackAttachProcess(
        reinterpret_cast<PRKPROCESS>(controllerProcess), &apc);
    __try
    {
        controllerVa = MmMapLockedPagesSpecifyCache(
            controllerMdl,
            UserMode,
            MmCached,
            NULL,
            FALSE,
            NormalPagePriority);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        controllerVa = NULL;
        status = GetExceptionCode();
    }
    KeUnstackDetachProcess(&apc);

    if (!controllerVa)
    {
        IoFreeMdl(controllerMdl);
        ObDereferenceObject(controllerProcess);
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    g_ControllerProcess = controllerProcess;
    g_ControllerMdl = controllerMdl;
    g_ControllerSharedMem = controllerVa;

    const ULONGLONG address = reinterpret_cast<ULONGLONG>(controllerVa);
    shared->ControllerAddressLow = static_cast<ULONG>(address);
    shared->ControllerAddressHigh = static_cast<ULONG>(address >> 32);
    KeMemoryBarrier();
    shared->ControllerProcessId = processId;
    shared->ControllerMappingStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static VOID UnloadWorker(PVOID Context);

static VOID ProcessMessage(
    const LKS_FRAME_HEADER* frame,
    const UCHAR* payload,
    ULONG payloadLength,
    PUCHAR responseScratch,
    ULONG responseScratchSize,
    PLKS_TRANSLATION_CACHE_ENTRY translationCache,
    USHORT translationCacheCapacity)
{
    if (!frame || !payload || !g_KernelSharedMem) return;
    PLKS_SHARED_MEMORY shared =
        reinterpret_cast<PLKS_SHARED_MEMORY>(g_KernelSharedMem);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(
        &shared->ActiveTransaction), static_cast<LONG>(frame->TransactionId));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(
        &shared->ActiveMessageType), static_cast<LONG>(frame->Type));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(
        &shared->ActiveReadCount), 0);
    SetActiveReadAddress(shared, 0);

    switch (frame->Type)
    {
    case LksMessageTargetThread:
        if (payloadLength == sizeof(LKS_TARGET_THREAD))
        {
            LKS_TARGET_THREAD request = {};
            RtlCopyMemory(&request, payload, sizeof(request));

            PETHREAD newThread = NULL;
            const NTSTATUS lookupStatus = PsLookupThreadByThreadId(
                ULongToHandle(request.ThreadId), &newThread);
            if (NT_SUCCESS(lookupStatus) && newThread)
            {
                if (!request.ProcessId || !request.ThreadId ||
                    !request.InputBuffer ||
                    request.InputBuffer >
                        0x00007FFFFFFFFFFFULL -
                            (sizeof(LKS_INPUT_PACKET) - 1) ||
                    HandleToULong(PsGetThreadProcessId(newThread)) !=
                        request.ProcessId)
                {
                    ObDereferenceObject(newThread);
                    DbgPrint(
                        "[LksDriver] Rejected foreign target TID=%u\n",
                        request.ThreadId);
                    break;
                }

                shared->ControllerMappingStatus = STATUS_PENDING;
                const NTSTATUS mappingStatus = EnsureControllerMapping(
                    request.ProcessId, shared);
                if (!NT_SUCCESS(mappingStatus))
                {
                    shared->ControllerMappingStatus = mappingStatus;
                    ObDereferenceObject(newThread);
                    DbgPrint(
                        "[LksDriver] Controller mapping failed: 0x%X\n",
                        mappingStatus);
                    break;
                }

                InterlockedExchange(
                    &g_TargetThreadId,
                    static_cast<LONG>(request.ThreadId));
                InterlockedExchange(
                    &g_MouseTargetProcessId,
                    static_cast<LONG>(request.ProcessId));
                InterlockedExchange64(
                    &g_MouseInputBuffer,
                    static_cast<LONG64>(request.InputBuffer));
                shared->MouseTargetThreadId = request.ThreadId;
                shared->MouseTargetProcessId = request.ProcessId;
                DbgPrint(
                    "[LksDriver] TargetThread set: PID=%u TID=%u thread=0x%llX\n",
                    request.ProcessId,
                    request.ThreadId,
                    reinterpret_cast<ULONG_PTR>(newThread));
                ObDereferenceObject(newThread);
            }
        }
        break;

    case LksMessageMouseMove:
        if (payloadLength == sizeof(LKS_MOUSE_MOVE))
        {
            LKS_MOUSE_MOVE move = {};
            RtlCopyMemory(&move, payload, sizeof(move));
            QueueMouseMoveApc(move.DeltaX, move.DeltaY);
        }
        break;

    case LksMessageReadMemory:
        if (payloadLength == sizeof(LKS_READ_REQUEST) && g_TargetProcess)
        {
            LKS_READ_REQUEST request = {};
            RtlCopyMemory(&request, payload, sizeof(request));

            UCHAR responseBuffer[
                sizeof(LKS_READ_RESPONSE) + LKS_MAX_READ_SIZE] = {};
            auto* response =
                reinterpret_cast<PLKS_READ_RESPONSE>(responseBuffer);
            response->Status = STATUS_INVALID_PARAMETER;
            response->Size = 0;

            if (IsValidUserRead(request.Address, request.Size))
            {
                InterlockedExchange(reinterpret_cast<volatile LONG*>(
                    &shared->ActiveReadCount), 1);
                SetActiveReadAddress(shared, request.Address);
                SIZE_T bytesRead = 0;
                const ULONGLONG deadline =
                    KeQueryInterruptTime() + 500000ULL;
                const NTSTATUS status = ReadProcessMemoryPhysical(
                    g_TargetProcess,
                    request.Address,
                    responseBuffer + sizeof(*response),
                    request.Size,
                    &bytesRead,
                    translationCache,
                    translationCacheCapacity,
                    deadline);
                if (status == STATUS_IO_TIMEOUT)
                {
                    InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                        &shared->ReadBudgetTimeoutCount));
                }
                response->Status = status;
                if (NT_SUCCESS(status) && bytesRead == request.Size)
                    response->Size = request.Size;
            }

            if (RingWriteFrame(
                &shared->Responses,
                LksMessageReadMemoryResult,
                frame->TransactionId,
                responseBuffer,
                static_cast<USHORT>(
                    sizeof(*response) + response->Size)))
            {
                InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(
                        &shared->LastCompletedTransaction),
                    static_cast<LONG>(frame->TransactionId));
            }
            else
            {
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                    &shared->ResponseDropCount));
            }
        }
        break;

    case LksMessageReadBatch:
        if (payloadLength >= sizeof(LKS_BATCH_REQUEST_HEADER) &&
            g_TargetProcess)
        {
            LKS_BATCH_REQUEST_HEADER requestHeader = {};
            RtlCopyMemory(
                &requestHeader, payload, sizeof(requestHeader));
            const ULONG expectedSize =
                sizeof(requestHeader) +
                requestHeader.Count * sizeof(LKS_BATCH_READ_ENTRY);
            if (requestHeader.Count == 0 ||
                requestHeader.Count > LKS_MAX_BATCH_READS ||
                payloadLength != expectedSize)
                break;

            LKS_BATCH_READ_ENTRY entries[LKS_MAX_BATCH_READS] = {};
            RtlCopyMemory(
                entries,
                payload + sizeof(requestHeader),
                requestHeader.Count * sizeof(LKS_BATCH_READ_ENTRY));

            const ULONG dataOffset =
                sizeof(LKS_BATCH_RESPONSE_HEADER) +
                requestHeader.Count * sizeof(LKS_BATCH_READ_RESULT);
            ULONG responseCapacity = dataOffset;
            for (USHORT i = 0; i < requestHeader.Count; ++i)
            {
                if (entries[i].Size <= LKS_MAX_READ_SIZE)
                {
                    if (responseCapacity >
                        responseScratchSize - entries[i].Size)
                    {
                        responseCapacity = responseScratchSize + 1;
                        break;
                    }
                    responseCapacity += entries[i].Size;
                }
            }
            if (!responseScratch ||
                dataOffset > responseScratchSize ||
                responseCapacity > responseScratchSize)
                break;

            PUCHAR responseBuffer = responseScratch;
            RtlZeroMemory(responseBuffer, dataOffset);

            auto* responseHeader =
                reinterpret_cast<PLKS_BATCH_RESPONSE_HEADER>(
                    responseBuffer);
            responseHeader->Count = requestHeader.Count;
            auto* results = reinterpret_cast<PLKS_BATCH_READ_RESULT>(
                responseBuffer + sizeof(*responseHeader));
            ULONG dataSize = 0;
            const ULONGLONG batchDeadline =
                KeQueryInterruptTime() + 500000ULL;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(
                &shared->ActiveReadCount), requestHeader.Count);

            for (USHORT i = 0; i < requestHeader.Count; ++i)
            {
                SetActiveReadAddress(shared, entries[i].Address);
                results[i].Status = STATUS_INVALID_PARAMETER;
                if (!IsValidUserRead(entries[i].Address, entries[i].Size))
                    continue;
                if (KeQueryInterruptTime() >= batchDeadline)
                {
                    results[i].Status = STATUS_IO_TIMEOUT;
                    continue;
                }

                SIZE_T bytesRead = 0;
                const NTSTATUS status = ReadProcessMemoryPhysical(
                    g_TargetProcess,
                    entries[i].Address,
                    responseBuffer + dataOffset + dataSize,
                    entries[i].Size,
                    &bytesRead,
                    translationCache,
                    translationCacheCapacity,
                    batchDeadline);
                if (status == STATUS_IO_TIMEOUT)
                {
                    InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                        &shared->ReadBudgetTimeoutCount));
                }
                results[i].Status = status;
                if (NT_SUCCESS(status) && bytesRead == entries[i].Size)
                {
                    results[i].Size = entries[i].Size;
                    dataSize += entries[i].Size;
                }
            }

            if (RingWriteFrame(
                &shared->Responses,
                LksMessageReadBatchResult,
                frame->TransactionId,
                responseBuffer,
                static_cast<USHORT>(dataOffset + dataSize)))
            {
                InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(
                        &shared->LastCompletedTransaction),
                    static_cast<LONG>(frame->TransactionId));
            }
            else
            {
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                    &shared->ResponseDropCount));
            }
        }
        break;

    case LksMessageDriverStop:
        
        
        DbgPrint("[LksDriver] DriverStop received, unloading\n");
        InterlockedExchange(&g_Unloading, TRUE);
        InterlockedExchange(&g_Running, FALSE);
        {
            HANDLE unloadHandle = NULL;
            OBJECT_ATTRIBUTES unloadAttributes;
            InitializeObjectAttributes(
                &unloadAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
            const NTSTATUS unloadStatus = PsCreateSystemThread(
                &unloadHandle,
                THREAD_ALL_ACCESS,
                &unloadAttributes,
                NULL,
                NULL,
                UnloadWorker,
                NULL);
            if (NT_SUCCESS(unloadStatus) && unloadHandle)
                ZwClose(unloadHandle);
            else
                DbgPrint(
                    "[LksDriver] DriverStop: unload worker failed 0x%X\n",
                    unloadStatus);
        }
        break;
    }
}

NTSTATUS FindProcessByName(PCWSTR name, PEPROCESS* process)
{
    NTSTATUS status;
    ULONG bufferSize = 0x10000;
    LKS_PSYSTEM_PROCESS_INFORMATION buffer = NULL;

    do
    {
        if (buffer) ExFreePool(buffer);
        buffer = reinterpret_cast<LKS_PSYSTEM_PROCESS_INFORMATION>(
            ExAllocatePool2(
                POOL_FLAG_NON_PAGED, bufferSize, 'ksL'));
        if (!buffer)
            return STATUS_INSUFFICIENT_RESOURCES;

        ULONG returnLength = 0;
        status = ZwQuerySystemInformation(
            SystemProcessInformation, buffer, bufferSize, &returnLength
        );

        if (status == STATUS_INFO_LENGTH_MISMATCH)
        {
            bufferSize *= 2;
            if (bufferSize > 0x200000)
            {
                ExFreePool(buffer);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            continue;
        }

        break;
    } while (TRUE);

    if (!NT_SUCCESS(status))
    {
        ExFreePool(buffer);
        return status;
    }

    UNICODE_STRING targetName;
    RtlInitUnicodeString(&targetName, name);

    LKS_PSYSTEM_PROCESS_INFORMATION current = buffer;
    status = STATUS_NOT_FOUND;

    while (TRUE)
    {
        if (current->ImageName.Buffer && current->UniqueProcessId)
        {
            if (RtlCompareUnicodeString(&current->ImageName, &targetName, TRUE) == 0)
            {
                status = PsLookupProcessByProcessId(current->UniqueProcessId, process);
                if (NT_SUCCESS(status)) break;
            }

            if (current->ImageName.Length > targetName.Length + sizeof(WCHAR))
            {
                UNICODE_STRING suffix;
                suffix.Length = targetName.Length;
                suffix.MaximumLength = targetName.Length;
                USHORT offset = (current->ImageName.Length - targetName.Length) / sizeof(WCHAR);
                suffix.Buffer = current->ImageName.Buffer + offset;

                if (suffix.Buffer[-1] == L'\\' || suffix.Buffer[-1] == L'/')
                {
                    if (RtlCompareUnicodeString(&suffix, &targetName, TRUE) == 0)
                    {
                        status = PsLookupProcessByProcessId(current->UniqueProcessId, process);
                        if (NT_SUCCESS(status)) break;
                    }
                }
            }
        }

        if (current->NextEntryOffset == 0) break;
        current = (LKS_PSYSTEM_PROCESS_INFORMATION)
            ((PUCHAR)current + current->NextEntryOffset);
    }

    ExFreePool(buffer);
    return status;
}

NTSTATUS SetupSharedMemory(PEPROCESS target)
{
    PHYSICAL_ADDRESS lowAddr = {};
    PHYSICAL_ADDRESS highAddr = {};
    PHYSICAL_ADDRESS skipAddr = {};
    highAddr.QuadPart = MAXLONGLONG;

    PVOID kernelVa = MmAllocateContiguousMemorySpecifyCacheNode(
        LKS_SHARED_MEMORY_SIZE, lowAddr, highAddr, skipAddr, MmCached, 0
    );

    if (!kernelVa)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(kernelVa, LKS_SHARED_MEMORY_SIZE);
    PLKS_SHARED_MEMORY shared =
        reinterpret_cast<PLKS_SHARED_MEMORY>(kernelVa);
    shared->Signature = LKS_SHARED_SIGNATURE;
    shared->Version = LKS_PROTOCOL_VERSION;
    shared->HeaderSize = LKS_SHARED_HEADER_SIZE;
    shared->TotalSize = LKS_SHARED_MEMORY_SIZE;
    shared->TargetProcessId =
        HandleToULong(PsGetProcessId(target));
    shared->DriverHeartbeat = 1;
    shared->DriverState = LksDriverRunning;
    shared->LastMouseStatus = LksMouseIdle;
    shared->ControllerMappingStatus = STATUS_NOT_FOUND;

    PMDL mdl = IoAllocateMdl(
        kernelVa, LKS_SHARED_MEMORY_SIZE, FALSE, FALSE, NULL);
    if (!mdl)
    {
        MmFreeContiguousMemory(kernelVa);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    MmBuildMdlForNonPagedPool(mdl);

    LKS_APC_STATE apc = {};
    KeStackAttachProcess(
        reinterpret_cast<PRKPROCESS>(target), &apc);

    PVOID userVa = NULL;
    BOOLEAN published = FALSE;

    __try
    {
        userVa = MmMapLockedPagesSpecifyCache(
            mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority
        );
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        userVa = NULL;
    }

    if (userVa)
    {
        __try
        {
            struct LKS_PEB *peb = (struct LKS_PEB *)PsGetProcessPeb(target);
            if (peb)
            {
                PVOID processParams = NULL;
                ProbeForRead(peb, sizeof(struct LKS_PEB), sizeof(ULONG_PTR));
                processParams = peb->ProcessParameters;

                if (processParams)
                {
                    struct LKS_PARAMS *params = (struct LKS_PARAMS *)processParams;
                    ProbeForWrite(params, sizeof(struct LKS_PARAMS), sizeof(ULONG_PTR));
                    params->Reserved2[0] = userVa;
                    published = TRUE;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            published = FALSE;
        }
    }

    if (userVa && !published)
    {
        MmUnmapLockedPages(userVa, mdl);
        userVa = NULL;
    }
    KeUnstackDetachProcess(&apc);

    if (!userVa)
    {
        IoFreeMdl(mdl);
        MmFreeContiguousMemory(kernelVa);
        return STATUS_UNSUCCESSFUL;
    }

    g_UserSharedMem = userVa;
    g_KernelSharedMem = kernelVa;
    g_Mdl = mdl;
    return STATUS_SUCCESS;
}

static VOID CleanupResources()
{
    PLKS_SHARED_MEMORY telemetry = GetSharedTelemetry();
    if (telemetry)
        InterlockedExchange(
            &telemetry->DriverState, LksDriverStopping);

    
    
    while (InterlockedCompareExchange(&g_ActiveMouseApcs, 0, 0) != 0)
    {
        LARGE_INTEGER delay;
        delay.QuadPart = -(1 * 10000);
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    InterlockedExchange(&g_MouseApcQueued, FALSE);
    InterlockedExchange64(&g_MousePackedDelta, 0);

    InterlockedExchange(&g_TargetThreadId, 0);
    InterlockedExchange(&g_MouseTargetProcessId, 0);
    InterlockedExchange64(&g_MouseInputBuffer, 0);

    PEPROCESS controllerProcess = g_ControllerProcess;
    PVOID controllerSharedMem = g_ControllerSharedMem;
    PMDL controllerMdl = g_ControllerMdl;
    g_ControllerProcess = NULL;
    g_ControllerSharedMem = NULL;
    g_ControllerMdl = NULL;
    if (controllerProcess)
    {
        const BOOLEAN controllerAlive =
            PsGetProcessExitStatus(controllerProcess) == STATUS_PENDING;
        if (controllerAlive && controllerSharedMem && controllerMdl)
        {
            LKS_APC_STATE controllerApc = {};
            KeStackAttachProcess(
                reinterpret_cast<PRKPROCESS>(controllerProcess),
                &controllerApc);
            __try
            {
                MmUnmapLockedPages(controllerSharedMem, controllerMdl);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            KeUnstackDetachProcess(&controllerApc);
        }
        if (controllerMdl) IoFreeMdl(controllerMdl);
        ObDereferenceObject(controllerProcess);
    }

    const BOOLEAN processAlive =
        g_TargetProcess &&
        PsGetProcessExitStatus(g_TargetProcess) == STATUS_PENDING;
    BOOLEAN backingCanBeReleased =
        !processAlive || !g_UserSharedMem || !g_Mdl;

    if (processAlive && g_UserSharedMem && g_Mdl)
    {
        LKS_APC_STATE apc = {};
        KeStackAttachProcess(
            reinterpret_cast<PRKPROCESS>(g_TargetProcess), &apc);
        __try
        {
            struct LKS_PEB* peb =
                reinterpret_cast<struct LKS_PEB*>(
                    PsGetProcessPeb(g_TargetProcess));
            if (peb && peb->ProcessParameters)
            {
                auto* params = reinterpret_cast<struct LKS_PARAMS*>(
                    peb->ProcessParameters);
                if (params->Reserved2[0] == g_UserSharedMem)
                    params->Reserved2[0] = NULL;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        __try
        {
            MmUnmapLockedPages(g_UserSharedMem, g_Mdl);
            backingCanBeReleased = TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        KeUnstackDetachProcess(&apc);
    }

    if (backingCanBeReleased)
    {
        g_UserSharedMem = NULL;
        if (g_Mdl)
        {
            IoFreeMdl(g_Mdl);
            g_Mdl = NULL;
        }
        if (g_KernelSharedMem)
        {
            MmFreeContiguousMemory(g_KernelSharedMem);
            g_KernelSharedMem = NULL;
        }
    }
    if (g_TargetProcess)
    {
        ObDereferenceObject(g_TargetProcess);
        g_TargetProcess = NULL;
    }
    InterlockedExchange64(&g_TargetDirectoryTableBase, 0);
}

VOID CustomDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    InterlockedExchange(&g_Unloading, TRUE);
    InterlockedExchange(&g_Running, FALSE);

    PETHREAD pollingThread = reinterpret_cast<PETHREAD>(
        InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_PollThread), NULL));
    if (pollingThread)
    {
        KeWaitForSingleObject(
            pollingThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(pollingThread);
    }
    else
    {
        CleanupResources();
    }
}

VOID PollingThread(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeSetPriorityThread(KeGetCurrentThread(), 13);

    while (g_Running && !g_PollThread)
    {
        LARGE_INTEGER shortDelay;
        shortDelay.QuadPart = -(1 * 10000);
        KeDelayExecutionThread(KernelMode, FALSE, &shortDelay);
    }

    PUCHAR payload = reinterpret_cast<PUCHAR>(
        ExAllocatePool2(
            POOL_FLAG_NON_PAGED, LKS_MAX_FRAME_PAYLOAD, 'FrkL'));
    PUCHAR responseScratch = reinterpret_cast<PUCHAR>(
        ExAllocatePool2(
            POOL_FLAG_NON_PAGED, LKS_MAX_FRAME_PAYLOAD, 'RrkL'));
    PLKS_TRANSLATION_CACHE_ENTRY translationCache =
        reinterpret_cast<PLKS_TRANSLATION_CACHE_ENTRY>(
            ExAllocatePool2(
                POOL_FLAG_NON_PAGED,
                sizeof(LKS_TRANSLATION_CACHE_ENTRY) *
                    LKS_TRANSLATION_CACHE_CAPACITY,
                'CrkL'));
    if (!payload || !responseScratch || !translationCache)
        InterlockedExchange(&g_Running, FALSE);
    if (translationCache)
    {
        RtlZeroMemory(
            translationCache,
            sizeof(LKS_TRANSLATION_CACHE_ENTRY) *
                LKS_TRANSLATION_CACHE_CAPACITY);
    }

    ULONGLONG activePollUntil = 0;
    ULONGLONG nextHeartbeat = 0;
    ULONGLONG nextTranslationCacheReset = 0;
    ULONGLONG nextProcessLivenessCheck = 0;
    while (g_Running)
    {
        const ULONGLONG interruptTime = KeQueryInterruptTime();
        if (interruptTime >= nextHeartbeat)
        {
            PLKS_SHARED_MEMORY telemetry = GetSharedTelemetry();
            if (telemetry)
            {
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                    &telemetry->DriverHeartbeat));
            }
            nextHeartbeat = interruptTime + 1ULL * 1000ULL * 10ULL;
        }
        if (translationCache &&
            interruptTime >= nextTranslationCacheReset)
        {
            RtlZeroMemory(
                translationCache,
                sizeof(LKS_TRANSLATION_CACHE_ENTRY) *
                    LKS_TRANSLATION_CACHE_CAPACITY);
            nextTranslationCacheReset =
                interruptTime + 1000ULL * 1000ULL * 10ULL;
        }
        if (interruptTime >= nextProcessLivenessCheck)
        {
            if (!g_TargetProcess ||
                PsGetProcessExitStatus(g_TargetProcess) != STATUS_PENDING)
            {
                InterlockedExchange(&g_Running, FALSE);
                break;
            }
            nextProcessLivenessCheck =
                interruptTime + 10ULL * 1000ULL * 1000ULL;
        }

        PLKS_SHARED_MEMORY shared =
            reinterpret_cast<PLKS_SHARED_MEMORY>(g_KernelSharedMem);
        LKS_FRAME_HEADER frame = {};
        if (shared &&
            RingReadFrame(
                &shared->Commands,
                &frame,
                payload,
                LKS_MAX_FRAME_PAYLOAD))
        {
            ProcessMessage(
                &frame,
                payload,
                frame.PayloadSize,
                responseScratch,
                LKS_MAX_FRAME_PAYLOAD,
                translationCache,
                static_cast<USHORT>(
                    LKS_TRANSLATION_CACHE_CAPACITY));
            InterlockedExchange(reinterpret_cast<volatile LONG*>(
                &shared->ActiveTransaction), 0);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(
                &shared->ActiveMessageType), 0);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(
                &shared->ActiveReadCount), 0);
            SetActiveReadAddress(shared, 0);
            activePollUntil = KeQueryInterruptTime() + 8ULL * 1000ULL * 10ULL;
            continue;
        }

        if (interruptTime < activePollUntil)
        {
            YieldProcessor();
            continue;
        }

        LARGE_INTEGER delay;
        delay.QuadPart = -(1 * 10000);
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    if (payload) ExFreePoolWithTag(payload, 'FrkL');
    if (responseScratch) ExFreePoolWithTag(responseScratch, 'RrkL');
    if (translationCache) ExFreePoolWithTag(translationCache, 'CrkL');
    CleanupResources();

    if (!g_Unloading)
    {
        PETHREAD pollingThread = reinterpret_cast<PETHREAD>(
            InterlockedExchangePointer(
                reinterpret_cast<PVOID volatile*>(&g_PollThread), NULL));
        if (pollingThread) ObDereferenceObject(pollingThread);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}








static VOID UnloadWorker(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    PETHREAD pollingThread = reinterpret_cast<PETHREAD>(
        InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_PollThread), NULL));
    if (pollingThread)
    {
        KeWaitForSingleObject(
            pollingThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(pollingThread);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS CustomDriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{



#if defined(LKS_STANDARD_DRIVER)
    UNREFERENCED_PARAMETER(RegistryPath);
    if (DriverObject)
        DriverObject->DriverUnload = CustomDriverUnload;
#else
    
    
    
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
#endif

    InterlockedExchange(&g_Unloading, FALSE);
    InterlockedExchange(&g_Running, FALSE);

    PEPROCESS targetProcess = NULL;
    NTSTATUS status = FindProcessByName(TARGET_PROCESS, &targetProcess);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("LksDriver: cs2.exe not found (0x%X)\n", status);
        return status;
    }
    DbgPrint("LksDriver: cs2.exe found, PEPROCESS=0x%llX\n", (ULONG_PTR)targetProcess);

    g_TargetProcess = targetProcess;
    PVOID sendInput = GetNtUserSendInput();
    if (!sendInput)
    {
        DbgPrint("LksDriver: NtUserSendInput export not found\n");
        ObDereferenceObject(g_TargetProcess);
        g_TargetProcess = NULL;
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    DbgPrint(
        "LksDriver: NtUserSendInput resolved at 0x%llX\n",
        reinterpret_cast<ULONG_PTR>(sendInput));

    const ULONG64 directoryTableBase =
        CaptureDirectoryTableBase(g_TargetProcess);
    if (!directoryTableBase)
    {
        ObDereferenceObject(g_TargetProcess);
        g_TargetProcess = NULL;
        InterlockedExchange64(&g_TargetDirectoryTableBase, 0);
        return STATUS_UNSUCCESSFUL;
    }
    InterlockedExchange64(
        &g_TargetDirectoryTableBase,
        static_cast<LONG64>(directoryTableBase));

    for (int attempt = 0; attempt < 20; attempt++)
    {
        status = SetupSharedMemory(targetProcess);
        if (NT_SUCCESS(status))
            break;

        DbgPrint("[LksDriver] SetupSharedMemory attempt %d failed: 0x%X, retrying...\n", attempt, status);

        LARGE_INTEGER delay;
        delay.QuadPart = -(200 * 10000); 
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    if (!NT_SUCCESS(status))
    {
        DbgPrint("LksDriver] SetupSharedMemory failed after retries: 0x%X\n", status);
        ObDereferenceObject(g_TargetProcess);
        g_TargetProcess = NULL;
        InterlockedExchange64(&g_TargetDirectoryTableBase, 0);
        return status;
    }
    DbgPrint("LksDriver: shared memory OK (kv=0x%llX uv=0x%llX)\n",
        (ULONG_PTR)g_KernelSharedMem, (ULONG_PTR)g_UserSharedMem);

    HANDLE threadHandle = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    InitializeObjectAttributes(
        &objectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    InterlockedExchange(&g_Running, TRUE);
    status = PsCreateSystemThread(
        &threadHandle,
        SYNCHRONIZE,
        &objectAttributes,
        NULL,
        NULL,
        PollingThread,
        NULL);
    if (!NT_SUCCESS(status))
    {
        InterlockedExchange(&g_Running, FALSE);
        CleanupResources();
        DbgPrint(
            "LksDriver: PsCreateSystemThread failed: 0x%X\n", status);
        return status;
    }

    PETHREAD pollingThread = NULL;
    status = ObReferenceObjectByHandle(
        threadHandle,
        SYNCHRONIZE,
        *PsThreadType,
        KernelMode,
        reinterpret_cast<PVOID*>(&pollingThread),
        NULL);
    if (!NT_SUCCESS(status))
    {
        InterlockedExchange(&g_Running, FALSE);
        ZwWaitForSingleObject(threadHandle, FALSE, NULL);
        ZwClose(threadHandle);
        return status;
    }
    g_PollThread = pollingThread;
    KeMemoryBarrier();
    ZwClose(threadHandle);

    DbgPrint(
        "LksDriver: polling thread created; use kdmapper without --free\n");
    return STATUS_SUCCESS;
}
