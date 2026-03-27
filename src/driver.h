/*
 * driver.h - Common includes for the CH341 ARM64 KMDF driver
 *
 * Include this header first in every .c file. It pulls in the WDK
 * and KMDF headers in the correct order, plus project-wide definitions.
 */

#pragma once

/* WDK / KMDF headers - order matters */
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdi.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <ntstrsafe.h>
#include <ntddser.h>

/* Project headers */
#include "ch341_hw.h"
#include "ringbuf.h"

/* Pool tag for general driver allocations */
#define CH341_POOL_TAG  '143C'   /* "C341" reversed */

/* Debug trace macro */
#if DBG
#define CH341_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, \
               "ch341arm64: " fmt "\n", ##__VA_ARGS__)
#else
#define CH341_LOG(fmt, ...) ((void)0)
#endif
