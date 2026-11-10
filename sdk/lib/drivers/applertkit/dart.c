/*
 * PROJECT:     LiberNT Apple RTKit Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Stream setup of Apple silicon DART IOMMUs
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#ifndef UNIT_TEST
#include <ntddk.h>
#endif
#include <reactos/drivers/appledart.h>

#define DART_PARAMS1                0x00
#define DART_PARAMS2                0x04
#define DART_PARAMS2_BYPASS         0x1
#define DART_T8110_PARAMS3          0x08
#define DART_T8110_PARAMS4          0x0c
#define DART_T8110_TLB_CMD          0x80
#define DART_T8110_TLB_CMD_BUSY     0x80000000
#define DART_T8110_TLB_CMD_FLUSH    0x00000100
#define DART_T8110_PROTECT          0x200
#define DART_T8110_PROTECT_TCR      0x1
#define DART_T8110_ENABLE_STREAMS   0xc00
#define DART_T8110_TCR              0x1000
#define DART_T8110_TCR_TRANSLATE    0x1
#define DART_T8110_TCR_BYPASS       0x6
#define DART_T8110_TCR_FOUR_LEVEL   0x8
#define DART_T8110_TTBR             0x1400
#define DART_T8110_TTBR_VALID       0x1

#define DART_T8020_STREAM_COMMAND   0x20
#define DART_T8020_STREAM_COMMAND_BUSY 0x00000004
#define DART_T8020_STREAM_COMMAND_INVALIDATE 0x00100000
#define DART_T8020_STREAM_SELECT    0x34
#define DART_T8020_CONFIG           0x60
#define DART_T8020_CONFIG_LOCK      0x00008000
#define DART_T8020_ENABLE_STREAMS   0xfc
#define DART_T8020_TCR              0x100
#define DART_T8020_TCR_TRANSLATE    0x00000080
#define DART_T8020_TCR_BYPASS       0x00001100
#define DART_T8020_TTBR             0x200
#define DART_T8020_TTBR_VALID       0x80000000
#define DART_T8020_TTBR_ADDRESS     0x7fffffff
#define DART_T8020_TTBR_SHIFT       12
#define DART_T8020_TTBR_COUNT       4
#define DART_T8020_STREAMS          16
#define DART_T8020_INPUT_BITS       32
#define DART_T8020_OUTPUT_BITS      42

#define DART_PAGE_SHIFT             14
#define DART_PAGE_SIZE              (1ULL << DART_PAGE_SHIFT)
#define DART_LEVEL_BITS             11
#define DART_TABLE_ENTRIES          (1UL << DART_LEVEL_BITS)
#define DART_PTE_VALID              0x1ULL
#define DART_PTE_SUBPAGE_END        (0xfffULL << 40)
#define DART_PTE_ADDRESS_MASK       0x0000003ffffffc00ULL
#define DART_TAG                    'tDpA'
#define DART_LOW_WINDOW_BASE        0x01000000ULL
#define DART_LOW_WINDOW_END         0xc0000000ULL

typedef struct _DART_TABLE
{
    PULONG64 Entries;
    struct _DART_TABLE **Children;
} DART_TABLE, *PDART_TABLE;

typedef struct _DART_IDENTITY
{
    ULONG InputBits;
    ULONG Levels;
    PDART_TABLE Top;
    ULONG64 TopPhysical;
} DART_IDENTITY, *PDART_IDENTITY;

typedef enum _DART_FAMILY
{
    DartFamilyNone,
    DartFamilyT8110,
    DartFamilyT6000
} DART_FAMILY;

typedef struct _DART_HW
{
    DART_FAMILY Family;
    PUCHAR Base;
    ULONG Size;
    ULONG64 Physical;
    ULONG Params2;
    ULONG Params3;
    ULONG Streams;
    ULONG InputBits;
    ULONG OutputBits;
    BOOLEAN Locked;
} DART_HW, *PDART_HW;

static DART_IDENTITY AppleDartIdentity;
static DART_IDENTITY AppleDartWindow32;
static volatile LONG AppleDartIdentityBusy;
static ULONG64 AppleDartLowNext = DART_LOW_WINDOW_BASE;
static ULONG64 AppleDartLowNext32 = DART_LOW_WINDOW_BASE;

static
ULONG
AppleDartRead(
    _In_ PUCHAR Base,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Offset));
}

static
VOID
AppleDartWrite(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Offset), Value);
}

static
DART_FAMILY
AppleDartFamily(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Dart)
{
    const VOID *Compatible;
    ULONG Length;

    Compatible = FdtGetProperty(&Tree->Fdt, Dart, "compatible", &Length);
    /* A node naming both takes the first: the t6000 layout. */
    if (FdtStringListContains(Compatible, Length, "apple,t6000-dart"))
        return DartFamilyT6000;
    if (FdtStringListContains(Compatible, Length, "apple,t8110-dart"))
        return DartFamilyT8110;
    return DartFamilyNone;
}

static
NTSTATUS
AppleDartOpen(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Dart,
    _In_z_ PCSTR Owner,
    _Out_ PDART_HW Hw)
{
    RtlZeroMemory(Hw, sizeof(*Hw));
    Hw->Family = AppleDartFamily(Tree, Dart);
    if (Hw->Family == DartFamilyNone)
    {
        DbgPrint("%s: DART %lx is neither a t8110 nor a t6000 DART\n", Owner, Dart);
        return STATUS_NOT_SUPPORTED;
    }
    Hw->Base = FdtDriverMapReg(Tree, Dart, 0, &Hw->Physical, &Hw->Size);
    if (!Hw->Base)
        return STATUS_INSUFFICIENT_RESOURCES;
    Hw->Params2 = AppleDartRead(Hw->Base, DART_PARAMS2);
    if (Hw->Family == DartFamilyT8110)
    {
        Hw->Params3 = AppleDartRead(Hw->Base, DART_T8110_PARAMS3);
        Hw->Streams = AppleDartRead(Hw->Base, DART_T8110_PARAMS4) & 0x1ff;
        Hw->InputBits = (Hw->Params3 >> 16) & 0x3f;
        Hw->OutputBits = (Hw->Params3 >> 24) & 0x3f;
        Hw->Locked = (AppleDartRead(Hw->Base, DART_T8110_PROTECT) & DART_T8110_PROTECT_TCR) != 0;
    }
    else
    {
        Hw->Streams = DART_T8020_STREAMS;
        Hw->InputBits = DART_T8020_INPUT_BITS;
        Hw->OutputBits = DART_T8020_OUTPUT_BITS;
        Hw->Locked = (AppleDartRead(Hw->Base, DART_T8020_CONFIG) & DART_T8020_CONFIG_LOCK) != 0;
    }
    return STATUS_SUCCESS;
}

static
VOID
AppleDartClose(
    _Inout_ PDART_HW Hw)
{
    if (Hw->Base)
        MmUnmapIoSpace(Hw->Base, Hw->Size);
    Hw->Base = NULL;
}

static
ULONG
AppleDartTcrOffset(
    _In_ DART_FAMILY Family,
    _In_ ULONG Sid)
{
    return (Family == DartFamilyT6000 ? DART_T8020_TCR : DART_T8110_TCR) + 4 * Sid;
}

static
ULONG
AppleDartTtbrOffset(
    _In_ DART_FAMILY Family,
    _In_ ULONG Sid)
{
    return Family == DartFamilyT6000 ? DART_T8020_TTBR + 4 * DART_T8020_TTBR_COUNT * Sid : DART_T8110_TTBR + 4 * Sid;
}

static
ULONG
AppleDartTcrBypass(
    _In_ DART_FAMILY Family)
{
    return Family == DartFamilyT6000 ? DART_T8020_TCR_BYPASS : DART_T8110_TCR_BYPASS;
}

static
ULONG
AppleDartTcrTranslate(
    _In_ DART_FAMILY Family,
    _In_ ULONG Levels)
{
    if (Family == DartFamilyT6000)
        return DART_T8020_TCR_TRANSLATE;
    return DART_T8110_TCR_TRANSLATE | (Levels == 3 ? DART_T8110_TCR_FOUR_LEVEL : 0);
}

static
BOOLEAN
AppleDartTcrTranslates(
    _In_ DART_FAMILY Family,
    _In_ ULONG Tcr)
{
    ULONG Bypass = AppleDartTcrBypass(Family);

    if ((Tcr & Bypass) == Bypass)
        return FALSE;
    return (Tcr & (Family == DartFamilyT6000 ? DART_T8020_TCR_TRANSLATE : DART_T8110_TCR_TRANSLATE)) != 0;
}

static
ULONG
AppleDartTtbr(
    _In_ DART_FAMILY Family,
    _In_ ULONG64 TablePhysical)
{
    if (Family == DartFamilyT6000)
        return (ULONG)((TablePhysical >> DART_T8020_TTBR_SHIFT) & DART_T8020_TTBR_ADDRESS) | DART_T8020_TTBR_VALID;
    return (ULONG)((TablePhysical >> DART_PAGE_SHIFT) << 2) | DART_T8110_TTBR_VALID;
}

static
BOOLEAN
AppleDartTtbrTable(
    _In_ DART_FAMILY Family,
    _In_ ULONG Ttbr,
    _Out_ PULONG64 TablePhysical)
{
    if (Family == DartFamilyT6000)
    {
        *TablePhysical = (ULONG64)(Ttbr & DART_T8020_TTBR_ADDRESS) << DART_T8020_TTBR_SHIFT;
        return (Ttbr & DART_T8020_TTBR_VALID) != 0;
    }
    *TablePhysical = (ULONG64)(Ttbr >> 2) << DART_PAGE_SHIFT;
    return (Ttbr & DART_T8110_TTBR_VALID) != 0;
}

static
VOID
AppleDartEnableStream(
    _In_ PUCHAR Base,
    _In_ DART_FAMILY Family,
    _In_ ULONG Sid)
{
    ULONG Offset = Family == DartFamilyT6000 ? DART_T8020_ENABLE_STREAMS : DART_T8110_ENABLE_STREAMS + 4 * (Sid >> 5);

    AppleDartWrite(Base, Offset, AppleDartRead(Base, Offset) | (1UL << (Sid & 31)));
}

static
ULONG64
AppleDartEntry(
    _In_ ULONG64 Physical)
{
    return (Physical >> 4) & DART_PTE_ADDRESS_MASK;
}

static
PDART_TABLE
AppleDartAllocateTable(
    _In_ BOOLEAN Leaf,
    _Out_ PULONG64 Physical)
{
    PHYSICAL_ADDRESS Low, High, Boundary;
    PDART_TABLE Table;

    Table = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Table), DART_TAG);
    if (!Table)
        return NULL;
    Table->Children = NULL;
    if (!Leaf)
    {
        Table->Children = ExAllocatePoolWithTag(NonPagedPool, DART_TABLE_ENTRIES * sizeof(PDART_TABLE), DART_TAG);
        if (!Table->Children)
        {
            ExFreePoolWithTag(Table, DART_TAG);
            return NULL;
        }
        RtlZeroMemory(Table->Children, DART_TABLE_ENTRIES * sizeof(PDART_TABLE));
    }
    Low.QuadPart = 0;
    High.QuadPart = -1;
    Boundary.QuadPart = DART_PAGE_SIZE;
    Table->Entries = MmAllocateContiguousMemorySpecifyCache(DART_PAGE_SIZE, Low, High, Boundary, MmCached);
    if (!Table->Entries)
    {
        if (Table->Children)
            ExFreePoolWithTag(Table->Children, DART_TAG);
        ExFreePoolWithTag(Table, DART_TAG);
        return NULL;
    }
    RtlZeroMemory(Table->Entries, DART_PAGE_SIZE);
    *Physical = (ULONG64)MmGetPhysicalAddress(Table->Entries).QuadPart;
    return Table;
}

static
NTSTATUS
AppleDartMapPage(
    _Inout_ PDART_IDENTITY Identity,
    _In_ ULONG64 Iova,
    _In_ ULONG64 Physical)
{
    PDART_TABLE Table = Identity->Top, Child;
    ULONG64 TablePhysical;
    ULONG Level, Slot;

    for (Level = Identity->Levels; Level > 1; --Level)
    {
        Slot = (ULONG)(Iova >> (DART_PAGE_SHIFT + (Level - 1) * DART_LEVEL_BITS)) & (DART_TABLE_ENTRIES - 1);
        Child = Table->Children[Slot];
        if (!Child)
        {
            Child = AppleDartAllocateTable(Level == 2, &TablePhysical);
            if (!Child)
                return STATUS_INSUFFICIENT_RESOURCES;
            Table->Children[Slot] = Child;
            Table->Entries[Slot] = AppleDartEntry(TablePhysical) | DART_PTE_VALID;
        }
        Table = Child;
    }
    Table->Entries[(Iova >> DART_PAGE_SHIFT) & (DART_TABLE_ENTRIES - 1)] =
        AppleDartEntry(Physical) | DART_PTE_SUBPAGE_END | DART_PTE_VALID;
    return STATUS_SUCCESS;
}

static
BOOLEAN
AppleDartUnmapPage(
    _Inout_ PDART_IDENTITY Identity,
    _In_ ULONG64 Iova)
{
    PDART_TABLE Table = Identity->Top;
    ULONG Level, Slot;

    for (Level = Identity->Levels; Table && Level > 1; --Level)
    {
        Slot = (ULONG)(Iova >> (DART_PAGE_SHIFT + (Level - 1) * DART_LEVEL_BITS)) & (DART_TABLE_ENTRIES - 1);
        Table = Table->Children[Slot];
    }
    if (!Table)
        return FALSE;
    Slot = (ULONG)(Iova >> DART_PAGE_SHIFT) & (DART_TABLE_ENTRIES - 1);
    if (!(Table->Entries[Slot] & DART_PTE_VALID))
        return FALSE;
    Table->Entries[Slot] = 0;
    return TRUE;
}

static
NTSTATUS
AppleDartBuildIdentity(
    _Inout_ PDART_IDENTITY Identity,
    _In_ ULONG InputBits,
    _In_ ULONG OutputBits)
{
    PPHYSICAL_MEMORY_RANGE Ranges;
    ULONG64 Mask, Page, End, Lowest = MAXULONG64, Highest = 0;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    if (InputBits < DART_PAGE_SHIFT + DART_LEVEL_BITS + 1 || InputBits > DART_PAGE_SHIFT + 3 * DART_LEVEL_BITS)
    {
        DbgPrint("DART: %lu-bit device addresses are outside the supported table depths\n", InputBits);
        return STATUS_NOT_SUPPORTED;
    }
    Ranges = MmGetPhysicalMemoryRanges();
    if (!Ranges)
    {
        DbgPrint("DART: no physical memory ranges\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (Index = 0; Ranges[Index].NumberOfBytes.QuadPart; ++Index)
    {
        Lowest = min(Lowest, (ULONG64)Ranges[Index].BaseAddress.QuadPart);
        Highest = max(Highest, (ULONG64)Ranges[Index].BaseAddress.QuadPart + Ranges[Index].NumberOfBytes.QuadPart);
    }
    Mask = (1ULL << InputBits) - 1;
    if (!Index || Highest > (1ULL << min(OutputBits, 42)) || ((Lowest ^ (Highest - 1)) & ~Mask))
    {
        DbgPrint("DART: memory %I64x-%I64x in %lu ranges does not fit %lu-bit input and %lu-bit output addresses\n",
                 Lowest, Highest, Index, InputBits, OutputBits);
        Status = STATUS_NOT_SUPPORTED;
        goto Done;
    }
    Identity->InputBits = InputBits;
    Identity->Levels = (InputBits > DART_PAGE_SHIFT + 2 * DART_LEVEL_BITS) ? 3 : 2;
    Identity->Top = AppleDartAllocateTable(FALSE, &Identity->TopPhysical);
    if (!Identity->Top)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    for (Index = 0; Ranges[Index].NumberOfBytes.QuadPart; ++Index)
    {
        Page = (ULONG64)Ranges[Index].BaseAddress.QuadPart & ~(DART_PAGE_SIZE - 1);
        End = (ULONG64)Ranges[Index].BaseAddress.QuadPart + Ranges[Index].NumberOfBytes.QuadPart;
        for (; Page < End; Page += DART_PAGE_SIZE)
        {
            Status = AppleDartMapPage(Identity, Page & Mask, Page);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint("DART: identity table for page %I64x not allocated: %lx\n", Page, Status);
                goto Done;
            }
        }
    }
    KeMemoryBarrier();

Done:
    ExFreePool(Ranges);
    return Status;
}

static
VOID
AppleDartFlush(
    _In_ PUCHAR Base,
    _In_ DART_FAMILY Family,
    _In_ ULONG Sid,
    _In_z_ PCSTR Owner)
{
    ULONG Spin;

    KeMemoryBarrier();
    if (Family == DartFamilyT6000)
    {
        AppleDartWrite(Base, DART_T8020_STREAM_SELECT, 1UL << Sid);
        AppleDartWrite(Base, DART_T8020_STREAM_COMMAND, DART_T8020_STREAM_COMMAND_INVALIDATE);
        for (Spin = 0; Spin < 1000 && (AppleDartRead(Base, DART_T8020_STREAM_COMMAND) & DART_T8020_STREAM_COMMAND_BUSY);
             ++Spin)
        {
            KeStallExecutionProcessor(1);
        }
    }
    else
    {
        AppleDartWrite(Base, DART_T8110_TLB_CMD, DART_T8110_TLB_CMD_FLUSH | Sid);
        for (Spin = 0; Spin < 1000 && (AppleDartRead(Base, DART_T8110_TLB_CMD) & DART_T8110_TLB_CMD_BUSY); ++Spin)
            KeStallExecutionProcessor(1);
    }
    if (Spin == 1000)
        DbgPrint("%s: DART TLB flush did not complete\n", Owner);
}

static
NTSTATUS
AppleDartBuildWindow(
    _Inout_ PDART_IDENTITY Identity,
    _In_ ULONG InputBits)
{
    ASSERT(InputBits <= DART_PAGE_SHIFT + 2 * DART_LEVEL_BITS);
    Identity->InputBits = InputBits;
    Identity->Levels = 2;
    Identity->Top = AppleDartAllocateTable(FALSE, &Identity->TopPhysical);
    if (!Identity->Top)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Identity->TopPhysical >> (DART_T8020_TTBR_SHIFT + 31))
    {
        DbgPrint("DART: table at %I64x is beyond what a t6000 TTBR reaches\n", Identity->TopPhysical);
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

static
PDART_IDENTITY
AppleDartTables(
    _In_ DART_FAMILY Family)
{
    return Family == DartFamilyT6000 ? &AppleDartWindow32 : &AppleDartIdentity;
}

static
NTSTATUS
AppleDartPrepareTables(
    _In_ PDART_HW Hw,
    _In_z_ PCSTR Owner)
{
    PDART_IDENTITY Tables = AppleDartTables(Hw->Family);
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(AppleDartIdentityBusy == 1);
    ASSERT(Hw->Family != DartFamilyNone);
    if (!Tables->Top)
    {
        if (Hw->Family == DartFamilyT6000)
            Status = AppleDartBuildWindow(Tables, Hw->InputBits);
        else
            Status = AppleDartBuildIdentity(Tables, Hw->InputBits, Hw->OutputBits);
        DbgPrint("%s: DART %s tables for %lu-bit addresses, %lu levels: %lx\n", Owner,
                 Hw->Family == DartFamilyT6000 ? "low window" : "identity", Hw->InputBits, Tables->Levels, Status);
    }
    else if (Tables->InputBits != Hw->InputBits)
    {
        Status = STATUS_NOT_SUPPORTED;
    }
    if (NT_SUCCESS(Status) && !Tables->Top)
        Status = STATUS_INSUFFICIENT_RESOURCES;
    return Status;
}

static
VOID
AppleDartTranslateStream(
    _In_ PDART_HW Hw,
    _In_ ULONG Sid,
    _In_z_ PCSTR Owner)
{
    PDART_IDENTITY Tables = AppleDartTables(Hw->Family);
    ULONG TcrOffset = AppleDartTcrOffset(Hw->Family, Sid), TtbrOffset = AppleDartTtbrOffset(Hw->Family, Sid);
    ULONG Ttbr = AppleDartTtbr(Hw->Family, Tables->TopPhysical);
    ULONG Tcr = AppleDartTcrTranslate(Hw->Family, Tables->Levels);
    ULONG Index;

    ASSERT(Sid < Hw->Streams);
    ASSERT(Tables->Top);
    if (AppleDartRead(Hw->Base, TcrOffset) == Tcr && AppleDartRead(Hw->Base, TtbrOffset) == Ttbr)
    {
        AppleDartFlush(Hw->Base, Hw->Family, Sid, Owner);
        return;
    }
    AppleDartWrite(Hw->Base, TcrOffset, 0);
    AppleDartWrite(Hw->Base, TtbrOffset, Ttbr);
    if (Hw->Family == DartFamilyT6000)
    {
        for (Index = 1; Index < DART_T8020_TTBR_COUNT; ++Index)
            AppleDartWrite(Hw->Base, TtbrOffset + 4 * Index, 0);
    }
    AppleDartEnableStream(Hw->Base, Hw->Family, Sid);
    AppleDartFlush(Hw->Base, Hw->Family, Sid, Owner);
    AppleDartWrite(Hw->Base, TcrOffset, Tcr);
}

static
NTSTATUS
AppleDartIdentityStream(
    _In_ PDART_HW Hw,
    _In_ ULONG Sid,
    _In_z_ PCSTR Owner)
{
    NTSTATUS Status;

    if (InterlockedCompareExchange(&AppleDartIdentityBusy, 1, 0) != 0)
        return STATUS_DEVICE_BUSY;
    Status = AppleDartPrepareTables(Hw, Owner);
    InterlockedExchange(&AppleDartIdentityBusy, 0);
    if (!NT_SUCCESS(Status))
        return Status;
    AppleDartTranslateStream(Hw, Sid, Owner);
    return STATUS_SUCCESS;
}

static
VOID
AppleDartBypassStream(
    _In_ PDART_HW Hw,
    _In_ ULONG Sid,
    _In_z_ PCSTR Owner)
{
    ULONG TtbrOffset = AppleDartTtbrOffset(Hw->Family, Sid);
    ULONG Index, Count = Hw->Family == DartFamilyT6000 ? DART_T8020_TTBR_COUNT : 1;

    AppleDartWrite(Hw->Base, AppleDartTcrOffset(Hw->Family, Sid), AppleDartTcrBypass(Hw->Family));
    for (Index = 0; Index < Count; ++Index)
        AppleDartWrite(Hw->Base, TtbrOffset + 4 * Index, 0);
    AppleDartEnableStream(Hw->Base, Hw->Family, Sid);
    AppleDartFlush(Hw->Base, Hw->Family, Sid, Owner);
}

static
NTSTATUS
AppleDartPhysicalStream(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Dart,
    _In_ ULONG Sid,
    _In_z_ PCSTR Owner)
{
    ULONG Bypass;
    DART_HW Hw;
    NTSTATUS Status;

    Status = AppleDartOpen(Tree, Dart, Owner, &Hw);
    if (!NT_SUCCESS(Status))
        return Status;
    Bypass = AppleDartTcrBypass(Hw.Family);
    DbgPrint("%s: DART %I64x (%s) stream %lu: params %08lx %08lx %08lx, %lu streams, %slocked, tcr %08lx ttbr %08lx\n",
             Owner, Hw.Physical, Hw.Family == DartFamilyT6000 ? "t6000" : "t8110", Sid,
             AppleDartRead(Hw.Base, DART_PARAMS1), Hw.Params2, Hw.Params3, Hw.Streams, Hw.Locked ? "" : "not ",
             Sid < Hw.Streams ? AppleDartRead(Hw.Base, AppleDartTcrOffset(Hw.Family, Sid)) : 0,
             Sid < Hw.Streams ? AppleDartRead(Hw.Base, AppleDartTtbrOffset(Hw.Family, Sid)) : 0);
    if (Sid >= Hw.Streams)
    {
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    else if (Hw.Locked)
    {
        if ((AppleDartRead(Hw.Base, AppleDartTcrOffset(Hw.Family, Sid)) & Bypass) != Bypass)
        {
            DbgPrint("%s: DART stream %lu is locked by the firmware without bypass\n", Owner, Sid);
            Status = STATUS_ACCESS_DENIED;
        }
    }
    else if (Hw.Params2 & DART_PARAMS2_BYPASS)
    {
        AppleDartBypassStream(&Hw, Sid, Owner);
    }
    else if (Hw.Family == DartFamilyT6000)
    {
        /* Memory starts at 1 TB, beyond 32-bit device addresses: such a stream takes low mappings only. */
        DbgPrint("%s: DART stream %lu has no bypass and 32-bit device addresses; use low mappings\n", Owner, Sid);
        Status = STATUS_NOT_SUPPORTED;
    }
    else
    {
        Status = AppleDartIdentityStream(&Hw, Sid, Owner);
    }
    AppleDartClose(&Hw);
    return Status;
}

NTSTATUS
NTAPI
AppleDartMapStream(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Dart,
    _In_ ULONG Sid,
    _In_z_ PCSTR Owner)
{
    ULONG Cells;

    if (Dart == FDT_NO_NODE || !FdtReadU32(&Tree->Fdt, Dart, "#iommu-cells", &Cells) || Cells != 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return AppleDartPhysicalStream(Tree, Dart, Sid, Owner);
}

NTSTATUS
NTAPI
AppleDartMapPhysical(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Node,
    _In_z_ PCSTR Owner)
{
    const VOID *Iommus;
    ULONG Length, Index, Dart, Cells = 0;
    ULONG64 Phandle, Sid;
    NTSTATUS Status;

    Iommus = FdtGetProperty(&Tree->Fdt, Node, "iommus", &Length);
    if (!Iommus)
        return STATUS_SUCCESS;
    for (Index = 0; FdtReadCells(Iommus, Length, Index, 1, &Phandle); Index += 1 + Cells)
    {
        Dart = FdtDriverFindPhandle(Tree, (ULONG)Phandle);
        if (Dart == FDT_NO_NODE || !FdtReadU32(&Tree->Fdt, Dart, "#iommu-cells", &Cells) || Cells != 1 ||
            !FdtReadCells(Iommus, Length, Index + 1, 1, &Sid))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        Status = AppleDartPhysicalStream(Tree, Dart, (ULONG)Sid, Owner);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
AppleDartMapLow(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Dart,
    _In_ ULONG Sid,
    _In_ ULONG64 Physical,
    _In_ SIZE_T Length,
    _Out_ PULONG64 Iova,
    _In_z_ PCSTR Owner)
{
    ULONG Cells;
    ULONG64 First, Last, Window, Page;
    PULONG64 Next;
    PDART_IDENTITY Tables;
    DART_HW Hw;
    NTSTATUS Status;

    *Iova = 0;
    if (Dart == FDT_NO_NODE || !Length || !FdtReadU32(&Tree->Fdt, Dart, "#iommu-cells", &Cells) || Cells != 1)
        return STATUS_INVALID_PARAMETER;
    if (AppleDartFamily(Tree, Dart) == DartFamilyNone)
        return STATUS_NOT_SUPPORTED;
    Status = AppleDartOpen(Tree, Dart, Owner, &Hw);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Sid >= Hw.Streams)
        Status = STATUS_INVALID_PARAMETER;
    else if (Hw.Locked)
        Status = STATUS_ACCESS_DENIED;
    else if (InterlockedCompareExchange(&AppleDartIdentityBusy, 1, 0) != 0)
        Status = STATUS_DEVICE_BUSY;
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_DEVICE_BUSY)
            DbgPrint("%s: DART stream %lu of %lu cannot take a low mapping: %lx\n", Owner, Sid, Hw.Streams, Status);
        AppleDartClose(&Hw);
        return Status;
    }

    Status = AppleDartPrepareTables(&Hw, Owner);
    Tables = AppleDartTables(Hw.Family);
    Next = Hw.Family == DartFamilyT6000 ? &AppleDartLowNext32 : &AppleDartLowNext;
    First = Physical & ~(DART_PAGE_SIZE - 1);
    Last = (Physical + Length + DART_PAGE_SIZE - 1) & ~(DART_PAGE_SIZE - 1);
    Window = *Next;
    if (NT_SUCCESS(Status) && (Last <= First || Window + (Last - First) > DART_LOW_WINDOW_END))
        Status = STATUS_INSUFFICIENT_RESOURCES;
    for (Page = First; NT_SUCCESS(Status) && Page < Last; Page += DART_PAGE_SIZE)
        Status = AppleDartMapPage(Tables, Window + (Page - First), Page);
    if (NT_SUCCESS(Status))
        *Next = Window + (Last - First);
    KeMemoryBarrier();
    InterlockedExchange(&AppleDartIdentityBusy, 0);

    if (NT_SUCCESS(Status))
    {
        AppleDartTranslateStream(&Hw, Sid, Owner);
        *Iova = Window + (Physical - First);
        DbgPrint("%s: DART stream %lu maps %I64x+%Ix at device address %I64x\n", Owner, Sid, Physical, Length, *Iova);
    }
    else
    {
        DbgPrint("%s: DART stream %lu low mapping of %I64x+%Ix failed: %lx\n", Owner, Sid, Physical, Length, Status);
    }
    AppleDartClose(&Hw);
    return Status;
}

NTSTATUS
NTAPI
AppleDartUnmapLow(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Dart,
    _In_ ULONG Sid,
    _In_ ULONG64 Iova,
    _In_ SIZE_T Length,
    _In_z_ PCSTR Owner)
{
    ULONG Cells;
    ULONG64 First, Last, Page;
    DART_HW Hw;
    NTSTATUS Status;

    if (Dart == FDT_NO_NODE || !Length || Iova < DART_LOW_WINDOW_BASE || Iova + Length < Iova ||
        Iova + Length > DART_LOW_WINDOW_END || !FdtReadU32(&Tree->Fdt, Dart, "#iommu-cells", &Cells) || Cells != 1)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (AppleDartFamily(Tree, Dart) == DartFamilyNone)
        return STATUS_NOT_SUPPORTED;
    Status = AppleDartOpen(Tree, Dart, Owner, &Hw);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Sid >= Hw.Streams)
        Status = STATUS_INVALID_PARAMETER;
    else if (InterlockedCompareExchange(&AppleDartIdentityBusy, 1, 0) != 0)
        Status = STATUS_DEVICE_BUSY;
    if (!NT_SUCCESS(Status))
    {
        AppleDartClose(&Hw);
        return Status;
    }

    First = Iova & ~(DART_PAGE_SIZE - 1);
    Last = (Iova + Length + DART_PAGE_SIZE - 1) & ~(DART_PAGE_SIZE - 1);
    for (Page = First; Page < Last; Page += DART_PAGE_SIZE)
    {
        if (!AppleDartUnmapPage(AppleDartTables(Hw.Family), Page))
            Status = STATUS_NOT_FOUND;
    }
    KeMemoryBarrier();
    InterlockedExchange(&AppleDartIdentityBusy, 0);
    AppleDartFlush(Hw.Base, Hw.Family, Sid, Owner);
    AppleDartClose(&Hw);
    return Status;
}

static
PULONG64
AppleDartAdoptedTable(
    _Inout_ PAPPLE_DART_ADOPTED Adopted,
    _In_ ULONG64 Physical,
    _Out_ PBOOLEAN Mapped)
{
    PHYSICAL_ADDRESS Address;
    ULONG Index;

    PULONG64 Table;

    *Mapped = FALSE;
    for (Index = 0; Index < Adopted->OwnTables; ++Index)
    {
        if (Adopted->OwnPhysical[Index] == Physical)
            return Adopted->OwnVirtual[Index];
    }
    for (Index = 0; Index < Adopted->ForeignTables; ++Index)
    {
        if (Adopted->ForeignPhysical[Index] == Physical)
            return Adopted->ForeignVirtual[Index];
    }
    Address.QuadPart = (LONGLONG)Physical;
    Table = MmMapIoSpace(Address, DART_PAGE_SIZE, MmCached);
    if (Table && Adopted->ForeignTables < RTL_NUMBER_OF(Adopted->ForeignPhysical))
    {
        Adopted->ForeignPhysical[Adopted->ForeignTables] = Physical;
        Adopted->ForeignVirtual[Adopted->ForeignTables] = Table;
        Adopted->ForeignTables++;
        return Table;
    }
    *Mapped = Table != NULL;
    return Table;
}

static
VOID
AppleDartAdoptedTableDone(
    _In_ PULONG64 Table,
    _In_ BOOLEAN Mapped)
{
    if (Table && Mapped)
        MmUnmapIoSpace(Table, DART_PAGE_SIZE);
}

static
ULONG64
AppleDartAdoptedWalk(
    _Inout_ PAPPLE_DART_ADOPTED Adopted,
    _In_ ULONG64 Iova,
    _In_ BOOLEAN Create)
{
    ULONG64 TablePhysical = Adopted->TopPhysical, Entry, ChildPhysical;
    PHYSICAL_ADDRESS Low, High, Boundary;
    PULONG64 Table, Child;
    BOOLEAN Mapped;
    ULONG Level, Slot;

    for (Level = Adopted->Levels; Level > 1; --Level)
    {
        Slot = (ULONG)(Iova >> (DART_PAGE_SHIFT + (Level - 1) * DART_LEVEL_BITS)) & (DART_TABLE_ENTRIES - 1);
        Table = AppleDartAdoptedTable(Adopted, TablePhysical, &Mapped);
        if (!Table)
            return 0;
        Entry = Table[Slot];
        if (!(Entry & DART_PTE_VALID))
        {
            if (!Create || Adopted->OwnTables == RTL_NUMBER_OF(Adopted->OwnPhysical))
            {
                AppleDartAdoptedTableDone(Table, Mapped);
                return 0;
            }
            Low.QuadPart = 0;
            High.QuadPart = -1;
            Boundary.QuadPart = DART_PAGE_SIZE;
            Child = MmAllocateContiguousMemorySpecifyCache(DART_PAGE_SIZE, Low, High, Boundary, MmCached);
            if (!Child)
            {
                AppleDartAdoptedTableDone(Table, Mapped);
                return 0;
            }
            RtlZeroMemory(Child, DART_PAGE_SIZE);
            ChildPhysical = (ULONG64)MmGetPhysicalAddress(Child).QuadPart;
            Adopted->OwnPhysical[Adopted->OwnTables] = ChildPhysical;
            Adopted->OwnVirtual[Adopted->OwnTables] = Child;
            Adopted->OwnTables++;
            KeMemoryBarrier();
            Entry = AppleDartEntry(ChildPhysical) | DART_PTE_VALID;
            Table[Slot] = Entry;
        }
        AppleDartAdoptedTableDone(Table, Mapped);
        TablePhysical = (Entry & DART_PTE_ADDRESS_MASK) << 4;
    }
    return TablePhysical;
}

NTSTATUS
NTAPI
AppleDartAdopt(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Dart,
    _In_ ULONG Sid,
    _In_ ULONG64 WindowBase,
    _In_ ULONG64 WindowEnd,
    _In_z_ PCSTR Owner,
    _Out_ PAPPLE_DART_ADOPTED Adopted)
{
    ULONG Cells, Tcr, Ttbr;
    ULONG64 Top;
    DART_HW Hw;
    NTSTATUS Status;

    RtlZeroMemory(Adopted, sizeof(*Adopted));
    if (Dart == FDT_NO_NODE || !FdtReadU32(&Tree->Fdt, Dart, "#iommu-cells", &Cells) || Cells != 1 ||
        WindowEnd <= WindowBase || (WindowBase & (DART_PAGE_SIZE - 1)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (AppleDartFamily(Tree, Dart) == DartFamilyNone)
        return STATUS_NOT_SUPPORTED;
    Status = AppleDartOpen(Tree, Dart, Owner, &Hw);
    if (!NT_SUCCESS(Status))
        return Status;
    Tcr = Sid < Hw.Streams ? AppleDartRead(Hw.Base, AppleDartTcrOffset(Hw.Family, Sid)) : 0;
    Ttbr = Sid < Hw.Streams ? AppleDartRead(Hw.Base, AppleDartTtbrOffset(Hw.Family, Sid)) : 0;
    DbgPrint("%s: DART %I64x (%s) stream %lu of %lu left by the firmware: tcr %08lx ttbr %08lx params %08lx %slocked\n",
             Owner, Hw.Physical, Hw.Family == DartFamilyT6000 ? "t6000" : "t8110", Sid, Hw.Streams, Tcr, Ttbr,
             Hw.Params3, Hw.Locked ? "" : "not ");
    if (Sid >= Hw.Streams || !AppleDartTcrTranslates(Hw.Family, Tcr) || !AppleDartTtbrTable(Hw.Family, Ttbr, &Top))
    {
        AppleDartClose(&Hw);
        return STATUS_DEVICE_NOT_READY;
    }
    Adopted->Base = Hw.Base;
    Adopted->BaseSize = Hw.Size;
    Adopted->Family = (ULONG)Hw.Family;
    Adopted->Sid = Sid;
    if (Hw.Family == DartFamilyT6000)
        Adopted->Levels = 2;
    else
        Adopted->Levels = (Tcr & DART_T8110_TCR_FOUR_LEVEL) ? 3 : 2;
    Adopted->InputBits = Hw.InputBits;
    Adopted->TopPhysical = Top;
    Adopted->NextIova = WindowBase;
    Adopted->EndIova = WindowEnd;
    Adopted->Owner = Owner;
    /* The tables index only the levels the stream has; a device address above them has no slot. */
    if (WindowEnd - 1 >= (1ULL << min(Adopted->InputBits, DART_PAGE_SHIFT + Adopted->Levels * DART_LEVEL_BITS)))
    {
        DbgPrint("%s: DART window %I64x-%I64x is beyond %lu-bit addresses with %lu levels\n", Owner, WindowBase,
                 WindowEnd, Adopted->InputBits, Adopted->Levels);
        AppleDartAdoptedRelease(Adopted);
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
AppleDartAdoptedTranslate(
    _Inout_ PAPPLE_DART_ADOPTED Adopted,
    _In_ ULONG64 Iova,
    _Out_ PULONG64 Physical)
{
    ULONG64 Leaf, Entry;
    PULONG64 Table;
    BOOLEAN Mapped;

    *Physical = 0;
    Leaf = AppleDartAdoptedWalk(Adopted, Iova, FALSE);
    if (!Leaf)
        return FALSE;
    Table = AppleDartAdoptedTable(Adopted, Leaf, &Mapped);
    if (!Table)
        return FALSE;
    Entry = Table[(Iova >> DART_PAGE_SHIFT) & (DART_TABLE_ENTRIES - 1)];
    AppleDartAdoptedTableDone(Table, Mapped);
    if (!(Entry & DART_PTE_VALID))
        return FALSE;
    *Physical = ((Entry & DART_PTE_ADDRESS_MASK) << 4) | (Iova & (DART_PAGE_SIZE - 1));
    return TRUE;
}

BOOLEAN
NTAPI
AppleDartAdoptedRangeFree(
    _Inout_ PAPPLE_DART_ADOPTED Adopted,
    _In_ ULONG64 Iova,
    _In_ SIZE_T Length)
{
    ULONG64 First = Iova & ~(DART_PAGE_SIZE - 1), Last = (Iova + Length + DART_PAGE_SIZE - 1) & ~(DART_PAGE_SIZE - 1);
    ULONG64 Page, Unused;

    if (!Length || Last <= First)
        return FALSE;
    for (Page = First; Page < Last; Page += DART_PAGE_SIZE)
    {
        if (AppleDartAdoptedTranslate(Adopted, Page, &Unused))
            return FALSE;
    }
    return TRUE;
}

NTSTATUS
NTAPI
AppleDartAdoptedMapAt(
    _Inout_ PAPPLE_DART_ADOPTED Adopted,
    _In_ ULONG64 Iova,
    _In_ ULONG64 Physical,
    _In_ SIZE_T Length)
{
    ULONG64 First, Last, Page, Leaf;
    PULONG64 Table;
    BOOLEAN Mapped;

    First = Physical & ~(DART_PAGE_SIZE - 1);
    Last = (Physical + Length + DART_PAGE_SIZE - 1) & ~(DART_PAGE_SIZE - 1);
    if (!Length || Last <= First || (Iova & (DART_PAGE_SIZE - 1)) != (Physical & (DART_PAGE_SIZE - 1)))
        return STATUS_INVALID_PARAMETER;
    Iova &= ~(DART_PAGE_SIZE - 1);
    if (!AppleDartAdoptedRangeFree(Adopted, Iova, (SIZE_T)(Last - First)))
        return STATUS_CONFLICTING_ADDRESSES;
    for (Page = 0; Page < Last - First; Page += DART_PAGE_SIZE)
    {
        Leaf = AppleDartAdoptedWalk(Adopted, Iova + Page, TRUE);
        Table = Leaf ? AppleDartAdoptedTable(Adopted, Leaf, &Mapped) : NULL;
        if (!Table)
        {
            DbgPrint("%s: DART stream %lu has no table for device address %I64x\n", Adopted->Owner, Adopted->Sid,
                     Iova + Page);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Table[((Iova + Page) >> DART_PAGE_SHIFT) & (DART_TABLE_ENTRIES - 1)] =
            AppleDartEntry(First + Page) | DART_PTE_SUBPAGE_END | DART_PTE_VALID;
        AppleDartAdoptedTableDone(Table, Mapped);
    }
    AppleDartFlush(Adopted->Base, (DART_FAMILY)Adopted->Family, Adopted->Sid, Adopted->Owner);
    DbgPrint("%s: DART stream %lu maps %I64x+%Ix at device address %I64x\n", Adopted->Owner, Adopted->Sid, Physical,
             Length, Iova + (Physical - First));
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
AppleDartAdoptedMap(
    _Inout_ PAPPLE_DART_ADOPTED Adopted,
    _In_ ULONG64 Physical,
    _In_ SIZE_T Length,
    _Out_ PULONG64 Iova)
{
    ULONG64 First, Span, Start;
    NTSTATUS Status;

    *Iova = 0;
    First = Physical & ~(DART_PAGE_SIZE - 1);
    Span = ((Physical + Length + DART_PAGE_SIZE - 1) & ~(DART_PAGE_SIZE - 1)) - First;
    if (!Length || !Span)
        return STATUS_INVALID_PARAMETER;
    for (Start = Adopted->NextIova; Start + Span <= Adopted->EndIova; Start += DART_PAGE_SIZE)
    {
        if (!AppleDartAdoptedRangeFree(Adopted, Start, (SIZE_T)Span))
            continue;
        Status = AppleDartAdoptedMapAt(Adopted, Start + (Physical - First), Physical, Length);
        if (!NT_SUCCESS(Status))
            return Status;
        Adopted->NextIova = Start + Span;
        *Iova = Start + (Physical - First);
        return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
NTAPI
AppleDartAdoptedRelease(
    _Inout_ PAPPLE_DART_ADOPTED Adopted)
{
    ULONG Index;

    for (Index = 0; Index < Adopted->ForeignTables; ++Index)
        MmUnmapIoSpace(Adopted->ForeignVirtual[Index], DART_PAGE_SIZE);
    Adopted->ForeignTables = 0;
    if (Adopted->Base)
        MmUnmapIoSpace(Adopted->Base, Adopted->BaseSize);
    Adopted->Base = NULL;
}
