/*
 * PROJECT:     LiberNT Apple MTP HID Transport Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SPI HID transport of the M1-family internal keyboard and trackpad
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#ifndef UNIT_TEST
#include <ntddk.h>
#endif
#include "spihid.h"

#define SPIHID_FLAGS_WRITE          0x40
#define SPIHID_FLAGS_READ           0x20
#define SPIHID_DEVICE_INFO          0xd0
#define SPIHID_HEADER_SIZE          8
#define SPIHID_REQ_INFO             0x20
#define SPIHID_REQ_INPUT            0x10
#define SPIHID_REQ_SET_FEATURE      0x52
#define SPIHID_INFO_DEVICE          0x01
#define SPIHID_INFO_INTERFACE       0x02
#define SPIHID_INFO_DESCRIPTOR      0x10
#define SPIHID_MIN_DEVICES          3

#define SPIHID_PACKET_FLAGS         0
#define SPIHID_PACKET_DEVICE        1
#define SPIHID_PACKET_OFFSET        2
#define SPIHID_PACKET_REMAIN        4
#define SPIHID_PACKET_LENGTH        6
#define SPIHID_PACKET_PAYLOAD       8
#define SPIHID_PACKET_CRC           (SPIHID_PACKET_SIZE - 2)

#define SPIHID_MESSAGE_REQUEST      0
#define SPIHID_MESSAGE_REPORT       1
#define SPIHID_MESSAGE_SUBDEVICE    2
#define SPIHID_MESSAGE_ID           3
#define SPIHID_MESSAGE_RESPONSE     4
#define SPIHID_MESSAGE_LENGTH       6

#define SPIHID_DEVINFO_COUNT        4
#define SPIHID_DEVINFO_VENDOR       6
#define SPIHID_DEVINFO_PRODUCT      8
#define SPIHID_DEVINFO_VERSION      10
#define SPIHID_DEVINFO_SIZE         22

#define SPIHID_IFINFO_COUNTRY       5
#define SPIHID_IFINFO_MAX_INPUT     6
#define SPIHID_IFINFO_MAX_OUTPUT    8
#define SPIHID_IFINFO_MAX_CONTROL   10
#define SPIHID_IFINFO_NAME_OFFSET   12
#define SPIHID_IFINFO_NAME_LENGTH   14
#define SPIHID_IFINFO_SIZE          16

#define SPIHID_TRACKPAD_RESET_REPORT 0x60

#define SPIHID_DEFAULT_SPEED        8000000
#define SPIHID_DEFAULT_CS_SETUP_NS  65000
#define SPIHID_DEFAULT_CS_HOLD_NS   65000
#define SPIHID_DEFAULT_CS_IDLE_NS   250000
#define SPIHID_WRITE_TURNAROUND_US  200
#define SPIHID_WAIT_MS              1000
#define SPIHID_BOOT_WAIT_MS         1000

static const UCHAR SpiHidBooted[4] = { 0xa0, 0x80, 0x00, 0x00 };
static const UCHAR SpiHidStatusOk[4] = { 0xac, 0x27, 0x68, 0xd5 };

#ifndef UNIT_TEST
static
VOID
SpiHidSleepMs(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -(LONGLONG)Milliseconds * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}
#endif

static
USHORT
SpiHidLe16(
    _In_reads_bytes_(2) const UCHAR *Data)
{
    return (USHORT)(Data[0] | (Data[1] << 8));
}

static
VOID
SpiHidPutLe16(
    _Out_writes_bytes_(2) PUCHAR Data,
    _In_ USHORT Value)
{
    Data[0] = (UCHAR)Value;
    Data[1] = (UCHAR)(Value >> 8);
}

/* CRC-16/ARC: polynomial 0x8005 reflected, initial value 0. */
USHORT
SpiHidCrc16(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    USHORT Crc = 0;
    ULONG Index, Bit;

    for (Index = 0; Index < Length; ++Index)
    {
        Crc ^= Data[Index];
        for (Bit = 0; Bit < 8; ++Bit)
            Crc = (Crc & 1) ? (USHORT)((Crc >> 1) ^ 0xa001) : (USHORT)(Crc >> 1);
    }
    return Crc;
}

static
ULONG
SpiHidReadDelayUs(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Node,
    _In_z_ PCSTR Property,
    _In_ ULONG DefaultNs)
{
    ULONG Ns;

    if (!FdtReadU32(&Tree->Fdt, Node, Property, &Ns))
        Ns = DefaultNs;
    return (Ns + 999) / 1000;
}

NTSTATUS
SpiHidOpen(
    _In_ PFDT_DRIVER_TREE Tree,
    _In_ ULONG Node,
    _In_ PSPIHID_REPORT_ROUTINE ReportRoutine,
    _In_opt_ PVOID ReportContext,
    _Out_ PSPIHID Hid)
{
    ULONG Transport;
    NTSTATUS Status;

    RtlZeroMemory(Hid, sizeof(*Hid));
    Transport = FdtDriverFindChild(Tree, Node, "apple,spi-hid-transport");
    if (Transport == FDT_NO_NODE)
        return STATUS_NO_SUCH_DEVICE;
    if (!FdtReadU32(&Tree->Fdt, Transport, "spi-max-frequency", &Hid->SpeedHz) || !Hid->SpeedHz)
        Hid->SpeedHz = SPIHID_DEFAULT_SPEED;
    /* The defaults add the firmware's 20 us chip select delays to the interface's 45 us: neither alone is reliable. */
    Hid->CsSetupUs = SpiHidReadDelayUs(Tree, Transport, "spi-cs-setup-delay-ns", SPIHID_DEFAULT_CS_SETUP_NS);
    Hid->CsHoldUs = SpiHidReadDelayUs(Tree, Transport, "spi-cs-hold-delay-ns", SPIHID_DEFAULT_CS_HOLD_NS);
    Hid->CsInactiveUs = SpiHidReadDelayUs(Tree, Transport, "spi-cs-inactive-delay-ns", SPIHID_DEFAULT_CS_IDLE_NS);
    Hid->ReportRoutine = ReportRoutine;
    Hid->ReportContext = ReportContext;

    Status = AppleGpioOpen(Tree, Transport, "spien-gpios", &Hid->Enable);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("spihid: no usable enable line: %lx\n", Status);
        return Status;
    }
    Status = AppleGpioOpenInterrupt(Tree, Transport, &Hid->Interrupt);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("spihid: no usable interrupt line: %lx\n", Status);
        SpiHidClose(Hid);
        return Status;
    }
    Status = AppleSpiOpen(Tree, Node, &Hid->Spi);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("spihid: the SPI controller did not open: %lx\n", Status);
        SpiHidClose(Hid);
        return Status;
    }
    DbgPrint("spihid: %lu Hz, chip select setup %lu us, hold %lu us, idle %lu us\n", Hid->SpeedHz, Hid->CsSetupUs,
             Hid->CsHoldUs, Hid->CsInactiveUs);
    return STATUS_SUCCESS;
}

VOID
SpiHidClose(
    _Inout_ PSPIHID Hid)
{
    if (Hid->Enable.Registers)
        AppleGpioSet(&Hid->Enable, FALSE);
    AppleSpiClose(&Hid->Spi);
    AppleGpioClose(&Hid->Interrupt);
    AppleGpioClose(&Hid->Enable);
    Hid->Booted = FALSE;
}

static
VOID
SpiHidSelect(
    _Inout_ PSPIHID Hid)
{
    AppleSpiSetChipSelect(&Hid->Spi, TRUE);
    KeStallExecutionProcessor(Hid->CsSetupUs);
}

static
VOID
SpiHidDeselect(
    _Inout_ PSPIHID Hid)
{
    KeStallExecutionProcessor(Hid->CsHoldUs);
    AppleSpiSetChipSelect(&Hid->Spi, FALSE);
    KeStallExecutionProcessor(Hid->CsInactiveUs);
}

static
VOID
SpiHidDeliver(
    _Inout_ PSPIHID Hid,
    _In_ UCHAR Interface,
    _In_reads_bytes_(Length) const UCHAR *Report,
    _In_ ULONG Length)
{
    if (Interface == SPIHID_INTERFACE_TRACKPAD && Length && Report[0] == SPIHID_TRACKPAD_RESET_REPORT)
    {
        /* The touch controller reset itself and is back to mouse reports. */
        Hid->TrackpadResetSeen = TRUE;
        return;
    }
    if (Hid->ReportRoutine)
    {
        Hid->Delivering = TRUE;
        Hid->ReportRoutine(Hid->ReportContext, Interface, Report, Length);
        Hid->Delivering = FALSE;
    }
}

static
VOID
SpiHidCopyString(
    _Out_writes_z_(Size) PCHAR Destination,
    _In_ ULONG Size,
    _In_reads_bytes_(Length) const UCHAR *Payload,
    _In_ ULONG Length,
    _In_ ULONG Offset,
    _In_ ULONG StringLength)
{
    Destination[0] = '\0';
    if (Offset < Length && StringLength <= Length - Offset && StringLength < Size)
    {
        RtlCopyMemory(Destination, Payload + Offset, StringLength);
        Destination[StringLength] = '\0';
    }
}

static
VOID
SpiHidResponse(
    _Inout_ PSPIHID Hid,
    _In_reads_bytes_(SPIHID_HEADER_SIZE) const UCHAR *Header,
    _In_reads_bytes_(Length) const UCHAR *Payload,
    _In_ ULONG Length)
{
    PSPIHID_INTERFACE Interface;
    UCHAR Which = Header[SPIHID_MESSAGE_SUBDEVICE];

    if (Header[SPIHID_MESSAGE_REQUEST] != SPIHID_REQ_INFO)
        return;
    switch (Header[SPIHID_MESSAGE_REPORT])
    {
        case SPIHID_INFO_DEVICE:
            if (Which != SPIHID_DEVICE_INFO || Hid->VendorId || Length < SPIHID_DEVINFO_SIZE)
                return;
            Hid->DeviceCount = SpiHidLe16(Payload + SPIHID_DEVINFO_COUNT);
            if (Hid->DeviceCount < SPIHID_MIN_DEVICES)
            {
                DbgPrint("spihid: the controller reports only %lu devices\n", Hid->DeviceCount);
                Hid->DeviceCount = 0;
                return;
            }
            Hid->DeviceCount = min(Hid->DeviceCount, SPIHID_MAX_INTERFACES);
            Hid->VendorId = SpiHidLe16(Payload + SPIHID_DEVINFO_VENDOR);
            Hid->ProductId = SpiHidLe16(Payload + SPIHID_DEVINFO_PRODUCT);
            Hid->Version = SpiHidLe16(Payload + SPIHID_DEVINFO_VERSION);
            DbgPrint("spihid: controller %04x:%04x version %04x with %lu interfaces\n", Hid->VendorId,
                     Hid->ProductId, Hid->Version, Hid->DeviceCount);
            break;

        case SPIHID_INFO_INTERFACE:
            if (Which >= SPIHID_MAX_INTERFACES || Length < SPIHID_IFINFO_SIZE)
                return;
            Interface = &Hid->Interfaces[Which];
            if (Interface->InfoValid)
                return;
            Interface->Country = Payload[SPIHID_IFINFO_COUNTRY];
            Interface->MaxInputReport = SpiHidLe16(Payload + SPIHID_IFINFO_MAX_INPUT);
            Interface->MaxOutputReport = SpiHidLe16(Payload + SPIHID_IFINFO_MAX_OUTPUT);
            Interface->MaxControlReport = SpiHidLe16(Payload + SPIHID_IFINFO_MAX_CONTROL);
            SpiHidCopyString(Interface->Name, sizeof(Interface->Name), Payload, Length,
                             SpiHidLe16(Payload + SPIHID_IFINFO_NAME_OFFSET),
                             SpiHidLe16(Payload + SPIHID_IFINFO_NAME_LENGTH));
            Interface->InfoValid = TRUE;
            DbgPrint("spihid: interface %u is %s, reports up to %u bytes\n", Which, Interface->Name,
                     Interface->MaxInputReport);
            break;

        case SPIHID_INFO_DESCRIPTOR:
            if (Which >= SPIHID_MAX_INTERFACES || Length > SPIHID_DESCRIPTOR_MAX)
                return;
            Interface = &Hid->Interfaces[Which];
            if (Interface->DescriptorLength)
                return;
            RtlCopyMemory(Interface->Descriptor, Payload, Length);
            Interface->DescriptorLength = Length;
            DbgPrint("spihid: interface %u report descriptor is %lu bytes\n", Which, Length);
            break;

        default:
            break;
    }
}

static
VOID
SpiHidMessage(
    _Inout_ PSPIHID Hid,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length,
    _In_ UCHAR Device,
    _In_ UCHAR Flags)
{
    ULONG PayloadLength;

    if (Length < SPIHID_HEADER_SIZE + 2 ||
        SpiHidCrc16(Data, Length - 2) != SpiHidLe16(Data + Length - 2))
    {
        Hid->BadPackets++;
        return;
    }
    PayloadLength = SpiHidLe16(Data + SPIHID_MESSAGE_LENGTH);
    if (!PayloadLength || PayloadLength + SPIHID_HEADER_SIZE + 2 > Length)
        return;
    if (Flags == SPIHID_FLAGS_WRITE)
    {
        SpiHidResponse(Hid, Data, Data + SPIHID_HEADER_SIZE, PayloadLength);
    }
    else if (Flags == SPIHID_FLAGS_READ && Data[SPIHID_MESSAGE_REQUEST] == SPIHID_REQ_INPUT &&
             Device > SPIHID_INTERFACE_MANAGEMENT &&
             Device < SPIHID_MAX_INTERFACES)
    {
        SpiHidDeliver(Hid, Device, Data + SPIHID_HEADER_SIZE, PayloadLength);
    }
}

static
VOID
SpiHidPacket(
    _Inout_ PSPIHID Hid)
{
    const UCHAR *Packet = Hid->Rx;
    ULONG Offset, Remain, Length;

    if (SpiHidCrc16(Packet, SPIHID_PACKET_CRC) != SpiHidLe16(Packet + SPIHID_PACKET_CRC))
    {
        Hid->BadPackets++;
        return;
    }
    Offset = SpiHidLe16(Packet + SPIHID_PACKET_OFFSET);
    Remain = SpiHidLe16(Packet + SPIHID_PACKET_REMAIN);
    Length = SpiHidLe16(Packet + SPIHID_PACKET_LENGTH);
    if (Length < SPIHID_HEADER_SIZE + 2)
    {
        if (Length == sizeof(SpiHidBooted) && RtlEqualMemory(Packet + SPIHID_PACKET_PAYLOAD, SpiHidBooted, sizeof(SpiHidBooted)))
        {
            if (!Hid->Booted)
                DbgPrint("spihid: the keyboard and trackpad controller is up\n");
            Hid->Booted = TRUE;
        }
        return;
    }
    if (Length > SPIHID_PACKET_DATA || Offset + Length + Remain > SPIHID_MAX_MESSAGE)
    {
        Hid->BadPackets++;
        return;
    }
    if (!Offset && !Remain)
    {
        SpiHidMessage(Hid, Packet + SPIHID_PACKET_PAYLOAD, Length, Packet[SPIHID_PACKET_DEVICE],
                      Packet[SPIHID_PACKET_FLAGS]);
        return;
    }

    if (Packet[SPIHID_PACKET_DEVICE] != Hid->MessageDevice || Packet[SPIHID_PACKET_FLAGS] != Hid->MessageFlags ||
        Offset != Hid->MessageOffset)
        Hid->MessageOffset = Hid->MessageLength = 0;
    if (!Offset)
    {
        RtlCopyMemory(Hid->Message, Packet + SPIHID_PACKET_PAYLOAD, Length);
        Hid->MessageOffset = Length;
        Hid->MessageLength = Length + Remain;
        Hid->MessageDevice = Packet[SPIHID_PACKET_DEVICE];
        Hid->MessageFlags = Packet[SPIHID_PACKET_FLAGS];
        return;
    }
    if (!Hid->MessageLength || Offset + Length + Remain != Hid->MessageLength)
    {
        Hid->MessageOffset = Hid->MessageLength = 0;
        return;
    }
    ASSERT(Hid->MessageOffset == Offset && Offset + Length <= Hid->MessageLength);
    RtlCopyMemory(Hid->Message + Offset, Packet + SPIHID_PACKET_PAYLOAD, Length);
    Hid->MessageOffset += Length;
    if (Hid->MessageOffset == Hid->MessageLength)
    {
        SpiHidMessage(Hid, Hid->Message, Hid->MessageLength, Hid->MessageDevice, Hid->MessageFlags);
        Hid->MessageOffset = Hid->MessageLength = 0;
    }
}

BOOLEAN
SpiHidPoll(
    _Inout_ PSPIHID Hid)
{
    NTSTATUS Status;

    if (!Hid->Spi.Registers || !AppleGpioGet(&Hid->Interrupt))
        return FALSE;
    SpiHidSelect(Hid);
    Status = AppleSpiTransfer(&Hid->Spi, Hid->SpeedHz, NULL, Hid->Rx, sizeof(Hid->Rx));
    SpiHidDeselect(Hid);
    if (NT_SUCCESS(Status))
        SpiHidPacket(Hid);
    return TRUE;
}

static
NTSTATUS
SpiHidRequest(
    _Inout_ PSPIHID Hid,
    _In_ UCHAR Target,
    _In_ UCHAR Request,
    _In_ UCHAR Report,
    _In_ UCHAR SubDevice,
    _In_ USHORT ResponseLength,
    _In_reads_bytes_opt_(Length) const UCHAR *Payload,
    _In_ ULONG Length)
{
    PUCHAR Message = Hid->Tx + SPIHID_PACKET_PAYLOAD;
    NTSTATUS Status;

    if (Length > SPIHID_PACKET_DATA - SPIHID_HEADER_SIZE - 2 || (Length && !Payload))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Hid->Tx, sizeof(Hid->Tx));
    Hid->Tx[SPIHID_PACKET_FLAGS] = SPIHID_FLAGS_WRITE;
    Hid->Tx[SPIHID_PACKET_DEVICE] = Target;
    SpiHidPutLe16(Hid->Tx + SPIHID_PACKET_LENGTH, (USHORT)(SPIHID_HEADER_SIZE + Length + 2));
    Message[SPIHID_MESSAGE_REQUEST] = Request;
    Message[SPIHID_MESSAGE_REPORT] = Report;
    Message[SPIHID_MESSAGE_SUBDEVICE] = SubDevice;
    Message[SPIHID_MESSAGE_ID] = Hid->NextId++;
    SpiHidPutLe16(Message + SPIHID_MESSAGE_RESPONSE, ResponseLength);
    SpiHidPutLe16(Message + SPIHID_MESSAGE_LENGTH, (USHORT)Length);
    if (Length)
        RtlCopyMemory(Message + SPIHID_HEADER_SIZE, Payload, Length);
    SpiHidPutLe16(Message + SPIHID_HEADER_SIZE + Length, SpiHidCrc16(Message, SPIHID_HEADER_SIZE + Length));
    SpiHidPutLe16(Hid->Tx + SPIHID_PACKET_CRC, SpiHidCrc16(Hid->Tx, SPIHID_PACKET_CRC));

    RtlZeroMemory(Hid->WriteStatus, sizeof(Hid->WriteStatus));
    SpiHidSelect(Hid);
    Status = AppleSpiTransfer(&Hid->Spi, Hid->SpeedHz, Hid->Tx, NULL, sizeof(Hid->Tx));
    if (NT_SUCCESS(Status))
    {
        KeStallExecutionProcessor(SPIHID_WRITE_TURNAROUND_US);
        Status = AppleSpiTransfer(&Hid->Spi, Hid->SpeedHz, NULL, Hid->WriteStatus, sizeof(Hid->WriteStatus));
    }
    SpiHidDeselect(Hid);
    if (NT_SUCCESS(Status) && !RtlEqualMemory(Hid->WriteStatus, SpiHidStatusOk, sizeof(SpiHidStatusOk)))
    {
        Hid->BadStatus++;
        DbgPrint("spihid: request %02x/%02x to %02x answered %02x %02x %02x %02x\n", Request, Report, Target,
                 Hid->WriteStatus[0], Hid->WriteStatus[1], Hid->WriteStatus[2], Hid->WriteStatus[3]);
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

typedef enum _SPIHID_WAIT
{
    SpiHidWaitBoot,
    SpiHidWaitDeviceInfo,
    SpiHidWaitInterfaceInfo,
    SpiHidWaitDescriptor
} SPIHID_WAIT;

static
BOOLEAN
SpiHidDone(
    _In_ PSPIHID Hid,
    _In_ SPIHID_WAIT What,
    _In_ ULONG Interface)
{
    switch (What)
    {
        case SpiHidWaitBoot:
            return Hid->Booted;
        case SpiHidWaitDeviceInfo:
            return Hid->VendorId != 0;
        case SpiHidWaitInterfaceInfo:
            return Hid->Interfaces[Interface].InfoValid;
        case SpiHidWaitDescriptor:
            return Hid->Interfaces[Interface].DescriptorLength != 0;
    }
    return FALSE;
}

static
BOOLEAN
SpiHidWaitFor(
    _Inout_ PSPIHID Hid,
    _In_ SPIHID_WAIT What,
    _In_ ULONG Interface,
    _In_ ULONG Milliseconds)
{
    ULONG Waited = 0;

    while (!SpiHidDone(Hid, What, Interface))
    {
        if (SpiHidPoll(Hid))
            continue;
        if (Waited++ >= Milliseconds)
            return FALSE;
        SpiHidSleepMs(1);
    }
    return TRUE;
}

NTSTATUS
SpiHidEnableMultitouch(
    _Inout_ PSPIHID Hid)
{
    static const UCHAR Multitouch[2] = { 0x02, 0x01 };

    Hid->TrackpadResetSeen = FALSE;
    return SpiHidRequest(Hid, SPIHID_INTERFACE_TRACKPAD, SPIHID_REQ_SET_FEATURE, Multitouch[0], 0,
                         sizeof(Multitouch), Multitouch, sizeof(Multitouch));
}

NTSTATUS
SpiHidStart(
    _Inout_ PSPIHID Hid)
{
    ULONG Index;
    NTSTATUS Status;

    if (!Hid->Spi.Registers)
        return STATUS_DEVICE_NOT_READY;
    AppleGpioConfigureInput(&Hid->Interrupt);
    AppleGpioSet(&Hid->Enable, TRUE);
    SpiHidSleepMs(5);
    AppleGpioSet(&Hid->Enable, FALSE);
    SpiHidSleepMs(5);
    AppleGpioSet(&Hid->Enable, TRUE);
    SpiHidSleepMs(50);

    if (!SpiHidWaitFor(Hid, SpiHidWaitBoot, 0, SPIHID_BOOT_WAIT_MS))
    {
        DbgPrint("spihid: the controller did not report that it booted (%lu bad packets)\n", Hid->BadPackets);
        return STATUS_DEVICE_NOT_READY;
    }

    Status = SpiHidRequest(Hid, SPIHID_DEVICE_INFO, SPIHID_REQ_INFO, SPIHID_INFO_DEVICE, SPIHID_DEVICE_INFO, 0,
                           NULL, 0);
    if (!NT_SUCCESS(Status) || !SpiHidWaitFor(Hid, SpiHidWaitDeviceInfo, 0, SPIHID_WAIT_MS))
    {
        DbgPrint("spihid: no device information: %lx\n", Status);
        return NT_SUCCESS(Status) ? STATUS_IO_TIMEOUT : Status;
    }
    for (Index = 0; Index < Hid->DeviceCount; ++Index)
    {
        if (NT_SUCCESS(SpiHidRequest(Hid, SPIHID_DEVICE_INFO, SPIHID_REQ_INFO, SPIHID_INFO_INTERFACE, (UCHAR)Index,
                                     SPIHID_DESCRIPTOR_MAX, NULL, 0)))
        {
            SpiHidWaitFor(Hid, SpiHidWaitInterfaceInfo, Index, SPIHID_WAIT_MS);
        }
    }
    for (Index = 1; Index < Hid->DeviceCount; ++Index)
    {
        if (NT_SUCCESS(SpiHidRequest(Hid, SPIHID_DEVICE_INFO, SPIHID_REQ_INFO, SPIHID_INFO_DESCRIPTOR, (UCHAR)Index,
                                     SPIHID_DESCRIPTOR_MAX, NULL, 0)))
        {
            SpiHidWaitFor(Hid, SpiHidWaitDescriptor, Index, SPIHID_WAIT_MS);
        }
    }
    if (!Hid->Interfaces[SPIHID_INTERFACE_KEYBOARD].DescriptorLength)
    {
        DbgPrint("spihid: the keyboard gave no report descriptor\n");
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}
