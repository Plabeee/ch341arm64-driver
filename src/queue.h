/*
 * queue.h - WDF I/O queue setup and routing
 */

#pragma once

#include "device.h"

/* Create all I/O queues for the device. Called from EvtDeviceAdd. */
NTSTATUS
QueueInitialize(
    _In_ WDFDEVICE Device
    );

/* Queue event callbacks */
EVT_WDF_IO_QUEUE_IO_READ           EvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE          EvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

/* Complete a pending read if data is now available. Called from RX path. */
VOID
QueueCompletePendingRead(
    _In_ PDEVICE_CONTEXT DevCtx
    );
