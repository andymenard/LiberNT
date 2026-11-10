/*
 * PROJECT:     LiberNT Apple RTKit Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Apple silicon SPI controller
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#include <reactos/drivers/fdtdrv.h>

typedef struct _APPLE_SPI
{
    PUCHAR Registers;
    ULONG Size;
    ULONG ReferenceHz;
} APPLE_SPI, *PAPPLE_SPI;

NTSTATUS
NTAPI
AppleSpiOpen(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Node,
    _Out_ PAPPLE_SPI Spi);

VOID
NTAPI
AppleSpiClose(
    _Inout_ PAPPLE_SPI Spi);

VOID
NTAPI
AppleSpiSetChipSelect(
    _In_ PAPPLE_SPI Spi,
    _In_ BOOLEAN Asserted);

NTSTATUS
NTAPI
AppleSpiTransfer(
    _Inout_ PAPPLE_SPI Spi,
    _In_ ULONG SpeedHz,
    _In_reads_bytes_opt_(Length) const UCHAR *Write,
    _Out_writes_bytes_opt_(Length) PUCHAR Read,
    _In_ ULONG Length);
