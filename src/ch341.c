/*
 * ch341.c - CH341 protocol implementation
 *
 * Vendor USB control transfers, baud rate calculation, line control,
 * modem control, and flow control.
 *
 * Protocol baseline from Linux ch341.c (GPLv2). All register encodings
 * should be verified against USBPcap captures from the x64 WCH driver.
 */

#include "driver.h"
#include "ch341.h"

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
    )
{
    NTSTATUS status;
    WDF_USB_CONTROL_SETUP_PACKET setupPacket;
    WDF_MEMORY_DESCRIPTOR memDesc;
    ULONG transferred = 0;

    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
        &setupPacket,
        BmRequestDeviceToHost,      /* Direction: IN */
        BmRequestToDevice,          /* Recipient: device */
        Request,
        Value,
        Index
    );

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc, Buffer, Len);

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DevCtx->UsbDevice,
        WDF_NO_HANDLE,
        NULL,   /* SendOptions */
        &setupPacket,
        &memDesc,
        &transferred
    );

    if (NT_SUCCESS(status)) {
        CH341_LOG("VendorRead: req=0x%02X val=0x%04X idx=0x%04X -> %lu bytes",
                  Request, Value, Index, transferred);
    } else {
        CH341_LOG("VendorRead FAILED: req=0x%02X val=0x%04X idx=0x%04X, status=0x%08X",
                  Request, Value, Index, status);
    }

    return status;
}

NTSTATUS
Ch341VendorWrite(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ UCHAR Request,
    _In_ USHORT Value,
    _In_ USHORT Index
    )
{
    NTSTATUS status;
    WDF_USB_CONTROL_SETUP_PACKET setupPacket;

    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
        &setupPacket,
        BmRequestHostToDevice,      /* Direction: OUT */
        BmRequestToDevice,          /* Recipient: device */
        Request,
        Value,
        Index
    );

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DevCtx->UsbDevice,
        WDF_NO_HANDLE,
        NULL,   /* SendOptions */
        &setupPacket,
        NULL,   /* No data stage */
        NULL    /* BytesTransferred */
    );

    if (NT_SUCCESS(status)) {
        CH341_LOG("VendorWrite: req=0x%02X val=0x%04X idx=0x%04X",
                  Request, Value, Index);
    } else {
        CH341_LOG("VendorWrite FAILED: req=0x%02X val=0x%04X idx=0x%04X, status=0x%08X",
                  Request, Value, Index, status);
    }

    return status;
}

/* ------------------------------------------------------------------ */
/* Hardware initialization                                             */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341InitHardware(
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    UCHAR versionBuf[2] = { 0 };

    CH341_LOG("Ch341InitHardware: starting initialization sequence");

    /*
     * Step 1: READ_VERSION
     * Linux expects 2 bytes; first byte is the chip version.
     * Typical response: 0x27 0x00 (version 0x27).
     */
    status = Ch341VendorRead(
        DevCtx,
        CH341_REQ_READ_VERSION,
        0, 0,
        versionBuf, sizeof(versionBuf)
    );
    if (!NT_SUCCESS(status)) {
        CH341_LOG("READ_VERSION failed");
        return status;
    }

    DevCtx->ChipVersion = versionBuf[0];
    CH341_LOG("Chip version: 0x%02X", DevCtx->ChipVersion);

    /*
     * Step 2: SERIAL_INIT
     */
    status = Ch341VendorWrite(
        DevCtx,
        CH341_REQ_SERIAL_INIT,
        0, 0
    );
    if (!NT_SUCCESS(status)) {
        CH341_LOG("SERIAL_INIT failed");
        return status;
    }

    /*
     * Step 3: Set default baud rate (9600)
     */
    status = Ch341SetBaudRate(DevCtx, DevCtx->CurrentBaud);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Default baud rate set failed");
        return status;
    }

    /*
     * Step 4: Set default line control (8N1)
     */
    status = Ch341SetLineControl(DevCtx, 8, NO_PARITY, STOP_BIT_1);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Default line control set failed");
        return status;
    }

    /*
     * Step 5: Set default modem control (DTR + RTS asserted)
     */
    status = Ch341SetModemControl(DevCtx, DevCtx->DtrState, DevCtx->RtsState);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Default modem control set failed");
        return status;
    }

    /*
     * Step 6: Disable hardware flow control
     */
    status = Ch341SetFlowControl(DevCtx, FALSE);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Default flow control set failed");
        return status;
    }

    /*
     * Step 7: Read initial modem status
     */
    {
        ULONG modemStatus;
        status = Ch341GetModemStatus(DevCtx, &modemStatus);
        if (NT_SUCCESS(status)) {
            DevCtx->ModemStatus = modemStatus;
            DevCtx->PreviousModemStatus = modemStatus;
        } else {
            /* Non-fatal: modem status read might fail on some variants */
            CH341_LOG("Initial modem status read failed (non-fatal)");
            status = STATUS_SUCCESS;
        }
    }

    CH341_LOG("CH341 initialization complete");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Baud rate                                                           */
/* ------------------------------------------------------------------ */

/*
 * Calculate prescaler and divisor for a target baud rate.
 *
 * Formula: baud = 48000000 / (2^(12 - 3*ps) * (256 - d))
 *
 * We try ps=3 (smallest prescaler factor) first for best resolution,
 * falling back to lower ps values for slower baud rates.
 *
 * TODO: Verify exact register encoding via USBPcap. The prescaler
 * register byte may include flag bits (e.g., bit 7 always set).
 * The Linux driver's encoding should be checked against captures
 * at 9600, 19200, 57600, and 115200 baud.
 */
static NTSTATUS
Ch341CalcDivisor(
    _In_  ULONG BaudRate,
    _Out_ UCHAR *PrescalerReg,
    _Out_ UCHAR *DivisorReg,
    _Out_ ULONG *ActualBaud
    )
{
    int ps;

    if (BaudRate < CH341_BAUD_MIN || BaudRate > CH341_BAUD_MAX) {
        return STATUS_INVALID_PARAMETER;
    }

    for (ps = 3; ps >= 0; ps--) {
        ULONG factor = CH341_PRESCALER_FACTOR(ps);
        ULONG div;
        ULONG actual;

        /* div = round(48000000 / (factor * baud)) */
        div = (CH341_OSC_FREQ + (factor * BaudRate / 2)) / (factor * BaudRate);

        if (div < 2 || div > 256) {
            continue;
        }

        actual = CH341_OSC_FREQ / (factor * div);

        /*
         * Prescaler register encoding:
         * The prescaler index goes into the register value. Some references
         * suggest bit 7 should be set; we include it as a TODO.
         *
         * TODO: Verify whether prescaler reg = ps | 0x80 or just ps.
         * Check USBPcap captures at multiple baud rates.
         */
        *PrescalerReg = (UCHAR)(ps | 0x80);
        *DivisorReg = (UCHAR)(256 - div);
        *ActualBaud = actual;

        CH341_LOG("Baud %lu -> ps=%d, div=%lu, actual=%lu", BaudRate, ps, div, actual);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
Ch341SetBaudRate(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ ULONG BaudRate
    )
{
    NTSTATUS status;
    UCHAR prescalerReg, divisorReg;
    ULONG actualBaud;

    status = Ch341CalcDivisor(BaudRate, &prescalerReg, &divisorReg, &actualBaud);
    if (!NT_SUCCESS(status)) {
        CH341_LOG("Ch341SetBaudRate: invalid baud %lu", BaudRate);
        return status;
    }

    /*
     * Write prescaler and divisor as a register pair:
     *   wValue = (REG_DIVISOR << 8) | REG_PRESCALER = 0x1312
     *   wIndex = (divisor_val << 8) | prescaler_val
     */
    status = Ch341VendorWrite(
        DevCtx,
        CH341_REQ_WRITE_REG,
        CH341_REG_BAUD_PAIR,
        (USHORT)((divisorReg << 8) | prescalerReg)
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DevCtx->CurrentBaud = actualBaud;
    CH341_LOG("Baud rate set: requested=%lu, actual=%lu", BaudRate, actualBaud);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Line control                                                        */
/* ------------------------------------------------------------------ */

/*
 * Map Windows serial line control parameters to CH341 LCR register.
 *
 * Windows constants (from ntddser.h):
 *   WordLength: 5, 6, 7, 8
 *   Parity: NO_PARITY(0), ODD_PARITY(1), EVEN_PARITY(2),
 *           MARK_PARITY(3), SPACE_PARITY(4)
 *   StopBits: STOP_BIT_1(0), STOP_BITS_1_5(1), STOP_BITS_2(2)
 */
NTSTATUS
Ch341SetLineControl(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ UCHAR WordLength,
    _In_ UCHAR Parity,
    _In_ UCHAR StopBits
    )
{
    NTSTATUS status;
    UCHAR lcr = CH341_LCR_ENABLE_RX | CH341_LCR_ENABLE_TX;
    UCHAR lcr2 = 0;

    /* Character size */
    switch (WordLength) {
    case 5: lcr |= CH341_LCR_CS5; break;
    case 6: lcr |= CH341_LCR_CS6; break;
    case 7: lcr |= CH341_LCR_CS7; break;
    case 8: lcr |= CH341_LCR_CS8; break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    /* Parity */
    switch (Parity) {
    case NO_PARITY:
        break;
    case ODD_PARITY:
        lcr |= CH341_LCR_ENABLE_PAR;
        break;
    case EVEN_PARITY:
        lcr |= CH341_LCR_ENABLE_PAR | CH341_LCR_PAR_EVEN;
        break;
    case MARK_PARITY:
        lcr |= CH341_LCR_ENABLE_PAR | CH341_LCR_MARK_SPACE;
        break;
    case SPACE_PARITY:
        lcr |= CH341_LCR_ENABLE_PAR | CH341_LCR_PAR_EVEN | CH341_LCR_MARK_SPACE;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    /* Stop bits */
    switch (StopBits) {
    case STOP_BIT_1:
        break;
    case STOP_BITS_1_5:
        /* 1.5 stop bits: treat as 2 for CH341 */
        /* TODO: verify CH341 behavior with 1.5 stop bits */
        lcr |= CH341_LCR_STOP_BITS_2;
        break;
    case STOP_BITS_2:
        lcr |= CH341_LCR_STOP_BITS_2;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Write LCR registers.
     * Version >= 0x30: use LCR register, LCR2 always 0.
     * Version <  0x30: different handling (TODO: implement legacy path).
     */
    if (DevCtx->ChipVersion < CH341_VERSION_THRESHOLD) {
        /*
         * TODO: Implement legacy LCR handling for chip versions < 0x30.
         * The Linux driver has a different code path for these older chips.
         * For now, use the same path and log a warning.
         */
        CH341_LOG("WARNING: Chip version 0x%02X < 0x30, legacy LCR path not fully implemented",
                  DevCtx->ChipVersion);
    }

    status = Ch341VendorWrite(
        DevCtx,
        CH341_REQ_WRITE_REG,
        CH341_REG_LCR_PAIR,
        (USHORT)((lcr2 << 8) | lcr)
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DevCtx->LineControl.WordLength = WordLength;
    DevCtx->LineControl.Parity = Parity;
    DevCtx->LineControl.StopBits = StopBits;

    CH341_LOG("Line control set: %d%c%s",
              WordLength,
              "NOEMS"[Parity],
              StopBits == STOP_BITS_2 ? "2" : "1");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Modem control                                                       */
/* ------------------------------------------------------------------ */

/*
 * Linux writes MODEM_CTRL with the bitwise NOT of the control value.
 * DTR and RTS are active low in the CH341 protocol.
 */
NTSTATUS
Ch341SetModemControl(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Dtr,
    _In_ BOOLEAN Rts
    )
{
    NTSTATUS status;
    USHORT controlBits = 0;

    if (Dtr) controlBits |= CH341_MODEM_OUT_DTR;
    if (Rts) controlBits |= CH341_MODEM_OUT_RTS;

    /* CH341 expects bitwise NOT of the control value */
    status = Ch341VendorWrite(
        DevCtx,
        CH341_REQ_MODEM_CTRL,
        (USHORT)~controlBits,
        0
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DevCtx->DtrState = Dtr;
    DevCtx->RtsState = Rts;

    CH341_LOG("Modem control: DTR=%d, RTS=%d", Dtr, Rts);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Modem status                                                        */
/* ------------------------------------------------------------------ */

/*
 * Read modem status via READ_REG at 0x0706.
 * Linux inverts the returned byte when decoding status bits.
 *
 * Returns Win32-format modem status (MS_CTS_ON, MS_DSR_ON, etc.)
 */
NTSTATUS
Ch341GetModemStatus(
    _In_  PDEVICE_CONTEXT DevCtx,
    _Out_ PULONG Win32Status
    )
{
    NTSTATUS status;
    UCHAR buf[2] = { 0 };
    UCHAR raw;
    ULONG result = 0;

    status = Ch341VendorRead(
        DevCtx,
        CH341_REQ_READ_REG,
        CH341_REG_MODEM_STATUS,
        0,
        buf, sizeof(buf)
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Linux reads 2 bytes and inverts the second byte (index 1)
     * to get the active-high modem status bits.
     * TODO: Verify which byte contains modem status via USBPcap.
     */
    raw = ~buf[1];

    result = CH341_MS_TO_WIN32_CTS(raw) |
             CH341_MS_TO_WIN32_DSR(raw) |
             CH341_MS_TO_WIN32_RI(raw)  |
             CH341_MS_TO_WIN32_DCD(raw);

    *Win32Status = result;

    CH341_LOG("Modem status: raw=0x%02X, win32=0x%08lX", raw, result);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Flow control                                                        */
/* ------------------------------------------------------------------ */

NTSTATUS
Ch341SetFlowControl(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN RtsCts
    )
{
    NTSTATUS status;
    USHORT flowValue = RtsCts ? CH341_FLOW_CTL_RTSCTS : CH341_FLOW_CTL_NONE;

    /*
     * Write flow control register pair.
     * Linux writes this via WRITE_REG with the flow control register address.
     *
     * TODO: Verify exact register address and value encoding for flow control.
     * The CH341_REG_FLOW_CTL (0x27) addressing may need a paired register.
     */
    status = Ch341VendorWrite(
        DevCtx,
        CH341_REQ_WRITE_REG,
        (USHORT)((CH341_REG_FLOW_CTL << 8) | CH341_REG_FLOW_CTL),
        flowValue
    );

    if (NT_SUCCESS(status)) {
        CH341_LOG("Flow control: %s", RtsCts ? "RTS/CTS" : "none");
    }

    return status;
}

/* ------------------------------------------------------------------ */
/* Break                                                               */
/* ------------------------------------------------------------------ */

/*
 * TODO: Break support has known quirks in some CH34x variants.
 * Linux has a simulated break path for chips that don't support
 * native break control. Verify with USBPcap and test on target device.
 */
NTSTATUS
Ch341SetBreak(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN BreakOn
    )
{
    NTSTATUS status;
    USHORT breakVal = BreakOn ? CH341_BREAK_ON : CH341_BREAK_OFF;

    status = Ch341VendorWrite(
        DevCtx,
        CH341_REQ_WRITE_REG,
        (USHORT)((CH341_REG_BREAK << 8) | CH341_REG_BREAK),
        breakVal
    );

    if (NT_SUCCESS(status)) {
        CH341_LOG("Break: %s", BreakOn ? "ON" : "OFF");
    }

    return status;
}
