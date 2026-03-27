#[[
  FindWDK.cmake - Locate the Windows Driver Kit and provide driver targets

  This module finds the WDK installation and provides the wdk_add_driver()
  function for creating KMDF kernel-mode driver targets.

  Cache variables set:
    WDK_FOUND        - TRUE if WDK was found
    WDK_ROOT         - Root of WDK installation
    WDK_VERSION      - WDK version string (e.g., 10.0.26100.0)

  User-settable variables:
    WDK_ROOT         - Override auto-detection of WDK path
    WDK_VERSION      - Override auto-detection of WDK version
    WDK_KMDF_VERSION - KMDF version to target (default: auto-detect latest)

  Provided functions:
    wdk_add_driver(<name> <source1> [source2 ...])
      Creates a .sys KMDF driver target with proper compiler/linker flags.
]]

cmake_minimum_required(VERSION 3.20)

if(WDK_FOUND)
    return()
endif()

# --------------------------------------------------------------------------
# Find WDK root
# --------------------------------------------------------------------------
if(NOT WDK_ROOT)
    # Standard installation paths
    set(_wdk_search_paths
        "C:/Program Files (x86)/Windows Kits/10"
        "C:/Program Files/Windows Kits/10"
    )

    # Also check WDKContentRoot environment variable
    if(DEFINED ENV{WDKContentRoot})
        list(INSERT _wdk_search_paths 0 "$ENV{WDKContentRoot}")
    endif()

    foreach(_path IN LISTS _wdk_search_paths)
        if(EXISTS "${_path}/Include")
            set(WDK_ROOT "${_path}" CACHE PATH "Windows Driver Kit root directory")
            break()
        endif()
    endforeach()
endif()

if(NOT WDK_ROOT OR NOT EXISTS "${WDK_ROOT}/Include")
    message(FATAL_ERROR
        "WDK not found. Set -DWDK_ROOT=<path> to your WDK 10 installation.\n"
        "Expected: C:/Program Files (x86)/Windows Kits/10"
    )
endif()

message(STATUS "WDK root: ${WDK_ROOT}")

# --------------------------------------------------------------------------
# Find WDK version (pick newest if not specified)
# --------------------------------------------------------------------------
if(NOT WDK_VERSION)
    file(GLOB _wdk_versions RELATIVE "${WDK_ROOT}/Include" "${WDK_ROOT}/Include/10.*")
    if(NOT _wdk_versions)
        message(FATAL_ERROR "No WDK versions found under ${WDK_ROOT}/Include/")
    endif()
    list(SORT _wdk_versions COMPARE NATURAL ORDER DESCENDING)
    list(GET _wdk_versions 0 WDK_VERSION)
    set(WDK_VERSION "${WDK_VERSION}" CACHE STRING "WDK version")
endif()

message(STATUS "WDK version: ${WDK_VERSION}")

# --------------------------------------------------------------------------
# Determine target architecture
# --------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(_wdk_arch "ARM64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_wdk_arch "x64")
else()
    set(_wdk_arch "x86")
endif()

# Allow override
if(WDK_TARGET_ARCH)
    set(_wdk_arch "${WDK_TARGET_ARCH}")
endif()

message(STATUS "WDK target arch: ${_wdk_arch}")

# --------------------------------------------------------------------------
# Find KMDF version
# --------------------------------------------------------------------------
if(NOT WDK_KMDF_VERSION)
    file(GLOB _kmdf_versions RELATIVE "${WDK_ROOT}/Include/wdf/kmdf"
         "${WDK_ROOT}/Include/wdf/kmdf/1.*")
    if(NOT _kmdf_versions)
        message(FATAL_ERROR "No KMDF versions found under ${WDK_ROOT}/Include/wdf/kmdf/")
    endif()
    list(SORT _kmdf_versions COMPARE NATURAL ORDER DESCENDING)
    list(GET _kmdf_versions 0 WDK_KMDF_VERSION)
    set(WDK_KMDF_VERSION "${WDK_KMDF_VERSION}" CACHE STRING "KMDF version")
endif()

message(STATUS "KMDF version: ${WDK_KMDF_VERSION}")

# --------------------------------------------------------------------------
# Set up paths
# --------------------------------------------------------------------------
set(WDK_INC_KM     "${WDK_ROOT}/Include/${WDK_VERSION}/km")
set(WDK_INC_SHARED "${WDK_ROOT}/Include/${WDK_VERSION}/shared")
set(WDK_INC_KM_CRT "${WDK_ROOT}/Include/${WDK_VERSION}/km/crt")
set(WDK_INC_KMDF   "${WDK_ROOT}/Include/wdf/kmdf/${WDK_KMDF_VERSION}")
set(WDK_LIB_KM     "${WDK_ROOT}/Lib/${WDK_VERSION}/km/${_wdk_arch}")
set(WDK_LIB_KMDF   "${WDK_ROOT}/Lib/wdf/kmdf/${_wdk_arch}/${WDK_KMDF_VERSION}")

# Verify critical paths exist
foreach(_dir ${WDK_INC_KM} ${WDK_INC_SHARED} ${WDK_LIB_KM})
    if(NOT EXISTS "${_dir}")
        message(FATAL_ERROR "WDK directory not found: ${_dir}")
    endif()
endforeach()

# --------------------------------------------------------------------------
# Architecture-specific defines
# --------------------------------------------------------------------------
if(_wdk_arch STREQUAL "ARM64")
    set(_wdk_arch_defines _ARM64_ _WIN64)
elseif(_wdk_arch STREQUAL "x64")
    set(_wdk_arch_defines _AMD64_ _WIN64)
else()
    set(_wdk_arch_defines _X86_ _WIN32)
endif()

# --------------------------------------------------------------------------
# Strip CMake's default user-mode settings for kernel drivers
# --------------------------------------------------------------------------
# Remove default user-mode libraries (kernel32.lib, user32.lib, etc.)
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
# Remove default /subsystem:console
string(REGEX REPLACE "/subsystem:[a-zA-Z]+" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
# Remove /RTC1 from debug flags (runtime checks need user-mode CRT)
string(REGEX REPLACE "/RTC[^ ]*" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
# Remove /MDd and /MD (kernel drivers don't use the CRT DLL)
string(REGEX REPLACE "/MD[d]?" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REGEX REPLACE "/MD[d]?" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}" CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "" CACHE STRING "" FORCE)

# --------------------------------------------------------------------------
# wdk_add_driver(<name> <sources...>)
# --------------------------------------------------------------------------
function(wdk_add_driver _target)
    add_executable(${_target} ${ARGN})

    # Output as .sys
    set_target_properties(${_target} PROPERTIES
        SUFFIX ".sys"
        OUTPUT_NAME "${_target}"
    )

    # Include directories
    target_include_directories(${_target} SYSTEM PRIVATE
        "${WDK_INC_KM}"
        "${WDK_INC_KM_CRT}"
        "${WDK_INC_SHARED}"
        "${WDK_INC_KMDF}"
    )

    # Parse KMDF major.minor from WDK_KMDF_VERSION for WDF bind negotiation
    string(REPLACE "." ";" _kmdf_ver_parts "${WDK_KMDF_VERSION}")
    list(GET _kmdf_ver_parts 0 _kmdf_major)
    list(GET _kmdf_ver_parts 1 _kmdf_minor)

    # Preprocessor defines
    target_compile_definitions(${_target} PRIVATE
        ${_wdk_arch_defines}
        UNICODE
        _UNICODE
        NTDDI_VERSION=0x0A00000C
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        WINNT=1
        POOL_NX_OPTIN=1
        DEPRECATE_DDK_FUNCTIONS=1
        KMDF_VERSION_MAJOR=${_kmdf_major}
        KMDF_VERSION_MINOR=${_kmdf_minor}
    )

    # Compiler flags (MSVC kernel mode)
    target_compile_options(${_target} PRIVATE
        /kernel         # Kernel-mode compilation
        /GS-            # No buffer security checks (kernel has its own)
        /Gy             # Function-level linking
        /Gm-            # No minimal rebuild
        /Zp8            # 8-byte struct alignment
        /GF             # String pooling
        /GR-            # No RTTI
        /EHs-c-         # No exceptions
        /W4             # Warning level 4
        /wd4100         # Unreferenced formal parameter (common in drivers)
        /wd4201         # Nameless struct/union (WDK headers use these)
        /wd4214         # Bit field types other than int (WDK headers)
    )

    # Debug/Release specific flags (NO /RTC1 - it requires user-mode CRT)
    target_compile_options(${_target} PRIVATE
        $<$<CONFIG:Debug>:/Zi /Od>
        $<$<CONFIG:Release>:/O2 /Oi>
    )

    # Linker flags
    target_link_options(${_target} PRIVATE
        /DRIVER
        /SUBSYSTEM:NATIVE
        /ENTRY:FxDriverEntry
        /NODEFAULTLIB
        /SECTION:INIT,d
        /INTEGRITYCHECK
        /MANIFEST:NO
        /OPT:REF
        /OPT:ICF
        /INCREMENTAL:NO
    )

    # Debug linker flags
    target_link_options(${_target} PRIVATE
        $<$<CONFIG:Debug>:/DEBUG>
    )

    # Link WDK libraries only - no user-mode libs
    target_link_libraries(${_target} PRIVATE
        "${WDK_LIB_KM}/ntoskrnl.lib"
        "${WDK_LIB_KM}/hal.lib"
        "${WDK_LIB_KM}/wmilib.lib"
        "${WDK_LIB_KM}/BufferOverflowFastFailK.lib"
        "${WDK_LIB_KM}/usbd.lib"
        "${WDK_LIB_KM}/wdmsec.lib"
        "${WDK_LIB_KMDF}/WdfDriverEntry.lib"
        "${WDK_LIB_KMDF}/WdfLdr.lib"
    )
endfunction()

set(WDK_FOUND TRUE)
