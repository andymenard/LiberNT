/*
 * PROJECT:     LiberNT ACPI Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     GenericSerialBus operation region handler
 *
 * A GenericSerialBus field access is forwarded the way Windows does it: the
 * field's Connection() (an I2cSerialBus/SpiSerialBus/UartSerialBus descriptor)
 * is registered with the Resource Hub, the connection is opened through
 * \Device\RESOURCE_HUB\<id> (which reparses to the controller named in the
 * descriptor), and the access is sent as IOCTL_SPB_EXECUTE_SEQUENCE: one
 * write transfer with the request bytes and, for process-call style
 * protocols, one read transfer into the ACPI status/length/data buffer.
 *
 * The handler is installed on a device's namespace node when its PDO
 * starts, and only if that scope declares GenericSerialBus regions.
 * Installing it runs _REG(GenericSerialBus, 1) for those regions.
 */

#include "precomp.h"
#include <accommon.h>
#include <acnamesp.h>
#include <amlcode.h>
#include <ntstrsafe.h>
#include <spb.h>
#include <reshub.h>

#include <debug.h>

#define ACPI_GSB_TAG                 'bGcA'
#define ACPI_GSB_OPEN_RETRY_MS       100
#define ACPI_GSB_OPEN_TIMEOUT_MS     5000
#define ACPI_GSB_IO_TIMEOUT_MS       5000

/* Status byte written to the ACPI buffer when the access could not be done. */
#define ACPI_GSB_STATUS_FAILED       1

typedef struct _ACPI_GSB_CONTEXT
{
    /* Must be first: ACPICA stores the field's Connection() here. */
    ACPI_CONNECTION_INFO Info;
    PPDO_DEVICE_DATA DeviceData;
    PFILE_OBJECT FileObject;
    PUCHAR OpenedConnection;
    USHORT OpenedConnectionLength;
} ACPI_GSB_CONTEXT, *PACPI_GSB_CONTEXT;

ULONGLONG
BuspRegisterGsbConnection(
    _In_ PPDO_DEVICE_DATA DeviceData,
    _In_ ACPI_RESOURCE *Resource,
    _Out_ PDEVICE_OBJECT *Controller);

static
VOID
AcpiGsbCloseTarget(
    _Inout_ PACPI_GSB_CONTEXT Context)
{
    if (Context->FileObject)
    {
        ObDereferenceObject(Context->FileObject);
        Context->FileObject = NULL;
    }
    if (Context->OpenedConnection)
    {
        ExFreePoolWithTag(Context->OpenedConnection, ACPI_GSB_TAG);
        Context->OpenedConnection = NULL;
        Context->OpenedConnectionLength = 0;
    }
}

static
BOOLEAN
AcpiGsbIsDriverAttached(
    _In_ PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT Top = IoGetAttachedDeviceReference(Pdo);
    BOOLEAN Attached = Top != Pdo;

    ObDereferenceObject(Top);
    return Attached;
}

static
NTSTATUS
AcpiGsbOpenTarget(
    _Inout_ PACPI_GSB_CONTEXT Context)
{
    ACPI_RESOURCE *Resource = NULL;
    PDEVICE_OBJECT Controller = NULL;
    ULONGLONG ConnectionId;
    WCHAR NameBuffer[64];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;
    LARGE_INTEGER Delay;
    ULONG Waited;
    NTSTATUS Status;

    if (Context->FileObject &&
        Context->OpenedConnectionLength == Context->Info.Length &&
        RtlCompareMemory(Context->OpenedConnection, Context->Info.Connection,
                         Context->Info.Length) == Context->Info.Length)
    {
        return STATUS_SUCCESS;
    }
    AcpiGsbCloseTarget(Context);

    if (!Context->Info.Connection || !Context->Info.Length ||
        ACPI_FAILURE(AcpiBufferToResource(Context->Info.Connection, Context->Info.Length, &Resource)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Resource->Type != ACPI_RESOURCE_TYPE_SERIAL_BUS)
    {
        ACPI_FREE(Resource);
        return STATUS_INVALID_PARAMETER;
    }

    ConnectionId = BuspRegisterGsbConnection(Context->DeviceData, Resource, &Controller);
    ACPI_FREE(Resource);
    if (!ConnectionId || !Controller)
        return STATUS_NO_SUCH_DEVICE;

    /* The controller's driver may still be starting (it usually starts right
     * after the device whose _REG enabled this region). */
    Delay.QuadPart = -10000LL * ACPI_GSB_OPEN_RETRY_MS;
    for (Waited = 0; !AcpiGsbIsDriverAttached(Controller); Waited += ACPI_GSB_OPEN_RETRY_MS)
    {
        if (Waited >= ACPI_GSB_OPEN_TIMEOUT_MS)
            return STATUS_DEVICE_NOT_READY;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    RtlStringCchPrintfW(NameBuffer, ARRAYSIZE(NameBuffer), L"%ls%08lx%08lx",
                        RESOURCE_HUB_DEVICE_NAME_PREFIX,
                        (ULONG)(ConnectionId >> 32), (ULONG)ConnectionId);
    RtlInitUnicodeString(&Name, NameBuffer);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&Handle, FILE_GENERIC_READ | FILE_GENERIC_WRITE, &Attributes, &IoStatus,
                          NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ACPI GSB: opening %wZ failed 0x%08lx\n", &Name, Status);
        return Status;
    }
    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode,
                                       (PVOID *)&Context->FileObject, NULL);
    ZwClose(Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    Context->OpenedConnection = ExAllocatePoolWithTag(NonPagedPool, Context->Info.Length, ACPI_GSB_TAG);
    if (!Context->OpenedConnection)
    {
        AcpiGsbCloseTarget(Context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(Context->OpenedConnection, Context->Info.Connection, Context->Info.Length);
    Context->OpenedConnectionLength = Context->Info.Length;
    DPRINT1("ACPI GSB: connected to %wZ\n", &Name);
    return STATUS_SUCCESS;
}

static
NTSTATUS
AcpiGsbExecute(
    _In_ PACPI_GSB_CONTEXT Context,
    _In_reads_bytes_opt_(WriteLength) PVOID WriteBuffer,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_opt_(ReadLength) PVOID ReadBuffer,
    _In_ ULONG ReadLength,
    _Out_ PULONG_PTR Information)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PDEVICE_OBJECT Target = IoGetRelatedDeviceObject(Context->FileObject);
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Timeout;
    KEVENT Event;
    ULONG Count = 0;
    PIRP Irp;
    NTSTATUS Status;

    *Information = 0;
    RtlZeroMemory(&Sequence, sizeof(Sequence));
    if (WriteBuffer && WriteLength)
        Sequence.List.Transfers[Count++] =
            SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, WriteBuffer, WriteLength);
    if (ReadBuffer && ReadLength)
        Sequence.List.Transfers[Count++] =
            SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, ReadBuffer, ReadLength);
    if (!Count)
        return STATUS_SUCCESS;
    Sequence.List.Size = sizeof(SPB_TRANSFER_LIST);
    Sequence.List.TransferCount = Count;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_SPB_EXECUTE_SEQUENCE, Target, &Sequence,
                                        FIELD_OFFSET(SPB_TRANSFER_LIST, Transfers[Count]),
                                        NULL, 0, FALSE, &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(Irp)->FileObject = Context->FileObject;

    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING)
    {
        Timeout.QuadPart = -10000LL * ACPI_GSB_IO_TIMEOUT_MS;
        if (KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
        {
            IoCancelIrp(Irp);
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        }
        Status = IoStatus.Status;
    }
    if (NT_SUCCESS(Status))
        *Information = IoStatus.Information;
    return Status;
}

static
ACPI_STATUS
NTAPI
AcpiGsbHandler(
    UINT32 Function,
    ACPI_PHYSICAL_ADDRESS Address,
    UINT32 BitWidth,
    UINT64 *Value,
    void *HandlerContext,
    void *RegionContext)
{
    PACPI_GSB_CONTEXT Context = HandlerContext;
    PUCHAR Buffer = (PUCHAR)Value;
    UCHAR Request[ACPI_MAX_GSBUS_DATA_SIZE];
    ULONG Protocol = Function >> 16;
    ULONG RequestLength;
    ULONG_PTR Transferred = 0;
    BOOLEAN ReadBack;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(BitWidth);
    UNREFERENCED_PARAMETER(RegionContext);

    if (!Context || !Buffer)
        return AE_BAD_PARAMETER;

    /* Process-call protocols write the request and read the reply into the
     * same status/length/data buffer; the others only transfer one way. */
    switch (Protocol)
    {
        case AML_FIELD_ATTRIB_PROCESS_CALL:
        case AML_FIELD_ATTRIB_BLOCK_PROCESS_CALL:
        case AML_FIELD_ATTRIB_RAW_PROCESS_BYTES:
            ReadBack = TRUE;
            break;
        default:
            ReadBack = (Function & ACPI_IO_MASK) == ACPI_READ;
            break;
    }

    RequestLength = 0;
    if ((Function & ACPI_IO_MASK) == ACPI_WRITE)
    {
        RequestLength = Buffer[1];
        if (RequestLength > sizeof(Request))
            RequestLength = sizeof(Request);
        RtlCopyMemory(Request, &Buffer[ACPI_SERIAL_HEADER_SIZE], RequestLength);
    }

    Status = AcpiGsbOpenTarget(Context);
    if (NT_SUCCESS(Status))
    {
        /* The device's reply goes after the ACPI status/length header; the
         * header describes the transfer: status 0 and the number of bytes
         * read (the request reports write + read bytes). */
        Status = AcpiGsbExecute(Context,
                                RequestLength ? Request : NULL, RequestLength,
                                ReadBack ? &Buffer[ACPI_SERIAL_HEADER_SIZE] : NULL,
                                ReadBack ? ACPI_MAX_GSBUS_DATA_SIZE : 0,
                                &Transferred);
        if (NT_SUCCESS(Status) && ReadBack)
        {
            Transferred = Transferred > RequestLength ? Transferred - RequestLength : 0;
            Buffer[0] = 0;
            Buffer[1] = (UCHAR)min(Transferred, ACPI_MAX_GSBUS_DATA_SIZE);
        }
        if (Status == STATUS_FILE_CLOSED || Status == STATUS_DELETE_PENDING ||
            Status == STATUS_NO_SUCH_DEVICE)
        {
            AcpiGsbCloseTarget(Context);
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ACPI GSB: protocol 0x%lx access failed 0x%08lx\n", Protocol, Status);
        Buffer[0] = ACPI_GSB_STATUS_FAILED;
        Buffer[1] = 0;
    }
    else if (!ReadBack)
    {
        Buffer[0] = 0;
    }

    /* Report device errors through the status byte so the AML can recover. */
    return AE_OK;
}

static
ACPI_STATUS
NTAPI
AcpiGsbFindRegion(
    ACPI_HANDLE Object,
    UINT32 NestingLevel,
    void *Context,
    void **ReturnValue)
{
    ACPI_OPERAND_OBJECT *Region;

    UNREFERENCED_PARAMETER(NestingLevel);
    UNREFERENCED_PARAMETER(ReturnValue);

    Region = AcpiNsGetAttachedObject((ACPI_NAMESPACE_NODE *)Object);
    if (Region && Region->Common.Type == ACPI_TYPE_REGION &&
        Region->Region.SpaceId == ACPI_ADR_SPACE_GSBUS)
    {
        *(PBOOLEAN)Context = TRUE;
        return AE_CTRL_TERMINATE;
    }
    return AE_OK;
}

VOID
AcpiGsbPdoStarted(
    _In_ PPDO_DEVICE_DATA DeviceData)
{
    PACPI_GSB_CONTEXT Context;
    BOOLEAN HasRegion = FALSE;
    ACPI_STATUS AcpiStatus;

    if (!DeviceData->AcpiHandle || DeviceData->GsbHandlerInstalled)
        return;

    /* Only direct children: regions declared in this device's own scope. */
    AcpiWalkNamespace(ACPI_TYPE_REGION, DeviceData->AcpiHandle, 1,
                      AcpiGsbFindRegion, NULL, &HasRegion, NULL);
    if (!HasRegion)
        return;

    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Context), ACPI_GSB_TAG);
    if (!Context)
        return;
    RtlZeroMemory(Context, sizeof(*Context));
    Context->DeviceData = DeviceData;

    AcpiStatus = AcpiInstallAddressSpaceHandler(DeviceData->AcpiHandle, ACPI_ADR_SPACE_GSBUS,
                                                AcpiGsbHandler, NULL, Context);
    if (ACPI_FAILURE(AcpiStatus))
    {
        DPRINT1("ACPI GSB: handler install failed %s\n", AcpiFormatException(AcpiStatus));
        ExFreePoolWithTag(Context, ACPI_GSB_TAG);
        return;
    }
    DeviceData->GsbHandlerInstalled = TRUE;
    DPRINT1("ACPI GSB: handler installed for %p\n", DeviceData->AcpiHandle);
}
