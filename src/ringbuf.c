/*
 * ringbuf.c - Circular buffer implementation for RX data
 */

#include "ringbuf.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, RingBufInit)
#pragma alloc_text(PAGE, RingBufFree)
#endif

NTSTATUS
RingBufInit(
    _Out_ PRING_BUFFER Ring,
    _In_  ULONG Size
    )
{
    PAGED_CODE();

    /* Validate power of 2 */
    if (Size == 0 || (Size & (Size - 1)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Ring->Buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, 'fuBR');
    if (!Ring->Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ring->Size = Size;
    Ring->Mask = Size - 1;
    Ring->Head = 0;
    Ring->Tail = 0;

    return STATUS_SUCCESS;
}

VOID
RingBufFree(
    _Inout_ PRING_BUFFER Ring
    )
{
    PAGED_CODE();

    if (Ring->Buffer) {
        ExFreePoolWithTag(Ring->Buffer, 'fuBR');
        Ring->Buffer = NULL;
    }
    Ring->Size = 0;
    Ring->Mask = 0;
    Ring->Head = 0;
    Ring->Tail = 0;
}

ULONG
RingBufWrite(
    _Inout_                  PRING_BUFFER Ring,
    _In_reads_bytes_(Length)  const UCHAR *Data,
    _In_                     ULONG Length
    )
{
    ULONG space = RingBufSpace(Ring);
    ULONG toWrite = min(Length, space);
    ULONG i;

    for (i = 0; i < toWrite; i++) {
        Ring->Buffer[Ring->Head & Ring->Mask] = Data[i];
        Ring->Head++;
    }

    return toWrite;
}

ULONG
RingBufRead(
    _Inout_                   PRING_BUFFER Ring,
    _Out_writes_bytes_(MaxLen) UCHAR *Dest,
    _In_                      ULONG MaxLen
    )
{
    ULONG available = RingBufAvailable(Ring);
    ULONG toRead = min(MaxLen, available);
    ULONG i;

    for (i = 0; i < toRead; i++) {
        Dest[i] = Ring->Buffer[Ring->Tail & Ring->Mask];
        Ring->Tail++;
    }

    return toRead;
}

ULONG
RingBufAvailable(
    _In_ const RING_BUFFER *Ring
    )
{
    return Ring->Head - Ring->Tail;
}

ULONG
RingBufSpace(
    _In_ const RING_BUFFER *Ring
    )
{
    return Ring->Size - (Ring->Head - Ring->Tail);
}

VOID
RingBufPurge(
    _Inout_ PRING_BUFFER Ring
    )
{
    Ring->Head = 0;
    Ring->Tail = 0;
}
