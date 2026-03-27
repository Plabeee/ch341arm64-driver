/*
 * ch341_hw.h - WCH CH340/CH341 USB-serial hardware definitions
 *
 * Protocol constants derived from the Linux ch341.c driver (GPLv2).
 * The CH341 protocol is not officially documented by WCH; the Linux driver
 * was reverse-engineered from the behavior of the Windows x64 driver.
 *
 * TODO: Verify all register encodings against USBPcap captures from the
 * x64 WCH driver. Annotated items are especially uncertain.
 */

#pragma once

/* ------------------------------------------------------------------ */
/* USB IDs                                                             */
/* ------------------------------------------------------------------ */

#define CH341_VID                   0x1A86
#define CH341_PID_CH340             0x7523
#define CH341_PID_CH341A            0x7522
#define CH341_PID_CH341_ALT         0x5523

/* ------------------------------------------------------------------ */
/* Vendor control request codes (bRequest field)                       */
/* ------------------------------------------------------------------ */

#define CH341_REQ_READ_VERSION      0x5F
#define CH341_REQ_WRITE_REG         0x9A
#define CH341_REQ_READ_REG          0x95
#define CH341_REQ_SERIAL_INIT       0xA1
#define CH341_REQ_MODEM_CTRL        0xA4

/* ------------------------------------------------------------------ */
/* Register addresses                                                  */
/* ------------------------------------------------------------------ */

#define CH341_REG_BREAK             0x05
#define CH341_REG_PRESCALER         0x12
#define CH341_REG_DIVISOR           0x13
#define CH341_REG_LCR               0x18
#define CH341_REG_LCR2              0x25
#define CH341_REG_FLOW_CTL          0x27

/*
 * Combined register pair addresses for WRITE_REG / READ_REG.
 * The CH341 WRITE_REG command writes two registers at once:
 *   wValue = (reg_addr_high << 8) | reg_addr_low
 *   wIndex = (reg_value_high << 8) | reg_value_low
 */
#define CH341_REG_BAUD_PAIR         ((CH341_REG_DIVISOR << 8) | CH341_REG_PRESCALER)    /* 0x1312 */
#define CH341_REG_LCR_PAIR          ((CH341_REG_LCR2 << 8) | CH341_REG_LCR)            /* 0x2518 */

/* Register pair used for modem status read (value field of READ_REG) */
#define CH341_REG_MODEM_STATUS      0x0706

/* ------------------------------------------------------------------ */
/* Line Control Register (LCR) bit masks                               */
/* ------------------------------------------------------------------ */

#define CH341_LCR_ENABLE_RX         0x80
#define CH341_LCR_ENABLE_TX         0x40
#define CH341_LCR_MARK_SPACE        0x20
#define CH341_LCR_PAR_EVEN          0x10
#define CH341_LCR_ENABLE_PAR        0x08
#define CH341_LCR_STOP_BITS_2       0x04

/* Character size encoded in bits [1:0] */
#define CH341_LCR_CS5               0x00
#define CH341_LCR_CS6               0x01
#define CH341_LCR_CS7               0x02
#define CH341_LCR_CS8               0x03
#define CH341_LCR_CS_MASK           0x03

/* Default LCR: 8N1 with RX+TX enabled */
#define CH341_LCR_DEFAULT           (CH341_LCR_ENABLE_RX | CH341_LCR_ENABLE_TX | CH341_LCR_CS8)

/* ------------------------------------------------------------------ */
/* Flow control                                                        */
/* ------------------------------------------------------------------ */

#define CH341_FLOW_CTL_NONE         0x0000
#define CH341_FLOW_CTL_RTSCTS       0x0101  /* TODO: verify exact encoding */

/* ------------------------------------------------------------------ */
/* Modem control output bits (active low: written as bitwise NOT)      */
/* ------------------------------------------------------------------ */

#define CH341_MODEM_OUT_DTR         0x20
#define CH341_MODEM_OUT_RTS         0x40

/* ------------------------------------------------------------------ */
/* Modem status input bits (active low: inverted when read)            */
/* ------------------------------------------------------------------ */

#define CH341_MODEM_IN_CTS          0x01
#define CH341_MODEM_IN_DSR          0x02
#define CH341_MODEM_IN_RI           0x04
#define CH341_MODEM_IN_DCD          0x08

/*
 * Map CH341 modem status bits to Win32 MS_* constants from ntddser.h.
 * Win32 defines:
 *   MS_CTS_ON  = 0x0010
 *   MS_DSR_ON  = 0x0020
 *   MS_RING_ON = 0x0040
 *   MS_RLSD_ON = 0x0080
 *
 * TODO: Verify CH341 bit positions against USBPcap captures.
 */
#define CH341_MS_TO_WIN32_CTS(s)    (((s) & CH341_MODEM_IN_CTS) ? SERIAL_MSR_CTS  : 0)
#define CH341_MS_TO_WIN32_DSR(s)    (((s) & CH341_MODEM_IN_DSR) ? SERIAL_MSR_DSR  : 0)
#define CH341_MS_TO_WIN32_RI(s)     (((s) & CH341_MODEM_IN_RI)  ? SERIAL_MSR_RI   : 0)
#define CH341_MS_TO_WIN32_DCD(s)    (((s) & CH341_MODEM_IN_DCD) ? SERIAL_MSR_DCD  : 0)

/* ------------------------------------------------------------------ */
/* Clock and baud rate                                                 */
/* ------------------------------------------------------------------ */

#define CH341_OSC_FREQ              48000000UL  /* 48 MHz oscillator */
#define CH341_BAUD_MIN              46UL
#define CH341_BAUD_MAX              3000000UL
#define CH341_BAUD_DEFAULT          9600UL

/*
 * Baud rate divisor model (from Linux ch341.c):
 *
 *   baud = CH341_OSC_FREQ / (prescaler_factor * (256 - divisor_byte))
 *
 * Prescaler index (ps=0..3) maps to division factors:
 *   ps=0 -> 2^12 = 4096   (low baud:   ~46 to  ~11,718)
 *   ps=1 -> 2^9  = 512    (low-mid:   ~366 to  ~93,750)
 *   ps=2 -> 2^6  = 64     (mid-high: ~2,929 to ~750,000)
 *   ps=3 -> 2^3  = 8      (high:    ~23,438 to ~6,000,000)
 *
 * TODO: The exact prescaler register encoding may include flag bits
 * (e.g., bit 7 set). Verify against USBPcap captures.
 */
#define CH341_PRESCALER_FACTOR(ps)  (1UL << (12 - 3 * (ps)))
#define CH341_PRESCALER_COUNT       4

/* ------------------------------------------------------------------ */
/* Chip version                                                        */
/* ------------------------------------------------------------------ */

/*
 * READ_VERSION returns 2 bytes. Linux expects 0x27 0x00 as typical.
 * The first byte is cached as the chip version.
 *
 * Version >= 0x30: use LCR register directly; LCR2 is always set to 0.
 * Version <  0x30: different line-control handling (legacy path).
 */
#define CH341_VERSION_THRESHOLD     0x30

/* ------------------------------------------------------------------ */
/* Break control                                                       */
/* ------------------------------------------------------------------ */

#define CH341_BREAK_ON              0x01  /* TODO: verify break register encoding */
#define CH341_BREAK_OFF             0x00

/* ------------------------------------------------------------------ */
/* USB transfer constants                                              */
/* ------------------------------------------------------------------ */

#define CH341_USB_TIMEOUT_MS        5000
#define CH341_CTRL_BUF_SIZE         8

/* Expected endpoint configuration (to be verified against descriptors):
 *   - 1 bulk IN endpoint
 *   - 1 bulk OUT endpoint
 *   - 1 interrupt IN endpoint (modem status)
 */
#define CH341_EXPECTED_BULK_IN      1
#define CH341_EXPECTED_BULK_OUT     1
#define CH341_EXPECTED_INT_IN       1

/* ------------------------------------------------------------------ */
/* Kernel-mode serial constants                                        */
/* These are the Win32 equivalents not provided by kernel headers.     */
/* ------------------------------------------------------------------ */

/* Modem status register bits (same values as Win32 MS_CTS_ON etc.) */
#ifndef SERIAL_MSR_CTS
#define SERIAL_MSR_DCTS             0x01
#define SERIAL_MSR_DDSR             0x02
#define SERIAL_MSR_TERI             0x04
#define SERIAL_MSR_DDCD             0x08
#define SERIAL_MSR_CTS              0x10
#define SERIAL_MSR_DSR              0x20
#define SERIAL_MSR_RI               0x40
#define SERIAL_MSR_DCD              0x80
#endif

/* Serial event flags (same values as Win32 EV_* from winbase.h) */
#ifndef EV_RXCHAR
#define EV_RXCHAR                   0x0001
#define EV_RXFLAG                   0x0002
#define EV_TXEMPTY                  0x0004
#define EV_CTS                      0x0008
#define EV_DSR                      0x0010
#define EV_RLSD                     0x0020
#define EV_BREAK                    0x0040
#define EV_ERR                      0x0080
#define EV_RING                     0x0100
#define EV_PERR                     0x0200
#define EV_RX80FULL                 0x0400
#define EV_EVENT1                   0x0800
#define EV_EVENT2                   0x1000
#endif
