/*
 * driver.c - DriverEntry and EvtDeviceAdd for CH341 ARM64 KMDF driver
 */

#include <initguid.h>
#include "driver.h"
#include "device.h"
#include "queue.h"
#include "serial.h"

/* GUID_DEVINTERFACE_COMPORT from ntddser.h needs INITGUID defined once */

/* Forward declarations - must precede #pragma alloc_text */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, EvtDeviceAdd)
#endif

/* ------------------------------------------------------------------ */
/* DriverEntry                                                         */
/* ------------------------------------------------------------------ */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;

    CH341_LOG("DriverEntry");

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        CH341_LOG("WdfDriverCreate failed: 0x%08X", status);
    }

    return status;
}

/* ------------------------------------------------------------------ */
/* EvtDeviceAdd                                                        */
/* ------------------------------------------------------------------ */

NTSTATUS
EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDFDEVICE device;
    PDEVICE_CONTEXT devCtx;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    CH341_LOG("EvtDeviceAdd");

    /* Set device type to serial port */
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_SERIAL_PORT);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);

    /* Register PnP/power callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = EvtDeviceD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    /* Create device with context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    devCtx = DeviceGetContext(device);
    RtlZeroMemory(devCtx, sizeof(DEVICE_CONTEXT));

    /* Set serial defaults */
    devCtx->CurrentBaud = CH341_BAUD_DEFAULT;
    devCtx->LineControl.WordLength = 8;
    devCtx->LineControl.Parity = NO_PARITY;
    devCtx->LineControl.StopBits = STOP_BIT_1;
    devCtx->DtrState = TRUE;
    devCtx->RtsState = TRUE;

    /* Create spinlock for serial state */
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &devCtx->SerialLock);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("WdfSpinLockCreate failed: 0x%08X", status);
        return status;
    }

    /* Initialize RX ring buffer */
    status = RingBufInit(&devCtx->RxBuffer, RING_BUF_DEFAULT_SIZE);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("RingBufInit failed: 0x%08X", status);
        return status;
    }

    /* Create I/O queues */
    status = QueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("QueueInitialize failed: 0x%08X", status);
        return status;
    }

    /* Register COM port device interface */
    status = SerialRegisterComPort(device, devCtx);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("SerialRegisterComPort failed: 0x%08X", status);
        return status;
    }

    CH341_LOG("EvtDeviceAdd completed successfully");
    return STATUS_SUCCESS;
}
