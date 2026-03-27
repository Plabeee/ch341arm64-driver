# ch341arm64 — ARM64 Windows 11 USB-Serial Driver for CH340/CH341

A native ARM64 KMDF kernel driver for WCH CH340/CH341 USB-to-UART devices on Windows on Arm. Exposes a standard COM port so legacy serial applications (terminal emulators, radio CPS tools, embedded dev consoles) work without x86/x64 emulation.

Windows on Arm cannot emulate kernel drivers. WCH does not ship an ARM64 build of their CH341 driver. This project fills that gap.

## Supported Hardware

| USB ID              | Chip     | Notes              |
|---------------------|----------|--------------------|
| `VID_1A86&PID_7523` | CH340    | Most common        |
| `VID_1A86&PID_7522` | CH341A   |                    |
| `VID_1A86&PID_5523` | CH341    | Alternate revision  |

The CH341 is **not** a CDC ACM device, so the Windows in-box `Usbser.sys` does not apply.

## Prerequisites

All of the following must be installed on an ARM64 Windows 11 machine:

| Dependency                | Version                  | Notes                                        |
|---------------------------|--------------------------|----------------------------------------------|
| **Visual Studio 2022+**   | 17.x with ARM64 C++ tools | Need the "Desktop development with C++" workload and the ARM64/ARM64EC build tools component |
| **Windows Driver Kit (WDK)** | 10.0.26100.0 or later   | Installs to `C:\Program Files (x86)\Windows Kits\10` |
| **CMake**                 | 3.20+                    | Included with VS or install standalone        |
| **Ninja**                 | Any recent version       | Included with VS or install standalone        |
| **Test signing enabled**  | —                        | Required to load unsigned/test-signed drivers |

### Enable test signing

From an **Administrator** command prompt:

```
bcdedit /set testsigning on
```

Reboot. A "Test Mode" watermark will appear on the desktop — this is expected.

## Project Layout

```
driver/
├── CMakeLists.txt          # Top-level build file
├── CMakePresets.json        # arm64-debug / arm64-release presets
├── cmake/
│   └── FindWDK.cmake       # Locates WDK, provides wdk_add_driver()
├── inf/
│   └── ch341arm64.inf       # Driver installation INF (Ports class)
├── src/
│   ├── driver.h             # Common includes, pool tag, debug macro
│   ├── driver.c             # DriverEntry, EvtDeviceAdd
│   ├── device.h             # DEVICE_CONTEXT, PnP/power callback decls
│   ├── device.c             # USB config, endpoint discovery, continuous reader
│   ├── ch341_hw.h           # CH341 register/protocol constants
│   ├── ch341.h              # CH341 protocol function declarations
│   ├── ch341.c              # Vendor USB transfers, baud/line/modem/flow control
│   ├── serial.h             # COM port registration and IOCTL decls
│   ├── serial.c             # IOCTL_SERIAL_* dispatch, COM port setup
│   ├── queue.h              # I/O queue declarations
│   ├── queue.c              # WDF queue init, read/write/ioctl handlers
│   ├── ringbuf.h            # RX ring buffer interface
│   └── ringbuf.c            # RX ring buffer implementation
├── sign_and_install.bat     # One-click sign + install script
└── DRIVER.md                # Original design brief
```

## Building

### 1. Configure

```bash
cmake --preset arm64-debug
```

If CMake cannot find the compiler, point it explicitly:

```bash
cmake --preset arm64-debug ^
  -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/<version>/bin/Hostarm64/arm64/cl.exe" ^
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
```

The `STATIC_LIBRARY` trick is needed because CMake's test-compile tries to link a user-mode exe, which fails against kernel-mode libs.

### 2. Build

```bash
cmake --build build/arm64-debug
```

Output: `build/arm64-debug/ch341arm64.sys`

### Release build

```bash
cmake --preset arm64-release
cmake --build build/arm64-release
```

## Installing

### Quick method: `sign_and_install.bat`

Run from an **Administrator** command prompt:

```
sign_and_install.bat
```

This script:

1. Checks test signing is enabled
2. Creates a self-signed code-signing certificate (`CH341ARM64Test`)
3. Exports it and adds it to the Root and TrustedPublisher stores
4. Runs `inf2cat` to generate a catalog file
5. Signs both `.cat` and `.sys` with `signtool`
6. Installs via `pnputil /add-driver /install`

### Manual method

From an **Administrator** shell in the `build/arm64-debug` directory:

```batch
:: Copy INF next to .sys
copy ..\..\inf\ch341arm64.inf .

:: Create catalog
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\inf2cat.exe" ^
  /driver:. /os:10_NI_ARM64 /uselocaltime

:: Sign (assumes test cert already created and trusted)
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\arm64\signtool.exe" ^
  sign /s My /sm /n CH341ARM64Test /fd SHA256 ch341arm64.cat
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\arm64\signtool.exe" ^
  sign /s My /sm /n CH341ARM64Test /fd SHA256 ch341arm64.sys

:: Install
pnputil /add-driver ch341arm64.inf /install
```

### Uninstalling

```batch
pnputil /delete-driver oem<N>.inf /uninstall
```

Find the OEM number with `pnputil /enum-drivers`.

## Verifying

After installation, plug in the CH340 cable. Check Device Manager under **Ports (COM & LPT)** — you should see:

```
CH340 USB-Serial (ARM64) (COMx)
```

Verify the COM port works:

```powershell
$port = New-Object System.IO.Ports.SerialPort 'COMx', 9600
$port.Open()
$port.IsOpen   # should be True
$port.Close()
```

## Architecture

### Driver stack

```
Application (CreateFile("COMx"), ReadFile, WriteFile, DeviceIoControl)
     │
  Win32 Serial API  ──  IOCTL_SERIAL_*
     │
  ch341arm64.sys (this driver)
     │
  KMDF  →  WdfUsbTargetDevice
     │
  USB bus driver (usbxhci / usbhub3)
     │
  CH340/CH341 hardware
```

### Data flow

**TX (application → device):** `EvtIoWrite` → `WdfUsbTargetPipeFormatRequestForWrite` → bulk OUT endpoint

**RX (device → application):** WDF continuous reader on bulk IN → `EvtUsbBulkInReadComplete` → ring buffer → `EvtIoRead` drains buffer

### CH341 protocol

The CH341 uses **vendor-specific USB control transfers** on endpoint 0 for configuration, and **bulk endpoints** for data. The protocol is not officially documented; register definitions are derived from the [Linux ch341 driver](https://github.com/torvalds/linux/blob/master/drivers/usb/serial/ch341.c).

Key operations:

| Operation       | Request | Value          | Index            |
|-----------------|---------|----------------|------------------|
| Read version    | 0x5F    | 0              | 0                |
| Serial init     | 0xA1    | 0              | 0                |
| Write register  | 0x9A    | reg_addr_pair  | reg_value_pair   |
| Read register   | 0x95    | reg_addr_pair  | 0                |
| Modem control   | 0xA4    | ~(DTR\|RTS)    | 0                |

Baud rate formula: `baud = 48MHz / (prescaler_factor × (256 − divisor))`

### Implemented serial IOCTLs

- `IOCTL_SERIAL_SET/GET_BAUD_RATE`
- `IOCTL_SERIAL_SET/GET_LINE_CONTROL` (data bits, parity, stop bits)
- `IOCTL_SERIAL_SET/GET_TIMEOUTS`
- `IOCTL_SERIAL_SET/GET_HANDFLOW` (RTS/CTS flow control)
- `IOCTL_SERIAL_SET/CLR_DTR`, `SET/CLR_RTS`
- `IOCTL_SERIAL_GET_MODEMSTATUS`
- `IOCTL_SERIAL_SET/GET_WAIT_MASK`, `WAIT_ON_MASK`
- `IOCTL_SERIAL_GET_COMMSTATUS`
- `IOCTL_SERIAL_PURGE`
- `IOCTL_SERIAL_GET_PROPERTIES`
- `IOCTL_SERIAL_SET_BREAK_ON/OFF`
- `IOCTL_SERIAL_SET_QUEUE_SIZE` (ignored)
- `IOCTL_SERIAL_SET/GET_CHARS` (stub)
- `IOCTL_SERIAL_RESET_DEVICE` (ignored)

## Known Limitations / TODO

- **Read timeouts:** Reads return immediately with available data (no blocking/timeout). A full implementation would pend requests with timer-based timeouts.
- **TX purge/abort:** `SERIAL_PURGE_TXCLEAR` / `SERIAL_PURGE_TXABORT` are logged but don't cancel in-flight writes.
- **COM port number:** Defaults to COM10 on first install. Windows may reassign; the number is stable across driver updates but not across full uninstall/reinstall cycles.
- **Legacy chip versions:** CH341 chips with version < 0x30 use a different LCR code path that is not fully implemented (falls back to the modern path with a warning).
- **Break control:** May not work correctly on all CH34x variants.
- **Protocol verification:** Register encodings should be verified against USBPcap captures from the official x64 WCH driver.

## WDF Version Note

The build targets **KMDF 1.25** even though the WDK ships 1.35 headers. This is intentional. The WDF headers require `KMDF_VERSION_MAJOR` and `KMDF_VERSION_MINOR` to be defined at compile time — without them, `WdfMinimumVersionRequired` defaults to `0xFFFFFFFF` and the WDF loader refuses to bind the driver (with a misleading "bind version greater than library" error). The `FindWDK.cmake` module handles this automatically.

## License

Protocol constants are derived from the Linux `ch341.c` driver (GPLv2). Refer to `DRIVER.md` for attribution and provenance details.
