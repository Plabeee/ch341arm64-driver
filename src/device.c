/*
 * device.c - USB lifecycle, endpoint discovery, and continuous reader
 */

#include "driver.h"
#include "device.h"
#include "ch341.h"
#include "serial.h"
#include "queue.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, EvtDevicePrepareHardware)
#pragma alloc_text(PAGE, EvtDeviceReleaseHardware)
#pragma alloc_text(PAGE, DeviceConfigureUsb)
#pragma alloc_text(PAGE, DeviceDiscoverEndpoints)
#endif

/* Forward declaration for continuous reader completion */
EVT_WDF_USB_READER_COMPLETION_ROUTINE EvtUsbBulkInReadComplete;
EVT_WDF_USB_READERS_FAILED            EvtUsbBulkInReaderFailed;

/* ------------------------------------------------------------------ */
/* EvtDevicePrepareHardware                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
EvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    NTSTATUS status;
    PDEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    CH341_LOG("EvtDevicePrepareHardware");

    devCtx = DeviceGetContext(Device);

    /* Create USB device object */
    status = DeviceConfigureUsb(devCtx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Discover bulk and interrupt endpoints */
    status = DeviceDiscoverEndpoints(devCtx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Start continuous reader on bulk IN pipe */
    status = DeviceStartContinuousReader(Device, devCtx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* EvtDeviceReleaseHardware                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
EvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    CH341_LOG("EvtDeviceReleaseHardware");

    devCtx = DeviceGetContext(Device);

    /* Unregister COM port from SERIALCOMM device map */
    if (devCtx->PortRegistered) {
        SerialMapComPort(devCtx, FALSE);
    }

    RingBufFree(&devCtx->RxBuffer);

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* EvtDeviceD0Entry - Hardware is powered, initialize CH341            */
/* ------------------------------------------------------------------ */

NTSTATUS
EvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    NTSTATUS status;
    PDEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(PreviousState);

    CH341_LOG("EvtDeviceD0Entry (previous state: %d)", PreviousState);

    devCtx = DeviceGetContext(Device);

    /* Run CH341 initialization sequence */
    status = Ch341InitHardware(devCtx);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Ch341InitHardware failed: 0x%08X", status);
        return status;
    }

    /* Register COM port in SERIALCOMM device map */
    if (devCtx->PortRegistered) {
        SerialMapComPort(devCtx, TRUE);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* EvtDeviceD0Exit - Device leaving D0 (powered) state                 */
/* ------------------------------------------------------------------ */

NTSTATUS
EvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(TargetState);

    CH341_LOG("EvtDeviceD0Exit (target state: %d)", TargetState);

    devCtx = DeviceGetContext(Device);

    /* Cancel pending read and purge the RX buffer on power-down */
    WdfTimerStop(devCtx->ReadTimer, FALSE);
    WdfSpinLockAcquire(devCtx->SerialLock);
    if (devCtx->PendingReadRequest) {
        WDFREQUEST readReq = devCtx->PendingReadRequest;
        devCtx->PendingReadRequest = NULL;
        WdfSpinLockRelease(devCtx->SerialLock);
        if (NT_SUCCESS(WdfRequestUnmarkCancelable(readReq))) {
            WdfRequestComplete(readReq, STATUS_CANCELLED);
        }
        WdfSpinLockAcquire(devCtx->SerialLock);
    }
    RingBufPurge(&devCtx->RxBuffer);
    WdfSpinLockRelease(devCtx->SerialLock);

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* DeviceConfigureUsb                                                  */
/* ------------------------------------------------------------------ */

NTSTATUS
DeviceConfigureUsb(
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    WDF_USB_DEVICE_CREATE_CONFIG usbConfig;
    WDFDEVICE device;

    PAGED_CODE();

    device = WdfObjectContextGetObject(DevCtx);

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&usbConfig, USBD_CLIENT_CONTRACT_VERSION_602);

    status = WdfUsbTargetDeviceCreateWithParameters(
        device,
        &usbConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &DevCtx->UsbDevice
    );
    if (!NT_SUCCESS(status)) {
        CH341_LOG("WdfUsbTargetDeviceCreateWithParameters failed: 0x%08X", status);
        return status;
    }

    /* Select the default configuration (configuration 1, interface 0) */
    {
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

        status = WdfUsbTargetDeviceSelectConfig(
            DevCtx->UsbDevice,
            WDF_NO_OBJECT_ATTRIBUTES,
            &configParams
        );
        if (!NT_SUCCESS(status)) {
            CH341_LOG("WdfUsbTargetDeviceSelectConfig failed: 0x%08X", status);
            return status;
        }

        DevCtx->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
    }

    CH341_LOG("USB device configured");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* DeviceDiscoverEndpoints                                             */
/* ------------------------------------------------------------------ */

NTSTATUS
DeviceDiscoverEndpoints(
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    UCHAR pipeCount;
    UCHAR i;

    PAGED_CODE();

    DevCtx->BulkInPipe = NULL;
    DevCtx->BulkOutPipe = NULL;
    DevCtx->InterruptPipe = NULL;

    pipeCount = WdfUsbInterfaceGetNumConfiguredPipes(DevCtx->UsbInterface);
    CH341_LOG("Found %d configured pipes", pipeCount);

    for (i = 0; i < pipeCount; i++) {
        WDFUSBPIPE pipe;
        WDF_USB_PIPE_INFORMATION pipeInfo;

        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        pipe = WdfUsbInterfaceGetConfiguredPipe(DevCtx->UsbInterface, i, &pipeInfo);

        CH341_LOG("  Pipe %d: type=%d, endpoint=0x%02X, maxPacket=%d",
                  i, pipeInfo.PipeType, pipeInfo.EndpointAddress,
                  pipeInfo.MaximumPacketSize);

        switch (pipeInfo.PipeType) {
        case WdfUsbPipeTypeBulk:
            if (USB_ENDPOINT_DIRECTION_IN(pipeInfo.EndpointAddress)) {
                DevCtx->BulkInPipe = pipe;
                CH341_LOG("  -> Bulk IN pipe");
            } else {
                DevCtx->BulkOutPipe = pipe;
                CH341_LOG("  -> Bulk OUT pipe");
            }
            break;

        case WdfUsbPipeTypeInterrupt:
            if (USB_ENDPOINT_DIRECTION_IN(pipeInfo.EndpointAddress)) {
                DevCtx->InterruptPipe = pipe;
                CH341_LOG("  -> Interrupt IN pipe");
            }
            break;

        default:
            CH341_LOG("  -> Ignored (type %d)", pipeInfo.PipeType);
            break;
        }
    }

    /* Validate we found the expected endpoints */
    if (!DevCtx->BulkInPipe || !DevCtx->BulkOutPipe) {
        CH341_LOG("Missing required bulk endpoints");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Interrupt pipe is optional but expected */
    if (!DevCtx->InterruptPipe) {
        CH341_LOG("WARNING: No interrupt IN pipe found (modem status via polling)");
    }

    /* Disable short packet check on bulk reads - CH341 may send partial packets */
    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(DevCtx->BulkInPipe);

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Continuous reader on bulk IN                                        */
/* ------------------------------------------------------------------ */

NTSTATUS
DeviceStartContinuousReader(
    _In_ WDFDEVICE Device,
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &readerConfig,
        EvtUsbBulkInReadComplete,
        DevCtx,
        64  /* Transfer length - typical CH341 max packet size */
    );

    readerConfig.EvtUsbTargetPipeReadersFailed = EvtUsbBulkInReaderFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(
        DevCtx->BulkInPipe,
        &readerConfig
    );
    if (!NT_SUCCESS(status)) {
        CH341_LOG("WdfUsbTargetPipeConfigContinuousReader failed: 0x%08X", status);
        return status;
    }

    DevCtx->ReaderStarted = TRUE;
    CH341_LOG("Continuous reader configured on bulk IN pipe");
    return STATUS_SUCCESS;
}

VOID
DeviceStopContinuousReader(
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    DevCtx->ReaderStarted = FALSE;
}

/* ------------------------------------------------------------------ */
/* Continuous reader completion - data received from device             */
/* ------------------------------------------------------------------ */

VOID
EvtUsbBulkInReadComplete(
    _In_ WDFUSBPIPE Pipe,
    _In_ WDFMEMORY  Buffer,
    _In_ size_t     NumBytesTransferred,
    _In_ WDFCONTEXT Context
    )
{
    PDEVICE_CONTEXT devCtx = (PDEVICE_CONTEXT)Context;
    PUCHAR data;
    ULONG written;

    UNREFERENCED_PARAMETER(Pipe);

    if (NumBytesTransferred == 0) {
        return;
    }

    data = (PUCHAR)WdfMemoryGetBuffer(Buffer, NULL);

    WdfSpinLockAcquire(devCtx->SerialLock);

    written = RingBufWrite(&devCtx->RxBuffer, data, (ULONG)NumBytesTransferred);

    /* Complete a pending read request if one is waiting */
    if (written > 0 && devCtx->PendingReadRequest) {
        QueueCompletePendingRead(devCtx);
        /* Note: QueueCompletePendingRead releases and re-acquires the lock */
    }

    /* Check if we should signal WAIT_ON_MASK for EV_RXCHAR */
    if (written > 0 && (devCtx->WaitMask & EV_RXCHAR) && devCtx->PendingWaitRequest) {
        WDFREQUEST waitReq = devCtx->PendingWaitRequest;
        devCtx->PendingWaitRequest = NULL;

        WdfSpinLockRelease(devCtx->SerialLock);

        /* Unmark cancelable before completing — if the cancel routine
         * already claimed this request, STATUS_CANCELLED is returned
         * and we must not touch it. */
        if (NT_SUCCESS(WdfRequestUnmarkCancelable(waitReq))) {
            PULONG eventMask;
            NTSTATUS status = WdfRequestRetrieveOutputBuffer(
                waitReq, sizeof(ULONG), (PVOID *)&eventMask, NULL);
            if (NT_SUCCESS(status)) {
                *eventMask = EV_RXCHAR;
                WdfRequestCompleteWithInformation(waitReq, STATUS_SUCCESS, sizeof(ULONG));
            } else {
                WdfRequestComplete(waitReq, status);
            }
        }
        return;
    }

    WdfSpinLockRelease(devCtx->SerialLock);

    if (written < (ULONG)NumBytesTransferred) {
        CH341_LOG("RX buffer overflow: lost %lu bytes",
                  (ULONG)NumBytesTransferred - written);
    }
}

BOOLEAN
EvtUsbBulkInReaderFailed(
    _In_ WDFUSBPIPE    Pipe,
    _In_ NTSTATUS      Status,
    _In_ USBD_STATUS   UsbdStatus
    )
{
    UNREFERENCED_PARAMETER(Pipe);

    CH341_LOG("Continuous reader failed: NTSTATUS=0x%08X, USBD=0x%08X",
              Status, UsbdStatus);

    /* Return TRUE to have the framework reset the pipe and restart the reader */
    return TRUE;
}
