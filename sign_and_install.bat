@echo off
setlocal

set WDKBIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\arm64
set WDKBIN_X86=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86
set BUILDDIR=C:\driver\build\arm64-debug
set INFDIR=C:\driver\inf
set CERTNAME=CH341ARM64Test
set CERTFILE=%BUILDDIR%\CH341Test.cer

echo === CH341 ARM64 Driver Sign and Install ===
echo.

:: Verify test signing is on
echo [0] Checking test signing...
bcdedit /enum {current} | findstr /i "testsigning" | findstr /i "Yes" >nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Test signing is not enabled. Run as admin:
    echo   bcdedit /set testsigning on
    echo Then reboot and run this script again.
    pause
    exit /b 1
)
echo     Test signing is ON.

:: Stage files
cd /d "%BUILDDIR%"
copy /Y "%INFDIR%\ch341arm64.inf" . >nul

:: Create test certificate, export it, and trust it
echo [1/6] Creating and trusting test certificate...
powershell -NoProfile -Command ^
  "$existing = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq 'CN=%CERTNAME%' };" ^
  "if (-not $existing) {" ^
  "  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=%CERTNAME%' -CertStoreLocation 'Cert:\LocalMachine\My' -FriendlyName 'CH341 ARM64 Test Cert';" ^
  "  Write-Host 'Certificate created:' $cert.Thumbprint;" ^
  "} else {" ^
  "  $cert = $existing[0];" ^
  "  Write-Host 'Certificate exists:' $cert.Thumbprint;" ^
  "};" ^
  "Export-Certificate -Cert $cert -FilePath '%CERTFILE%' -Force | Out-Null;" ^
  "Write-Host 'Exported to %CERTFILE%'"

echo [2/6] Adding certificate to trusted stores...
certutil -addstore Root "%CERTFILE%" >nul 2>&1
certutil -addstore TrustedPublisher "%CERTFILE%" >nul 2>&1
echo     Added to Root and TrustedPublisher stores.

:: Create catalog file
echo [3/6] Creating catalog file...
"%WDKBIN_X86%\inf2cat.exe" /driver:"%BUILDDIR%" /os:10_NI_ARM64 /uselocaltime 2>nul
if not exist "%BUILDDIR%\ch341arm64.cat" (
    "%WDKBIN_X86%\inf2cat.exe" /driver:"%BUILDDIR%" /os:10_CO_ARM64 /uselocaltime 2>nul
)
if not exist "%BUILDDIR%\ch341arm64.cat" (
    "%WDKBIN_X86%\inf2cat.exe" /driver:"%BUILDDIR%" /os:10_GE_ARM64 /uselocaltime 2>nul
)
if not exist "%BUILDDIR%\ch341arm64.cat" (
    echo ERROR: inf2cat failed to create catalog. Check INF file.
    pause
    exit /b 1
)
echo     Catalog created.

:: Sign catalog
echo [4/6] Signing catalog...
"%WDKBIN%\signtool.exe" sign /s My /sm /n "%CERTNAME%" /fd SHA256 /t http://timestamp.digicert.com "%BUILDDIR%\ch341arm64.cat"
if %ERRORLEVEL% NEQ 0 (
    echo     Retrying without timestamp...
    "%WDKBIN%\signtool.exe" sign /s My /sm /n "%CERTNAME%" /fd SHA256 "%BUILDDIR%\ch341arm64.cat"
)

:: Sign driver binary
echo [5/6] Signing driver binary...
"%WDKBIN%\signtool.exe" sign /s My /sm /n "%CERTNAME%" /fd SHA256 /t http://timestamp.digicert.com "%BUILDDIR%\ch341arm64.sys"
if %ERRORLEVEL% NEQ 0 (
    echo     Retrying without timestamp...
    "%WDKBIN%\signtool.exe" sign /s My /sm /n "%CERTNAME%" /fd SHA256 "%BUILDDIR%\ch341arm64.sys"
)

:: Verify signature
echo.
echo     Verifying signature...
"%WDKBIN%\signtool.exe" verify /pa "%BUILDDIR%\ch341arm64.sys" >nul 2>&1 && echo     SYS signature OK. || echo     SYS verify warning (expected for test certs)
"%WDKBIN%\signtool.exe" verify /pa "%BUILDDIR%\ch341arm64.cat" >nul 2>&1 && echo     CAT signature OK. || echo     CAT verify warning (expected for test certs)

:: Install driver
echo [6/6] Installing driver...
pnputil /add-driver "%BUILDDIR%\ch341arm64.inf" /install

echo.
echo === Done ===
echo If install succeeded, plug in the CH340 cable and check Device Manager.
echo.
pause
