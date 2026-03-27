/*
 * serial.c - Serial (COM port) IOCTL compatibility layer
 *
 * Implements the IOCTL_SERIAL_* contract from ntddser.h so that Win32
 * serial APIs (CreateFile, SetCommState, GetCommState, WaitCommEvent,
 * SetCommTimeouts, PurgeComm, EscapeCommFunction) work against this device.
 */

#include "driver.h"
#include "device.h"
#include "ch341.h"
#include "serial.h"

/* Forward declarations */
static VOID EvtWaitOnMaskCancel(WDFREQUEST Request);

/* ------------------------------------------------------------------ */
/* COM port registration                                               */
/* ------------------------------------------------------------------ */

NTSTATUS
SerialRegisterComPort(
    _In_ WDFDEVICE Device,
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(comPortInterface, L"");
    WDFKEY hKey = NULL;
    DECLARE_CONST_UNICODE_STRING(portNameValue, L"PortName");
    WCHAR portNameBuf[16] = { 0 };
    UNICODE_STRING portNameStr;
    UNICODE_STRING symLinkStr;
    WCHAR symLinkBuf[32];
    BOOLEAN portNameExists = FALSE;

    /* Register the COM port device interface */
    status = WdfDeviceCreateDeviceInterface(
        Device,
        (LPGUID)&GUID_DEVINTERFACE_COMPORT,
        &comPortInterface
    );
    if (!NT_SUCCESS(status)) {
        CH341_LOG("WdfDeviceCreateDeviceInterface failed: 0x%08X", status);
        return status;
    }

    /*
     * Check if PortName already exists in the device registry (from a
     * previous install). Only write a default if it's missing, so the
     * port number stays stable across driver updates.
     */
    status = WdfDeviceOpenRegistryKey(
        Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES, &hKey);
    if (NT_SUCCESS(status)) {
        portNameStr.Buffer = portNameBuf;
        portNameStr.Length = 0;
        portNameStr.MaximumLength = sizeof(portNameBuf);

        status = WdfRegistryQueryUnicodeString(hKey, &portNameValue, NULL, &portNameStr);
        if (NT_SUCCESS(status) && portNameStr.Length > 0) {
            portNameExists = TRUE;
        }

        if (!portNameExists) {
            DECLARE_CONST_UNICODE_STRING(defaultPort, L"COM10");
            WdfRegistryAssignUnicodeString(hKey, &portNameValue, &defaultPort);
            RtlStringCbCopyW(portNameBuf, sizeof(portNameBuf), L"COM10");
            portNameStr.Length = 10; /* 5 chars * 2 bytes */
        }

        WdfRegistryClose(hKey);
    } else {
        RtlStringCbCopyW(portNameBuf, sizeof(portNameBuf), L"COM10");
    }

    /* Null-terminate */
    portNameBuf[sizeof(portNameBuf) / sizeof(WCHAR) - 1] = L'\0';

    /* Parse port number from name */
    DevCtx->PortNumber = 10; /* default */
    {
        WCHAR *p = portNameBuf + 3; /* skip "COM" */
        ULONG num = 0;
        while (*p >= L'0' && *p <= L'9') {
            num = num * 10 + (*p - L'0');
            p++;
        }
        if (num > 0) DevCtx->PortNumber = num;
    }

    /* Create \DosDevices\COMx symbolic link so CreateFile("COMx") works */
    RtlStringCbPrintfW(symLinkBuf, sizeof(symLinkBuf), L"\\DosDevices\\%ws", portNameBuf);
    RtlInitUnicodeString(&symLinkStr, symLinkBuf);

    status = WdfDeviceCreateSymbolicLink(Device, &symLinkStr);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("WdfDeviceCreateSymbolicLink(%ws) failed: 0x%08X", symLinkBuf, status);
        /* Non-fatal: device interface still works */
    } else {
        CH341_LOG("Symbolic link created: %ws", symLinkBuf);
    }

    DevCtx->PortRegistered = TRUE;
    CH341_LOG("COM port registered as %ws", portNameBuf);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IOCTL dispatch                                                      */
/* ------------------------------------------------------------------ */

NTSTATUS
SerialHandleIoctl(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength
    )
{
    PDEVICE_CONTEXT devCtx = DeviceGetContext(Device);
    NTSTATUS status = STATUS_SUCCESS;
    size_t info = 0;

    switch (IoControlCode) {

    /* ============================================================== */
    /* Baud rate                                                       */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_BAUD_RATE:
    {
        PSERIAL_BAUD_RATE baudRate;

        if (InputBufferLength < sizeof(SERIAL_BAUD_RATE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(SERIAL_BAUD_RATE), (PVOID *)&baudRate, NULL);
        if (!NT_SUCCESS(status)) break;

        status = Ch341SetBaudRate(devCtx, baudRate->BaudRate);
        break;
    }

    case IOCTL_SERIAL_GET_BAUD_RATE:
    {
        PSERIAL_BAUD_RATE baudRate;

        if (OutputBufferLength < sizeof(SERIAL_BAUD_RATE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(SERIAL_BAUD_RATE), (PVOID *)&baudRate, NULL);
        if (!NT_SUCCESS(status)) break;

        baudRate->BaudRate = devCtx->CurrentBaud;
        info = sizeof(SERIAL_BAUD_RATE);
        break;
    }

    /* ============================================================== */
    /* Line control                                                    */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_LINE_CONTROL:
    {
        PSERIAL_LINE_CONTROL lineCtl;

        if (InputBufferLength < sizeof(SERIAL_LINE_CONTROL)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(SERIAL_LINE_CONTROL), (PVOID *)&lineCtl, NULL);
        if (!NT_SUCCESS(status)) break;

        status = Ch341SetLineControl(
            devCtx,
            lineCtl->WordLength,
            lineCtl->Parity,
            lineCtl->StopBits
        );
        break;
    }

    case IOCTL_SERIAL_GET_LINE_CONTROL:
    {
        PSERIAL_LINE_CONTROL lineCtl;

        if (OutputBufferLength < sizeof(SERIAL_LINE_CONTROL)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(SERIAL_LINE_CONTROL), (PVOID *)&lineCtl, NULL);
        if (!NT_SUCCESS(status)) break;

        *lineCtl = devCtx->LineControl;
        info = sizeof(SERIAL_LINE_CONTROL);
        break;
    }

    /* ============================================================== */
    /* Timeouts                                                        */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_TIMEOUTS:
    {
        PSERIAL_TIMEOUTS timeouts;

        if (InputBufferLength < sizeof(SERIAL_TIMEOUTS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(SERIAL_TIMEOUTS), (PVOID *)&timeouts, NULL);
        if (!NT_SUCCESS(status)) break;

        devCtx->Timeouts = *timeouts;
        CH341_LOG("Timeouts set: RI=%lu, RTM=%lu, RTC=%lu, WTM=%lu, WTC=%lu",
                  timeouts->ReadIntervalTimeout,
                  timeouts->ReadTotalTimeoutMultiplier,
                  timeouts->ReadTotalTimeoutConstant,
                  timeouts->WriteTotalTimeoutMultiplier,
                  timeouts->WriteTotalTimeoutConstant);
        break;
    }

    case IOCTL_SERIAL_GET_TIMEOUTS:
    {
        PSERIAL_TIMEOUTS timeouts;

        if (OutputBufferLength < sizeof(SERIAL_TIMEOUTS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(SERIAL_TIMEOUTS), (PVOID *)&timeouts, NULL);
        if (!NT_SUCCESS(status)) break;

        *timeouts = devCtx->Timeouts;
        info = sizeof(SERIAL_TIMEOUTS);
        break;
    }

    /* ============================================================== */
    /* Handshake / flow control                                        */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_HANDFLOW:
    {
        PSERIAL_HANDFLOW handflow;
        BOOLEAN rtsCts;

        if (InputBufferLength < sizeof(SERIAL_HANDFLOW)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(SERIAL_HANDFLOW), (PVOID *)&handflow, NULL);
        if (!NT_SUCCESS(status)) break;

        /* Determine if RTS/CTS hardware flow control is requested */
        rtsCts = (handflow->FlowReplace & SERIAL_RTS_HANDSHAKE) &&
                 (handflow->ControlHandShake & SERIAL_CTS_HANDSHAKE);

        status = Ch341SetFlowControl(devCtx, rtsCts);
        if (NT_SUCCESS(status)) {
            devCtx->HandFlow = *handflow;
        }
        break;
    }

    case IOCTL_SERIAL_GET_HANDFLOW:
    {
        PSERIAL_HANDFLOW handflow;

        if (OutputBufferLength < sizeof(SERIAL_HANDFLOW)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(SERIAL_HANDFLOW), (PVOID *)&handflow, NULL);
        if (!NT_SUCCESS(status)) break;

        *handflow = devCtx->HandFlow;
        info = sizeof(SERIAL_HANDFLOW);
        break;
    }

    /* ============================================================== */
    /* DTR / RTS                                                       */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_DTR:
        status = Ch341SetModemControl(devCtx, TRUE, devCtx->RtsState);
        break;

    case IOCTL_SERIAL_CLR_DTR:
        status = Ch341SetModemControl(devCtx, FALSE, devCtx->RtsState);
        break;

    case IOCTL_SERIAL_SET_RTS:
        status = Ch341SetModemControl(devCtx, devCtx->DtrState, TRUE);
        break;

    case IOCTL_SERIAL_CLR_RTS:
        status = Ch341SetModemControl(devCtx, devCtx->DtrState, FALSE);
        break;

    /* ============================================================== */
    /* Modem status                                                    */
    /* ============================================================== */

    case IOCTL_SERIAL_GET_MODEMSTATUS:
    {
        PULONG modemStatus;

        if (OutputBufferLength < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(ULONG), (PVOID *)&modemStatus, NULL);
        if (!NT_SUCCESS(status)) break;

        status = Ch341GetModemStatus(devCtx, modemStatus);
        if (NT_SUCCESS(status)) {
            devCtx->ModemStatus = *modemStatus;
            info = sizeof(ULONG);
        }
        break;
    }

    /* ============================================================== */
    /* Wait mask / WaitCommEvent                                       */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_WAIT_MASK:
    {
        PULONG mask;

        if (InputBufferLength < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(ULONG), (PVOID *)&mask, NULL);
        if (!NT_SUCCESS(status)) break;

        WdfSpinLockAcquire(devCtx->SerialLock);

        /* Cancel any pending WAIT_ON_MASK if the mask changes */
        if (devCtx->PendingWaitRequest) {
            WDFREQUEST pendingReq = devCtx->PendingWaitRequest;
            devCtx->PendingWaitRequest = NULL;
            WdfSpinLockRelease(devCtx->SerialLock);

            if (NT_SUCCESS(WdfRequestUnmarkCancelable(pendingReq))) {
                WdfRequestComplete(pendingReq, STATUS_SUCCESS);
            }

            WdfSpinLockAcquire(devCtx->SerialLock);
        }

        devCtx->WaitMask = *mask;
        WdfSpinLockRelease(devCtx->SerialLock);

        CH341_LOG("Wait mask set: 0x%08lX", *mask);
        break;
    }

    case IOCTL_SERIAL_GET_WAIT_MASK:
    {
        PULONG mask;

        if (OutputBufferLength < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(ULONG), (PVOID *)&mask, NULL);
        if (!NT_SUCCESS(status)) break;

        *mask = devCtx->WaitMask;
        info = sizeof(ULONG);
        break;
    }

    case IOCTL_SERIAL_WAIT_ON_MASK:
    {
        if (OutputBufferLength < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        WdfSpinLockAcquire(devCtx->SerialLock);

        if (devCtx->WaitMask == 0) {
            WdfSpinLockRelease(devCtx->SerialLock);
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        /* Only one WAIT_ON_MASK can be pending at a time */
        if (devCtx->PendingWaitRequest) {
            WdfSpinLockRelease(devCtx->SerialLock);
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        /*
         * Check if any waited-for condition is already true.
         * For now, just check EV_RXCHAR (data in buffer).
         */
        if ((devCtx->WaitMask & EV_RXCHAR) && RingBufAvailable(&devCtx->RxBuffer) > 0) {
            WdfSpinLockRelease(devCtx->SerialLock);

            PULONG eventMask;
            status = WdfRequestRetrieveOutputBuffer(
                Request, sizeof(ULONG), (PVOID *)&eventMask, NULL);
            if (NT_SUCCESS(status)) {
                *eventMask = EV_RXCHAR;
                info = sizeof(ULONG);
            }
            break;
        }

        /* Pend the request - will be completed when an event fires */
        devCtx->PendingWaitRequest = Request;
        WdfSpinLockRelease(devCtx->SerialLock);

        /* Mark the request as pending and return STATUS_PENDING */
        status = WdfRequestMarkCancelableEx(Request, EvtWaitOnMaskCancel);
        if (!NT_SUCCESS(status)) {
            WdfSpinLockAcquire(devCtx->SerialLock);
            devCtx->PendingWaitRequest = NULL;
            WdfSpinLockRelease(devCtx->SerialLock);
            break;
        }

        /* Don't complete the request - it's pending */
        return STATUS_PENDING;
    }

    /* ============================================================== */
    /* Comm status                                                     */
    /* ============================================================== */

    case IOCTL_SERIAL_GET_COMMSTATUS:
    {
        PSERIAL_STATUS serialStatus;

        if (OutputBufferLength < sizeof(SERIAL_STATUS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(SERIAL_STATUS), (PVOID *)&serialStatus, NULL);
        if (!NT_SUCCESS(status)) break;

        RtlZeroMemory(serialStatus, sizeof(SERIAL_STATUS));

        WdfSpinLockAcquire(devCtx->SerialLock);
        serialStatus->AmountInInQueue = RingBufAvailable(&devCtx->RxBuffer);
        WdfSpinLockRelease(devCtx->SerialLock);

        serialStatus->AmountInOutQueue = 0;
        info = sizeof(SERIAL_STATUS);
        break;
    }

    /* ============================================================== */
    /* Purge                                                           */
    /* ============================================================== */

    case IOCTL_SERIAL_PURGE:
    {
        PULONG purgeMask;

        if (InputBufferLength < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(ULONG), (PVOID *)&purgeMask, NULL);
        if (!NT_SUCCESS(status)) break;

        if (*purgeMask & SERIAL_PURGE_RXCLEAR) {
            WdfSpinLockAcquire(devCtx->SerialLock);
            RingBufPurge(&devCtx->RxBuffer);
            WdfSpinLockRelease(devCtx->SerialLock);
            CH341_LOG("RX buffer purged");
        }

        if (*purgeMask & SERIAL_PURGE_TXCLEAR) {
            /* TODO: Cancel pending write requests */
            CH341_LOG("TX purge requested (pending writes not yet cancelled)");
        }

        if (*purgeMask & SERIAL_PURGE_RXABORT) {
            /* TODO: Cancel pending read requests */
            CH341_LOG("RX abort requested");
        }

        if (*purgeMask & SERIAL_PURGE_TXABORT) {
            /* TODO: Cancel pending write requests */
            CH341_LOG("TX abort requested");
        }

        break;
    }

    /* ============================================================== */
    /* Port properties                                                 */
    /* ============================================================== */

    case IOCTL_SERIAL_GET_PROPERTIES:
    {
        PSERIAL_COMMPROP props;

        if (OutputBufferLength < sizeof(SERIAL_COMMPROP)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(SERIAL_COMMPROP), (PVOID *)&props, NULL);
        if (!NT_SUCCESS(status)) break;

        RtlZeroMemory(props, sizeof(SERIAL_COMMPROP));
        props->PacketLength = sizeof(SERIAL_COMMPROP);
        props->PacketVersion = 2;
        props->ServiceMask = SERIAL_SP_SERIALCOMM;
        props->MaxTxQueue = 0;    /* No limit */
        props->MaxRxQueue = 0;
        props->MaxBaud = SERIAL_BAUD_USER;
        props->ProvSubType = SERIAL_SP_RS232;
        props->ProvCapabilities =
            SERIAL_PCF_DTRDSR |
            SERIAL_PCF_RTSCTS |
            SERIAL_PCF_TOTALTIMEOUTS |
            SERIAL_PCF_INTTIMEOUTS |
            SERIAL_PCF_PARITY_CHECK |
            SERIAL_PCF_SETXCHAR;
        props->SettableParams =
            SERIAL_SP_PARITY |
            SERIAL_SP_BAUD |
            SERIAL_SP_DATABITS |
            SERIAL_SP_STOPBITS |
            SERIAL_SP_HANDSHAKING |
            SERIAL_SP_PARITY_CHECK;
        props->SettableBaud =
            SERIAL_BAUD_075 | SERIAL_BAUD_110 | SERIAL_BAUD_150 |
            SERIAL_BAUD_300 | SERIAL_BAUD_600 | SERIAL_BAUD_1200 |
            SERIAL_BAUD_1800 | SERIAL_BAUD_2400 | SERIAL_BAUD_4800 |
            SERIAL_BAUD_7200 | SERIAL_BAUD_9600 | SERIAL_BAUD_14400 |
            SERIAL_BAUD_19200 | SERIAL_BAUD_38400 | SERIAL_BAUD_56K |
            SERIAL_BAUD_57600 | SERIAL_BAUD_115200 | SERIAL_BAUD_128K |
            SERIAL_BAUD_USER;
        props->SettableData =
            SERIAL_DATABITS_5 | SERIAL_DATABITS_6 |
            SERIAL_DATABITS_7 | SERIAL_DATABITS_8;
        props->SettableStopParity =
            SERIAL_STOPBITS_10 | SERIAL_STOPBITS_20 |
            SERIAL_PARITY_NONE | SERIAL_PARITY_ODD |
            SERIAL_PARITY_EVEN | SERIAL_PARITY_MARK |
            SERIAL_PARITY_SPACE;
        props->CurrentTxQueue = 0;
        props->CurrentRxQueue = RING_BUF_DEFAULT_SIZE;

        info = sizeof(SERIAL_COMMPROP);
        break;
    }

    /* ============================================================== */
    /* Break                                                           */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_BREAK_ON:
        status = Ch341SetBreak(devCtx, TRUE);
        break;

    case IOCTL_SERIAL_SET_BREAK_OFF:
        status = Ch341SetBreak(devCtx, FALSE);
        break;

    /* ============================================================== */
    /* Unimplemented but non-fatal IOCTLs                              */
    /* ============================================================== */

    case IOCTL_SERIAL_SET_QUEUE_SIZE:
        /* Ignore - we use a fixed ring buffer */
        CH341_LOG("IOCTL_SERIAL_SET_QUEUE_SIZE (ignored)");
        break;

    case IOCTL_SERIAL_SET_CHARS:
        /* XON/XOFF characters - not supported by CH341 hardware flow control */
        CH341_LOG("IOCTL_SERIAL_SET_CHARS (ignored)");
        break;

    case IOCTL_SERIAL_GET_CHARS:
    {
        PSERIAL_CHARS chars;
        if (OutputBufferLength < sizeof(SERIAL_CHARS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(SERIAL_CHARS), (PVOID *)&chars, NULL);
        if (!NT_SUCCESS(status)) break;
        RtlZeroMemory(chars, sizeof(SERIAL_CHARS));
        info = sizeof(SERIAL_CHARS);
        break;
    }

    case IOCTL_SERIAL_RESET_DEVICE:
        CH341_LOG("IOCTL_SERIAL_RESET_DEVICE (ignored)");
        break;

    /* ============================================================== */
    /* Unknown IOCTL                                                   */
    /* ============================================================== */

    default:
        CH341_LOG("Unknown IOCTL: 0x%08lX", IoControlCode);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
    return status;
}

/* ------------------------------------------------------------------ */
/* WAIT_ON_MASK cancellation                                           */
/* ------------------------------------------------------------------ */

static VOID
EvtWaitOnMaskCancel(
    _In_ WDFREQUEST Request
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));
    PDEVICE_CONTEXT devCtx = DeviceGetContext(device);

    WdfSpinLockAcquire(devCtx->SerialLock);
    if (devCtx->PendingWaitRequest == Request) {
        devCtx->PendingWaitRequest = NULL;
    }
    WdfSpinLockRelease(devCtx->SerialLock);

    WdfRequestComplete(Request, STATUS_CANCELLED);
}

/* ------------------------------------------------------------------ */
/* Modem status change notification                                    */
/* ------------------------------------------------------------------ */

VOID
SerialNotifyModemStatusChange(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ ULONG NewModemStatus
    )
{
    ULONG changedBits;
    ULONG eventMask = 0;
    WDFREQUEST waitReq = NULL;
    ULONG matchedEvents;

    WdfSpinLockAcquire(DevCtx->SerialLock);

    changedBits = DevCtx->ModemStatus ^ NewModemStatus;
    DevCtx->PreviousModemStatus = DevCtx->ModemStatus;
    DevCtx->ModemStatus = NewModemStatus;

    /* Map modem status changes to serial events */
    if (changedBits & SERIAL_MSR_CTS)  eventMask |= EV_CTS;
    if (changedBits & SERIAL_MSR_DSR)  eventMask |= EV_DSR;
    if (changedBits & SERIAL_MSR_RI)   eventMask |= EV_RING;
    if (changedBits & SERIAL_MSR_DCD)  eventMask |= EV_RLSD;

    matchedEvents = eventMask & DevCtx->WaitMask;
    if (matchedEvents && DevCtx->PendingWaitRequest) {
        waitReq = DevCtx->PendingWaitRequest;
        DevCtx->PendingWaitRequest = NULL;
    }

    WdfSpinLockRelease(DevCtx->SerialLock);

    /* Complete outside the lock */
    if (waitReq && NT_SUCCESS(WdfRequestUnmarkCancelable(waitReq))) {
        PULONG outputMask;
        NTSTATUS status = WdfRequestRetrieveOutputBuffer(
            waitReq, sizeof(ULONG), (PVOID *)&outputMask, NULL);
        if (NT_SUCCESS(status)) {
            *outputMask = matchedEvents;
            WdfRequestCompleteWithInformation(waitReq, STATUS_SUCCESS, sizeof(ULONG));
        } else {
            WdfRequestComplete(waitReq, status);
        }
    }
}

/* ------------------------------------------------------------------ */
/* SERIALCOMM device map registration                                  */
/* ------------------------------------------------------------------ */

/*
 * Register or unregister the COM port in the SERIALCOMM device map.
 * Win32 SerialPort.GetPortNames() and mode.com read from
 * HKLM\HARDWARE\DEVICEMAP\SERIALCOMM to discover COM ports.
 *
 * The value name is the NT device name (e.g., \Device\ch341arm64_0)
 * and the data is the DOS port name (e.g., COM10).
 */
/*
 * Read the actual PortName from the device's hardware registry key.
 * Windows may override our default "COM10" with the next available port.
 */
static NTSTATUS
SerialReadPortName(
    _In_  WDFDEVICE Device,
    _Out_writes_(MaxLen) WCHAR *PortName,
    _In_  ULONG MaxLen
    )
{
    NTSTATUS status;
    WDFKEY hKey;
    UNICODE_STRING valueName;
    UNICODE_STRING valueData;

    RtlInitUnicodeString(&valueName, L"PortName");
    valueData.Buffer = PortName;
    valueData.Length = 0;
    valueData.MaximumLength = (USHORT)(MaxLen * sizeof(WCHAR));

    status = WdfDeviceOpenRegistryKey(
        Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
        WDF_NO_OBJECT_ATTRIBUTES, &hKey);
    if (!NT_SUCCESS(status)) return status;

    status = WdfRegistryQueryUnicodeString(hKey, &valueName, NULL, &valueData);
    WdfRegistryClose(hKey);
    return status;
}

VOID
SerialMapComPort(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Register
    )
{
    NTSTATUS status;
    WCHAR valueName[64];

    /* Build a unique value name for the device map entry */
    status = RtlStringCbPrintfW(valueName, sizeof(valueName),
        L"\\Device\\ch341arm64_%lu", DevCtx->PortNumber);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("SerialMapComPort: failed to format value name");
        return;
    }

    if (Register) {
        WCHAR portName[16] = { 0 };
        WDFDEVICE device = WdfObjectContextGetObject(DevCtx);

        /* Read the actual port name Windows assigned */
        status = SerialReadPortName(device, portName, sizeof(portName) / sizeof(WCHAR));
        if (!NT_SUCCESS(status)) {
            /* Fallback to our default */
            RtlStringCbPrintfW(portName, sizeof(portName), L"COM%lu", DevCtx->PortNumber);
        }

        status = RtlWriteRegistryValue(
            RTL_REGISTRY_DEVICEMAP,
            L"SERIALCOMM",
            valueName,
            REG_SZ,
            portName,
            (ULONG)((wcslen(portName) + 1) * sizeof(WCHAR))
        );
        if (NT_SUCCESS(status)) {
            CH341_LOG("Registered %ws in SERIALCOMM device map", portName);
        } else {
            CH341_LOG("Failed to register in SERIALCOMM: 0x%08X", status);
        }
    } else {
        RtlDeleteRegistryValue(
            RTL_REGISTRY_DEVICEMAP,
            L"SERIALCOMM",
            valueName
        );
        CH341_LOG("Unregistered from SERIALCOMM device map");
    }
}
