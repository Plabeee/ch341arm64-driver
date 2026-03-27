cmake_minimum_required(VERSION 3.20)
project(ch341arm64 C)

# Find the Windows Driver Kit
# Target KMDF 1.33 for compatibility with installed runtime
# (WDK 26100 headers ship 1.35 bind version newer than the installed runtime)
set(WDK_KMDF_VERSION "1.25" CACHE STRING "KMDF version" FORCE)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(FindWDK)

# Driver sources
set(DRIVER_SOURCES
    src/driver.c
    src/device.c
    src/ch341.c
    src/serial.c
    src/queue.c
    src/ringbuf.c
)

# Create the kernel driver target
wdk_add_driver(ch341arm64 ${DRIVER_SOURCES})

# KMDF version defines - REQUIRED for WDF bind version negotiation.
# Without these, WdfMinimumVersionRequired defaults to 0xFFFFFFFF
# and the WDF loader refuses to bind (misleading "bind version greater" error).
target_compile_definitions(ch341arm64 PRIVATE
    KMDF_VERSION_MAJOR=1
    KMDF_VERSION_MINOR=25
    $<$<CONFIG:Debug>:DBG=1>
)

# Add project include path so headers find each other
target_include_directories(ch341arm64 PRIVATE "${CMAKE_SOURCE_DIR}/src")

# Copy INF alongside the .sys for easy packaging
configure_file(
    "${CMAKE_SOURCE_DIR}/inf/ch341arm64.inf"
    "${CMAKE_BINARY_DIR}/ch341arm64.inf"
    COPYONLY
)
