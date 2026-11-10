/*
 * PROJECT:     LiberNT Apple RTKit Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Apple silicon SPI controller
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#ifndef UNIT_TEST
#include <ntddk.h>
#endif
#include <reactos/drivers/applepmgr.h>
#include <reactos/drivers/applegpio.h>
#include <reactos/drivers/applespi.h>

#define SPI_CTRL                0x000
#define SPI_CTRL_RUN            0x00000001
#define SPI_CTRL_TX_RESET       0x00000004
#define SPI_CTRL_RX_RESET       0x00000008
#define SPI_CFG                 0x004
#define SPI_CFG_CPHA            0x00000002
#define SPI_CFG_CPOL            0x00000004
#define SPI_CFG_MODE_IRQ        0x00000020
#define SPI_CFG_LSB_FIRST       0x00002000
#define SPI_CFG_WORD_8          0x00000000
#define SPI_CFG_THRESH_8        0x00000000
#define SPI_PIN                 0x00c
#define SPI_PIN_CS              0x00000002
#define SPI_TXDATA              0x010
#define SPI_RXDATA              0x020
#define SPI_CLKDIV              0x030
#define SPI_CLKDIV_MAX          0x7ff
#define SPI_RXCNT               0x034
#define SPI_TXCNT               0x04c
#define SPI_FIFOSTAT            0x10c
#define SPI_FIFOSTAT_TX_LEVEL(x) (((x) >> 8) & 0xff)
#define SPI_FIFOSTAT_RX_LEVEL(x) (((x) >> 24) & 0xff)
#define SPI_IE_XFER             0x130
#define SPI_IF_XFER             0x134
#define SPI_XFER_RXCOMPLETE     0x00000001
#define SPI_XFER_TXCOMPLETE     0x00000002
#define SPI_IE_FIFO             0x138
#define SPI_IF_FIFO             0x13c
#define SPI_FIFO_RXTHRESH       0x00000010
#define SPI_FIFO_TXTHRESH       0x00000020
#define SPI_FIFO_RXUNDERRUN     0x00010000
#define SPI_FIFO_TXOVERFLOW     0x00020000
#define SPI_SHIFTCFG            0x150
#define SPI_SHIFTCFG_BITS_MASK  0x003f0000
#define SPI_SHIFTCFG_BITS_SHIFT 16
#define SPI_SHIFTCFG_OVERRIDE_CS 0x01000000
#define SPI_PINCFG              0x154
#define SPI_PINCFG_KEEP_CS      0x00000002
#define SPI_PINCFG_CS_IDLE_VAL  0x00000200
#define SPI_DELAY_PRE           0x160
#define SPI_DELAY_POST          0x168
#define SPI_DELAY_POST_CYCLES   0x194
#define SPI_REGISTER_SPAN       0x198

#define SPI_FIFO_DEPTH          16
#define SPI_DEFAULT_REFERENCE   24000000
#define SPI_TIMEOUT_US          200000
#define SPI_RX_DRAIN_TRIES      100

static
ULONG
AppleSpiRead(
    _In_ PAPPLE_SPI Spi,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Spi->Registers + Offset));
}

static
VOID
AppleSpiWrite(
    _In_ PAPPLE_SPI Spi,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Spi->Registers + Offset), Value);
}

static
VOID
AppleSpiModify(
    _In_ PAPPLE_SPI Spi,
    _In_ ULONG Offset,
    _In_ ULONG Clear,
    _In_ ULONG Set)
{
    AppleSpiWrite(Spi, Offset, (AppleSpiRead(Spi, Offset) & ~Clear) | Set);
}

NTSTATUS
NTAPI
AppleSpiOpen(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Node,
    _Out_ PAPPLE_SPI Spi)
{
    const VOID *Compatible, *Clocks;
    ULONG Length, Clock;
    ULONG64 Phandle;
    NTSTATUS Status;

    RtlZeroMemory(Spi, sizeof(*Spi));
    Compatible = FdtGetProperty(&Tree->Fdt, Node, "compatible", &Length);
    if (!FdtStringListContains(Compatible, Length, "apple,t8103-spi") &&
        !FdtStringListContains(Compatible, Length, "apple,spi"))
        return STATUS_NOT_SUPPORTED;
    Spi->ReferenceHz = SPI_DEFAULT_REFERENCE;
    Clocks = FdtGetProperty(&Tree->Fdt, Node, "clocks", &Length);
    if (Clocks && FdtReadCells(Clocks, Length, 0, 1, &Phandle) &&
        (Clock = FdtDriverFindPhandle(Tree, (ULONG)Phandle)) != FDT_NO_NODE &&
        !FdtReadU32(&Tree->Fdt, Clock, "clock-frequency", &Spi->ReferenceHz))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!Spi->ReferenceHz)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Status = ApplePmgrPowerOnDevice(Tree, Node);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = AppleGpioApplyPinctrl(Tree, Node);
    if (!NT_SUCCESS(Status))
        return Status;
    Spi->Registers = FdtDriverMapReg(Tree, Node, 0, NULL, &Spi->Size);
    if (!Spi->Registers)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Spi->Size < SPI_REGISTER_SPAN)
    {
        AppleSpiClose(Spi);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    AppleSpiWrite(Spi, SPI_PIN, SPI_PIN_CS);
    AppleSpiModify(Spi, SPI_SHIFTCFG, SPI_SHIFTCFG_OVERRIDE_CS, 0);
    AppleSpiModify(Spi, SPI_PINCFG, SPI_PINCFG_CS_IDLE_VAL, SPI_PINCFG_KEEP_CS);
    AppleSpiWrite(Spi, SPI_CTRL, SPI_CTRL_RX_RESET | SPI_CTRL_TX_RESET);
    AppleSpiWrite(Spi, SPI_CFG, SPI_CFG_THRESH_8 | SPI_CFG_MODE_IRQ | SPI_CFG_WORD_8);
    AppleSpiWrite(Spi, SPI_IE_FIFO, 0);
    AppleSpiWrite(Spi, SPI_IE_XFER, 0);
    AppleSpiWrite(Spi, SPI_DELAY_PRE, 0);
    AppleSpiWrite(Spi, SPI_DELAY_POST, 0);
    AppleSpiWrite(Spi, SPI_DELAY_POST_CYCLES, 0);
    return STATUS_SUCCESS;
}

VOID
NTAPI
AppleSpiClose(
    _Inout_ PAPPLE_SPI Spi)
{
    if (Spi->Registers)
    {
        AppleSpiWrite(Spi, SPI_CTRL, 0);
        AppleSpiWrite(Spi, SPI_PIN, SPI_PIN_CS);
        MmUnmapIoSpace(Spi->Registers, Spi->Size);
    }
    RtlZeroMemory(Spi, sizeof(*Spi));
}

VOID
NTAPI
AppleSpiSetChipSelect(
    _In_ PAPPLE_SPI Spi,
    _In_ BOOLEAN Asserted)
{
    if (Spi->Registers)
        AppleSpiModify(Spi, SPI_PIN, SPI_PIN_CS, Asserted ? 0 : SPI_PIN_CS);
}

static
VOID
AppleSpiFillTx(
    _In_ PAPPLE_SPI Spi,
    _Inout_ const UCHAR **Write,
    _Inout_ PULONG Left)
{
    ULONG InUse, Words;

    if (!*Write || !*Left)
        return;
    InUse = SPI_FIFOSTAT_TX_LEVEL(AppleSpiRead(Spi, SPI_FIFOSTAT));
    Words = InUse >= SPI_FIFO_DEPTH ? 0 : min(*Left, SPI_FIFO_DEPTH - InUse);
    *Left -= Words;
    while (Words--)
        AppleSpiWrite(Spi, SPI_TXDATA, *(*Write)++);
}

static
BOOLEAN
AppleSpiDrainRx(
    _In_ PAPPLE_SPI Spi,
    _Inout_ PUCHAR *Read,
    _Inout_ PULONG Left)
{
    ULONG Words;

    if (!*Read || !*Left)
        return TRUE;
    Words = SPI_FIFOSTAT_RX_LEVEL(AppleSpiRead(Spi, SPI_FIFOSTAT));
    if (Words > *Left)
        return FALSE;
    *Left -= Words;
    while (Words--)
        *(*Read)++ = (UCHAR)AppleSpiRead(Spi, SPI_RXDATA);
    return TRUE;
}

NTSTATUS
NTAPI
AppleSpiTransfer(
    _Inout_ PAPPLE_SPI Spi,
    _In_ ULONG SpeedHz,
    _In_reads_bytes_opt_(Length) const UCHAR *Write,
    _Out_writes_bytes_opt_(Length) PUCHAR Read,
    _In_ ULONG Length)
{
    ULONG TxLeft, RxLeft, Pending = 0, Divider, Waited, Budget, Tries, Fifo;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Spi->Registers || !Length || (!Write && !Read) || !SpeedHz)
        return STATUS_INVALID_PARAMETER;
    TxLeft = Write ? Length : 0;
    RxLeft = Read ? Length : 0;
    Divider = (Spi->ReferenceHz + SpeedHz - 1) / SpeedHz;
    Divider = min(max(Divider, 1), SPI_CLKDIV_MAX);
    Budget = SPI_TIMEOUT_US + (ULONG)(((ULONG64)Length * 8 * Divider * 1000000) / Spi->ReferenceHz);

    AppleSpiWrite(Spi, SPI_CLKDIV, Divider);
    AppleSpiModify(Spi, SPI_SHIFTCFG, SPI_SHIFTCFG_BITS_MASK, 8 << SPI_SHIFTCFG_BITS_SHIFT);
    AppleSpiWrite(Spi, SPI_CTRL, SPI_CTRL_RX_RESET | SPI_CTRL_TX_RESET);
    AppleSpiWrite(Spi, SPI_IF_XFER, 0xffffffff);
    AppleSpiWrite(Spi, SPI_IF_FIFO, 0xffffffff);
    if (Write)
        Pending |= SPI_XFER_TXCOMPLETE;
    if (Read)
        Pending |= SPI_XFER_RXCOMPLETE;
    AppleSpiWrite(Spi, SPI_TXCNT, TxLeft);
    AppleSpiWrite(Spi, SPI_RXCNT, RxLeft);

    AppleSpiFillTx(Spi, &Write, &TxLeft);
    AppleSpiWrite(Spi, SPI_CTRL, SPI_CTRL_RUN);
    /* A few words leave the FIFO as soon as the controller runs. */
    AppleSpiFillTx(Spi, &Write, &TxLeft);

    for (Waited = 0; Pending; )
    {
        Pending &= ~AppleSpiRead(Spi, SPI_IF_XFER);
        AppleSpiFillTx(Spi, &Write, &TxLeft);
        if (!AppleSpiDrainRx(Spi, &Read, &RxLeft))
        {
            Status = STATUS_DATA_OVERRUN;
            break;
        }
        if (!Pending)
            break;
        if (Waited++ >= Budget)
        {
            Status = STATUS_IO_TIMEOUT;
            break;
        }
        KeStallExecutionProcessor(1);
    }
    /* The completion can come before the last word reaches the receive FIFO. */
    for (Tries = 0; NT_SUCCESS(Status) && RxLeft && Tries < SPI_RX_DRAIN_TRIES; ++Tries)
    {
        if (!AppleSpiDrainRx(Spi, &Read, &RxLeft))
            Status = STATUS_DATA_OVERRUN;
    }
    ASSERT(TxLeft <= Length && RxLeft <= Length);
    if (NT_SUCCESS(Status) && (TxLeft || RxLeft))
        Status = STATUS_IO_DEVICE_ERROR;
    Fifo = AppleSpiRead(Spi, SPI_IF_FIFO);
    if (NT_SUCCESS(Status) && (Fifo & (SPI_FIFO_TXOVERFLOW | SPI_FIFO_RXUNDERRUN)))
        Status = STATUS_IO_DEVICE_ERROR;
    AppleSpiWrite(Spi, SPI_CTRL, 0);
    if (!NT_SUCCESS(Status))
        DbgPrint("applespi: transfer of %lu bytes failed: %lx (%lu to send, %lu to receive, fifo flags %08lx)\n",
                 Length, Status, TxLeft, RxLeft, Fifo);
    return Status;
}
