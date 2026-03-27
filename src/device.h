/*
 * device.h - Device context and USB lifecycle declarations
 */

#pragma once

#include "driver.h"

/* ------------------------------------------------------------------ */
/* Device context                                                      */
/* ------------------------------------------------------------------ */

typedef struct _DEVICE_CONTEXT {
    /* USB objects */
    WDFUSBDEVICE    UsbDevice;
    WDFUSBINTERFACE UsbInterface;
    WDFUSBPIPE      BulkInPipe;
    WDFUSBPIPE      BulkOutPipe;
    WDFUSBPIPE      InterruptPipe;

    /* CH341 chip state */
    UCHAR           ChipVersion;

    /* Current serial configuration */
    ULONG           CurrentBaud;
    SERIAL_LINE_CONTROL LineControl;    /* From ntddser.h */
    SERIAL_HANDFLOW HandFlow;           /* From ntddser.h */
    SERIAL_TIMEOUTS Timeouts;           /* From ntddser.h */
    BOOLEAN         DtrState;
    BOOLEAN         RtsState;

    /* Modem status (Win32 MS_* bit format) */
    ULONG           ModemStatus;
    ULONG           PreviousModemStatus;

    /* Wait mask for WaitCommEvent */
    ULONG           WaitMask;
    WDFREQUEST      PendingWaitRequest;

    /* Receive buffer */
    RING_BUFFER     RxBuffer;

    /* Synchronization */
    WDFSPINLOCK     SerialLock;

    /* COM port identity */
    ULONG           PortNumber;
    BOOLEAN         PortRegistered;

    /* Continuous reader state */
    BOOLEAN         ReaderStarted;

    /* Pending read request (waiting for RX data or timeout) */
    WDFREQUEST      PendingReadRequest;
    WDFTIMER        ReadTimer;
    size_t          PendingReadLength;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

/* ------------------------------------------------------------------ */
/* Device lifecycle callbacks                                          */
/* ------------------------------------------------------------------ */

EVT_WDF_DEVICE_PREPARE_HARDWARE     EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     EvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY             EvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT              EvtDeviceD0Exit;

/* ------------------------------------------------------------------ */
/* USB helpers                                                         */
/* ------------------------------------------------------------------ */

NTSTATUS
DeviceConfigureUsb(
    _In_ PDEVICE_CONTEXT DevCtx
    );

NTSTATUS
DeviceDiscoverEndpoints(
    _In_ PDEVICE_CONTEXT DevCtx
    );

NTSTATUS
DeviceStartContinuousReader(
    _In_ WDFDEVICE Device,
    _In_ PDEVICE_CONTEXT DevCtx
    );

VOID
DeviceStopContinuousReader(
    _In_ PDEVICE_CONTEXT DevCtx
    );
