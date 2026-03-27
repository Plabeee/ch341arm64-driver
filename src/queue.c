/*
 * queue.c - WDF I/O queue setup and Read/Write routing
 */

#include "driver.h"
#include "device.h"
#include "ch341.h"
#include "queue.h"
#include "serial.h"

/* Forward declarations */
static VOID EvtWriteComplete(WDFREQUEST, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS, WDFCONTEXT);
static VOID EvtReadTimeoutTimer(WDFTIMER Timer);
static VOID EvtPendingReadCancel(WDFREQUEST Request);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, QueueInitialize)
#endif

/* ------------------------------------------------------------------ */
/* Queue initialization                                                */
/* ------------------------------------------------------------------ */

NTSTATUS
QueueInitialize(
    _In_ WDFDEVICE Device
    )
{
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PDEVICE_CONTEXT devCtx;
    WDF_TIMER_CONFIG timerConfig;
    WDF_OBJECT_ATTRIBUTES timerAttrs;

    PAGED_CODE();

    devCtx = DeviceGetContext(Device);

    /*
     * Default queue: receives all I/O requests not routed elsewhere.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = EvtIoRead;
    queueConfig.EvtIoWrite = EvtIoWrite;
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Default queue creation failed: 0x%08X", status);
        return status;
    }

    /* Create timer for read timeouts (parented to device so it's auto-cleaned) */
    WDF_TIMER_CONFIG_INIT(&timerConfig, EvtReadTimeoutTimer);
    timerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttrs);
    timerAttrs.ParentObject = Device;

    status = WdfTimerCreate(&timerConfig, &timerAttrs, &devCtx->ReadTimer);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Read timer creation failed: 0x%08X", status);
        return status;
    }

    CH341_LOG("I/O queues initialized");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* EvtIoRead - Application reads data from the serial port             */
/* ------------------------------------------------------------------ */

/*
 * Determine the total read timeout in milliseconds from SERIAL_TIMEOUTS.
 *
 * Windows serial timeout rules:
 *   - If ReadIntervalTimeout == MAXULONG && ReadTotalTimeoutMultiplier == 0
 *     && ReadTotalTimeoutConstant == 0: return immediately with available data.
 *   - If ReadIntervalTimeout == MAXULONG && ReadTotalTimeoutMultiplier == MAXULONG
 *     && ReadTotalTimeoutConstant > 0 && ReadTotalTimeoutConstant < MAXULONG:
 *     wait up to ReadTotalTimeoutConstant ms for at least 1 byte.
 *   - Otherwise: total = (ReadTotalTimeoutMultiplier * numBytes) + ReadTotalTimeoutConstant.
 *     0 means no timeout (wait forever).
 */
static ULONG
CalcReadTimeoutMs(
    _In_ const SERIAL_TIMEOUTS *T,
    _In_ ULONG NumBytes
    )
{
    /* Immediate return with available data */
    if (T->ReadIntervalTimeout == MAXULONG &&
        T->ReadTotalTimeoutMultiplier == 0 &&
        T->ReadTotalTimeoutConstant == 0) {
        return 0;
    }

    /* Wait for first byte with total timeout constant */
    if (T->ReadIntervalTimeout == MAXULONG &&
        T->ReadTotalTimeoutMultiplier == MAXULONG &&
        T->ReadTotalTimeoutConstant > 0 &&
        T->ReadTotalTimeoutConstant < MAXULONG) {
        return T->ReadTotalTimeoutConstant;
    }

    /* Standard formula */
    {
        ULONG total = T->ReadTotalTimeoutConstant;
        if (T->ReadTotalTimeoutMultiplier > 0 && NumBytes > 0) {
            /* Saturate to avoid overflow */
            if (T->ReadTotalTimeoutMultiplier > (MAXULONG - total) / NumBytes) {
                return MAXULONG;
            }
            total += T->ReadTotalTimeoutMultiplier * NumBytes;
        }
        return total;
    }
}

VOID
EvtIoRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length
    )
{
    PDEVICE_CONTEXT devCtx;
    NTSTATUS status;
    PVOID outputBuffer;
    ULONG bytesRead;
    ULONG timeoutMs;

    devCtx = DeviceGetContext(WdfIoQueueGetDevice(Queue));

    status = WdfRequestRetrieveOutputBuffer(Request, Length, &outputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    /* Try to satisfy from the ring buffer first */
    WdfSpinLockAcquire(devCtx->SerialLock);
    bytesRead = RingBufRead(&devCtx->RxBuffer, (PUCHAR)outputBuffer, (ULONG)Length);

    if (bytesRead > 0) {
        WdfSpinLockRelease(devCtx->SerialLock);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesRead);
        return;
    }

    /* No data available. Check timeout policy. */
    timeoutMs = CalcReadTimeoutMs(&devCtx->Timeouts, (ULONG)Length);

    if (timeoutMs == 0) {
        /* Immediate return mode */
        WdfSpinLockRelease(devCtx->SerialLock);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    /* Only one pending read at a time */
    if (devCtx->PendingReadRequest) {
        WdfSpinLockRelease(devCtx->SerialLock);
        WdfRequestComplete(Request, STATUS_DEVICE_BUSY);
        return;
    }

    /* Pend the request — will be completed by RX data or timer */
    devCtx->PendingReadRequest = Request;
    devCtx->PendingReadLength = Length;
    WdfSpinLockRelease(devCtx->SerialLock);

    status = WdfRequestMarkCancelableEx(Request, EvtPendingReadCancel);
    if (!NT_SUCCESS(status)) {
        WdfSpinLockAcquire(devCtx->SerialLock);
        devCtx->PendingReadRequest = NULL;
        WdfSpinLockRelease(devCtx->SerialLock);
        WdfRequestComplete(Request, status);
        return;
    }

    /* Start timeout timer */
    if (timeoutMs < MAXULONG) {
        WdfTimerStart(devCtx->ReadTimer,
                      WDF_REL_TIMEOUT_IN_MS(timeoutMs));
    }
    /* If timeoutMs == MAXULONG, wait indefinitely (no timer) */
}

/* ------------------------------------------------------------------ */
/* Complete a pending read when RX data arrives                        */
/* Called from EvtUsbBulkInReadComplete under SerialLock.               */
/* ------------------------------------------------------------------ */

VOID
QueueCompletePendingRead(
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    WDFREQUEST readReq;
    PVOID outputBuffer;
    NTSTATUS status;
    ULONG bytesRead;

    /* Caller holds SerialLock */
    readReq = DevCtx->PendingReadRequest;
    if (!readReq) {
        return;
    }

    DevCtx->PendingReadRequest = NULL;

    /* Release lock before WDF calls */
    WdfSpinLockRelease(DevCtx->SerialLock);

    /* Stop the timeout timer */
    WdfTimerStop(DevCtx->ReadTimer, FALSE);

    if (!NT_SUCCESS(WdfRequestUnmarkCancelable(readReq))) {
        /* Cancel routine owns it */
        WdfSpinLockAcquire(DevCtx->SerialLock);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(
        readReq, DevCtx->PendingReadLength, &outputBuffer, NULL);
    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(DevCtx->SerialLock);
        bytesRead = RingBufRead(&DevCtx->RxBuffer, (PUCHAR)outputBuffer,
                                (ULONG)DevCtx->PendingReadLength);
        WdfSpinLockRelease(DevCtx->SerialLock);
        WdfRequestCompleteWithInformation(readReq, STATUS_SUCCESS, bytesRead);
    } else {
        WdfRequestComplete(readReq, status);
    }

    WdfSpinLockAcquire(DevCtx->SerialLock);
}

/* ------------------------------------------------------------------ */
/* Read timeout timer callback                                         */
/* ------------------------------------------------------------------ */

static VOID
EvtReadTimeoutTimer(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT devCtx = DeviceGetContext(device);
    WDFREQUEST readReq;

    WdfSpinLockAcquire(devCtx->SerialLock);

    readReq = devCtx->PendingReadRequest;
    if (!readReq) {
        WdfSpinLockRelease(devCtx->SerialLock);
        return;
    }
    devCtx->PendingReadRequest = NULL;

    WdfSpinLockRelease(devCtx->SerialLock);

    if (!NT_SUCCESS(WdfRequestUnmarkCancelable(readReq))) {
        return; /* Cancel routine owns it */
    }

    /* Timeout expired: return whatever is in the buffer (may be 0) */
    {
        PVOID outputBuffer;
        NTSTATUS status = WdfRequestRetrieveOutputBuffer(
            readReq, devCtx->PendingReadLength, &outputBuffer, NULL);
        if (NT_SUCCESS(status)) {
            ULONG bytesRead;
            WdfSpinLockAcquire(devCtx->SerialLock);
            bytesRead = RingBufRead(&devCtx->RxBuffer, (PUCHAR)outputBuffer,
                                    (ULONG)devCtx->PendingReadLength);
            WdfSpinLockRelease(devCtx->SerialLock);
            WdfRequestCompleteWithInformation(readReq, STATUS_SUCCESS, bytesRead);
        } else {
            WdfRequestComplete(readReq, status);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pending read cancellation                                           */
/* ------------------------------------------------------------------ */

static VOID
EvtPendingReadCancel(
    _In_ WDFREQUEST Request
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));
    PDEVICE_CONTEXT devCtx = DeviceGetContext(device);

    WdfTimerStop(devCtx->ReadTimer, FALSE);

    WdfSpinLockAcquire(devCtx->SerialLock);
    if (devCtx->PendingReadRequest == Request) {
        devCtx->PendingReadRequest = NULL;
    }
    WdfSpinLockRelease(devCtx->SerialLock);

    WdfRequestComplete(Request, STATUS_CANCELLED);
}

/* ------------------------------------------------------------------ */
/* EvtIoWrite - Application writes data to the serial port             */
/* ------------------------------------------------------------------ */

VOID
EvtIoWrite(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length
    )
{
    PDEVICE_CONTEXT devCtx;
    NTSTATUS status;
    WDFMEMORY reqMemory;

    UNREFERENCED_PARAMETER(Length);

    devCtx = DeviceGetContext(WdfIoQueueGetDevice(Queue));

    status = WdfRequestRetrieveInputMemory(Request, &reqMemory);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    /*
     * Send data to the device via bulk OUT pipe.
     * Format the request and send it asynchronously.
     */
    status = WdfUsbTargetPipeFormatRequestForWrite(
        devCtx->BulkOutPipe,
        Request,
        reqMemory,
        NULL    /* Offsets */
    );
    if (!NT_SUCCESS(status)) {
        CH341_LOG("FormatRequestForWrite failed: 0x%08X", status);
        WdfRequestComplete(Request, status);
        return;
    }

    WdfRequestSetCompletionRoutine(Request, EvtWriteComplete, devCtx);

    if (!WdfRequestSend(Request, WdfUsbTargetPipeGetIoTarget(devCtx->BulkOutPipe), NULL)) {
        status = WdfRequestGetStatus(Request);
        CH341_LOG("WdfRequestSend failed: 0x%08X", status);
        WdfRequestComplete(Request, status);
    }
}

/* Write completion */
static VOID
EvtWriteComplete(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
    )
{
    NTSTATUS status;
    size_t bytesWritten;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    status = Params->IoStatus.Status;
    bytesWritten = Params->IoStatus.Information;

    if (!NT_SUCCESS(status)) {
        CH341_LOG("Write completed with error: 0x%08X", status);
    }

    WdfRequestCompleteWithInformation(Request, status, bytesWritten);
}

/* ------------------------------------------------------------------ */
/* EvtIoDeviceControl - IOCTL dispatch                                 */
/* ------------------------------------------------------------------ */

VOID
EvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    NTSTATUS status;

    status = SerialHandleIoctl(
        device,
        Request,
        IoControlCode,
        InputBufferLength,
        OutputBufferLength
    );

    if (status != STATUS_PENDING) {
        /* SerialHandleIoctl completes the request itself on non-PENDING status */
    }
}
