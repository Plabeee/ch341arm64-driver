/*
 * ch341.h - CH341 protocol-level operations
 *
 * All functions that send vendor USB control transfers or manipulate
 * CH341-specific register state live here.
 */

#pragma once

#include "device.h"

/* ------------------------------------------------------------------ */
/* Low-level vendor control transfers                                  */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341VendorRead(
    _In_                    PDEVICE_CONTEXT DevCtx,
    _In_                    UCHAR Request,
    _In_                    USHORT Value,
    _In_                    USHORT Index,
    _Out_writes_bytes_(Len) PVOID Buffer,
    _In_                    ULONG Len
    );

NTSTATUS
Ch341VendorWrite(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ UCHAR Request,
    _In_ USHORT Value,
    _In_ USHORT Index
    );

/* ------------------------------------------------------------------ */
/* Hardware initialization                                             */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341InitHardware(
    _In_ PDEVICE_CONTEXT DevCtx
    );

/* ------------------------------------------------------------------ */
/* Baud rate                                                           */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341SetBaudRate(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ ULONG BaudRate
    );

/* ------------------------------------------------------------------ */
/* Line control (data bits, parity, stop bits)                         */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341SetLineControl(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ UCHAR WordLength,
    _In_ UCHAR Parity,
    _In_ UCHAR StopBits
    );

/* ------------------------------------------------------------------ */
/* Modem control (DTR / RTS)                                           */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341SetModemControl(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Dtr,
    _In_ BOOLEAN Rts
    );

/* ------------------------------------------------------------------ */
/* Modem status (CTS / DSR / RI / DCD)                                 */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341GetModemStatus(
    _In_  PDEVICE_CONTEXT DevCtx,
    _Out_ PULONG Win32Status
    );

/* ------------------------------------------------------------------ */
/* Flow control                                                        */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341SetFlowControl(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN RtsCts
    );

/* ------------------------------------------------------------------ */
/* Break                                                               */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341SetBreak(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN BreakOn
    );
