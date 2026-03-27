/*
 * ringbuf.h - Lock-free circular buffer for RX data accumulation
 *
 * The ring buffer uses a power-of-2 size for fast modular arithmetic.
 * Callers are responsible for external synchronization (spinlock).
 */

#pragma once

#include <ntddk.h>

#define RING_BUF_DEFAULT_SIZE   4096    /* Must be power of 2 */

typedef struct _RING_BUFFER {
    PUCHAR  Buffer;
    ULONG   Size;       /* Always a power of 2 */
    ULONG   Mask;       /* Size - 1, for fast modulo */
    ULONG   Head;       /* Next write position */
    ULONG   Tail;       /* Next read position */
} RING_BUFFER, *PRING_BUFFER;

/* Allocate ring buffer from non-paged pool. Size must be power of 2. */
NTSTATUS
RingBufInit(
    _Out_ PRING_BUFFER Ring,
    _In_  ULONG Size
    );

/* Free the ring buffer. */
VOID
RingBufFree(
    _Inout_ PRING_BUFFER Ring
    );

/* Write data into the ring buffer. Returns number of bytes actually written. */
ULONG
RingBufWrite(
    _Inout_                  PRING_BUFFER Ring,
    _In_reads_bytes_(Length)  const UCHAR *Data,
    _In_                     ULONG Length
    );

/* Read data from the ring buffer. Returns number of bytes actually read. */
ULONG
RingBufRead(
    _Inout_                   PRING_BUFFER Ring,
    _Out_writes_bytes_(MaxLen) UCHAR *Dest,
    _In_                      ULONG MaxLen
    );

/* Number of bytes available to read. */
ULONG
RingBufAvailable(
    _In_ const RING_BUFFER *Ring
    );

/* Free space available for writing. */
ULONG
RingBufSpace(
    _In_ const RING_BUFFER *Ring
    );

/* Discard all data. */
VOID
RingBufPurge(
    _Inout_ PRING_BUFFER Ring
    );
