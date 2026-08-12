// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#pragma once




#define LKS_SHARED_SIGNATURE       0x4C4B535Fu
#define LKS_PROTOCOL_VERSION       9u
#define LKS_SHARED_MEMORY_SIZE     0x10000u
#define LKS_MAX_BATCH_READS        128u
#define LKS_MAX_READ_SIZE          1024u
#define LKS_MAX_FRAME_PAYLOAD      0x6000u

#define LKS_SHARED_HEADER_SIZE     104u
#define LKS_RING_METADATA_SIZE     8u
#define LKS_RING_DATA_SIZE \
    (((LKS_SHARED_MEMORY_SIZE - LKS_SHARED_HEADER_SIZE) / 2u) - LKS_RING_METADATA_SIZE)

enum LKS_MESSAGE_TYPE : USHORT {
    LksMessageText              = 0x02,
    LksMessageMouseMove         = 0x04,
    LksMessageTargetThread      = 0x05,
    LksMessageReadMemory        = 0x06,
    LksMessageReadMemoryResult  = 0x07,
    LksMessageReadBatch         = 0x08,
    LksMessageReadBatchResult   = 0x09,
    LksMessageDriverStop        = 0x0A
};

enum LKS_DRIVER_STATE : LONG {
    LksDriverStopped  = 0,
    LksDriverRunning  = 1,
    LksDriverStopping = 2
};

enum LKS_MOUSE_STATUS : LONG {
    LksMouseIdle            = 0,
    LksMouseQueued          = 1,
    LksMouseDispatched      = 2,
    LksMouseNoTargetThread  = -1,
    LksMouseExportMissing   = -2,
    LksMouseAllocationFail  = -3,
    LksMouseInsertFail      = -4,
    LksMouseDispatchExcept  = -5,
    LksMouseDispatchReject  = -6,
    LksMouseRundown         = -7,
    LksMouseWrongIrql       = -8
};

#pragma pack(push, 1)

typedef struct _LKS_FRAME_HEADER {
    USHORT Type;
    USHORT PayloadSize;
    ULONG TransactionId;
} LKS_FRAME_HEADER, *PLKS_FRAME_HEADER;

typedef struct _LKS_RING_BUFFER {
    volatile ULONG WriteIndex;
    volatile ULONG ReadIndex;
    UCHAR Data[LKS_RING_DATA_SIZE];
} LKS_RING_BUFFER, *PLKS_RING_BUFFER;

typedef struct _LKS_SHARED_HEADER {
    ULONG Signature;
    USHORT Version;
    USHORT HeaderSize;
    ULONG TotalSize;
    ULONG TargetProcessId;
    volatile ULONG DriverHeartbeat;
    volatile LONG DriverState;
    volatile LONG LastMouseStatus;
    volatile ULONG MouseQueueCount;
    volatile ULONG MouseDispatchCount;
    volatile ULONG LastCompletedTransaction;
    volatile ULONG ResponseDropCount;
    volatile ULONG MouseTargetThreadId;
    volatile ULONG MouseApcNormalCount;
    volatile ULONG MouseApcRundownCount;
    volatile ULONG MouseLastIrql;
    volatile ULONG MouseTargetProcessId;
    volatile ULONG ActiveTransaction;
    volatile ULONG ActiveMessageType;
    volatile ULONG ActiveReadCount;
    volatile ULONG ActiveAddressLow;
    volatile ULONG ActiveAddressHigh;
    volatile ULONG ReadBudgetTimeoutCount;
    volatile ULONG ControllerProcessId;
    volatile ULONG ControllerAddressLow;
    volatile ULONG ControllerAddressHigh;
    volatile LONG ControllerMappingStatus;
} LKS_SHARED_HEADER, *PLKS_SHARED_HEADER;

typedef struct _LKS_SHARED_MEMORY {
    ULONG Signature;
    USHORT Version;
    USHORT HeaderSize;
    ULONG TotalSize;
    ULONG TargetProcessId;
    volatile ULONG DriverHeartbeat;
    volatile LONG DriverState;
    volatile LONG LastMouseStatus;
    volatile ULONG MouseQueueCount;
    volatile ULONG MouseDispatchCount;
    volatile ULONG LastCompletedTransaction;
    volatile ULONG ResponseDropCount;
    volatile ULONG MouseTargetThreadId;
    volatile ULONG MouseApcNormalCount;
    volatile ULONG MouseApcRundownCount;
    volatile ULONG MouseLastIrql;
    volatile ULONG MouseTargetProcessId;
    volatile ULONG ActiveTransaction;
    volatile ULONG ActiveMessageType;
    volatile ULONG ActiveReadCount;
    volatile ULONG ActiveAddressLow;
    volatile ULONG ActiveAddressHigh;
    volatile ULONG ReadBudgetTimeoutCount;
    volatile ULONG ControllerProcessId;
    volatile ULONG ControllerAddressLow;
    volatile ULONG ControllerAddressHigh;
    volatile LONG ControllerMappingStatus;
    LKS_RING_BUFFER Commands;
    LKS_RING_BUFFER Responses;
} LKS_SHARED_MEMORY, *PLKS_SHARED_MEMORY;

typedef struct _LKS_READ_REQUEST {
    ULONGLONG Address;
    ULONG Size;
    ULONG Reserved;
} LKS_READ_REQUEST, *PLKS_READ_REQUEST;

typedef struct _LKS_READ_RESPONSE {
    LONG Status;
    ULONG Size;
} LKS_READ_RESPONSE, *PLKS_READ_RESPONSE;

typedef struct _LKS_BATCH_REQUEST_HEADER {
    USHORT Count;
    USHORT Reserved;
} LKS_BATCH_REQUEST_HEADER, *PLKS_BATCH_REQUEST_HEADER;

typedef struct _LKS_BATCH_READ_ENTRY {
    ULONGLONG Address;
    USHORT Size;
    USHORT Reserved;
} LKS_BATCH_READ_ENTRY, *PLKS_BATCH_READ_ENTRY;

typedef struct _LKS_BATCH_RESPONSE_HEADER {
    USHORT Count;
    USHORT Reserved;
} LKS_BATCH_RESPONSE_HEADER, *PLKS_BATCH_RESPONSE_HEADER;

typedef struct _LKS_BATCH_READ_RESULT {
    LONG Status;
    USHORT Size;
    USHORT Reserved;
} LKS_BATCH_READ_RESULT, *PLKS_BATCH_READ_RESULT;

typedef struct _LKS_MOUSE_MOVE {
    LONG DeltaX;
    LONG DeltaY;
} LKS_MOUSE_MOVE, *PLKS_MOUSE_MOVE;

typedef struct _LKS_TARGET_THREAD {
    ULONG ProcessId;
    ULONG ThreadId;
    ULONGLONG InputBuffer;
} LKS_TARGET_THREAD, *PLKS_TARGET_THREAD;

#pragma pack(pop)

static_assert(sizeof(LKS_FRAME_HEADER) == 8, "Unexpected frame header size");
static_assert(sizeof(LKS_SHARED_HEADER) == LKS_SHARED_HEADER_SIZE, "Unexpected shared header size");
static_assert(sizeof(LKS_SHARED_MEMORY) == LKS_SHARED_MEMORY_SIZE, "Unexpected shared memory size");
static_assert(sizeof(LKS_BATCH_READ_ENTRY) == 12, "Unexpected batch entry size");
