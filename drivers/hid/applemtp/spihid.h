/*
 * PROJECT:     LiberNT Apple MTP HID Transport Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SPI HID transport of the M1-family internal keyboard and trackpad
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#include <reactos/drivers/fdtdrv.h>
#include <reactos/drivers/applegpio.h>
#include <reactos/drivers/applespi.h>

#define SPIHID_PACKET_SIZE          256
#define SPIHID_PACKET_DATA          246
#define SPIHID_MAX_MESSAGE          0x800
#define SPIHID_DESCRIPTOR_MAX       512
#define SPIHID_MAX_INTERFACES       4

#define SPIHID_INTERFACE_MANAGEMENT 0
#define SPIHID_INTERFACE_KEYBOARD   1
#define SPIHID_INTERFACE_TRACKPAD   2

typedef VOID (NTAPI *PSPIHID_REPORT_ROUTINE)(
    _In_ PVOID Context,
    _In_ UCHAR Interface,
    _In_reads_bytes_(Length) const UCHAR *Report,
    _In_ ULONG Length);

typedef struct _SPIHID_INTERFACE
{
    BOOLEAN InfoValid;
    UCHAR Country;
    USHORT MaxInputReport;
    USHORT MaxOutputReport;
    USHORT MaxControlReport;
    CHAR Name[32];
    ULONG DescriptorLength;
    UCHAR Descriptor[SPIHID_DESCRIPTOR_MAX];
} SPIHID_INTERFACE, *PSPIHID_INTERFACE;

typedef struct _SPIHID
{
    APPLE_SPI Spi;
    APPLE_GPIO Enable;
    APPLE_GPIO Interrupt;
    ULONG SpeedHz;
    ULONG CsSetupUs;
    ULONG CsHoldUs;
    ULONG CsInactiveUs;

    PSPIHID_REPORT_ROUTINE ReportRoutine;
    PVOID ReportContext;
    BOOLEAN Delivering;

    UCHAR Tx[SPIHID_PACKET_SIZE];
    UCHAR Rx[SPIHID_PACKET_SIZE];
    UCHAR WriteStatus[4];
    UCHAR NextId;

    UCHAR Message[SPIHID_MAX_MESSAGE];
    ULONG MessageOffset;
    ULONG MessageLength;
    UCHAR MessageDevice;
    UCHAR MessageFlags;

    BOOLEAN Booted;
    ULONG DeviceCount;
    USHORT VendorId;
    USHORT ProductId;
    USHORT Version;
    SPIHID_INTERFACE Interfaces[SPIHID_MAX_INTERFACES];

    ULONG BadPackets;
    ULONG BadStatus;
    BOOLEAN TrackpadResetSeen;
} SPIHID, *PSPIHID;

NTSTATUS
SpiHidOpen(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Node,
    _In_ PSPIHID_REPORT_ROUTINE ReportRoutine,
    _In_opt_ PVOID ReportContext,
    _Out_ PSPIHID Hid);

NTSTATUS
SpiHidStart(
    _Inout_ PSPIHID Hid);

NTSTATUS
SpiHidEnableMultitouch(
    _Inout_ PSPIHID Hid);

BOOLEAN
SpiHidPoll(
    _Inout_ PSPIHID Hid);

VOID
SpiHidClose(
    _Inout_ PSPIHID Hid);

USHORT
SpiHidCrc16(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length);
