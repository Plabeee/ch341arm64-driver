/*
 * serial.h - Serial (COM port) IOCTL compatibility layer
 */

#pragma once

#include "device.h"

/* ------------------------------------------------------------------ */
/* COM port registration                                               */
/* ------------------------------------------------------------------ */

NTSTATUS
SerialRegisterComPort(
    _In_ WDFDEVICE Device,
    _In_ PDEVICE_CONTEXT DevCtx
    );

/* ------------------------------------------------------------------ */
/* IOCTL dispatch (called from EvtIoDeviceControl)                     */
/* ------------------------------------------------------------------ */

NTSTATUS
SerialHandleIoctl(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength
    );

/* ------------------------------------------------------------------ */
/* Modem status change notification                                    */
/* ------------------------------------------------------------------ */

VOID
SerialNotifyModemStatusChange(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ ULONG NewModemStatus
    );

/* ------------------------------------------------------------------ */
/* SERIALCOMM device map registration                                  */
/* ------------------------------------------------------------------ */

VOID
SerialMapComPort(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Register
    );
