@echo off
setlocal
cd /d "%~dp0"

rem Usage: install.cmd [all|thumbnail|preview]
rem
rem Registers only what you ask for. The two handlers are independent, so you
rem can install one now and add the other later without rebuilding.

set WHAT=%~1
if "%WHAT%"=="" set WHAT=all

if /i not "%WHAT%"=="all" if /i not "%WHAT%"=="thumbnail" if /i not "%WHAT%"=="preview" (
    echo Unknown option "%WHAT%". Use: all, thumbnail or preview.
    exit /b 1
)

rem Works both from the source tree and from an extracted release, where the
rem DLL sits next to this script.
set DLL=QoiShellExt.dll
if not exist "%DLL%" set DLL=build\QoiShellExt.dll
if not exist "%DLL%" (
    echo QoiShellExt.dll not found. Run build.cmd first.
    exit /b 1
)

net session >nul 2>&1
if errorlevel 1 (
    echo Run this script as administrator.
    exit /b 1
)

set DEST=%ProgramFiles%\navossoc\QoiShellExt
if not exist "%DEST%" mkdir "%DEST%"

rem If the DLL is already registered and loaded by a shell host, the copy fails
rem with "access denied"; killing the hosts releases it.
taskkill /f /im dllhost.exe >nul 2>&1
taskkill /f /im prevhost.exe >nul 2>&1

copy /y "%DLL%" "%DEST%\" >nul || exit /b 1
copy /y "LICENSE" "%DEST%\" >nul

rem /n /i: calls DllInstall instead of DllRegisterServer, so the keyword
rem decides which handlers are registered.
regsvr32 /s /n /i:"%WHAT%" "%DEST%\QoiShellExt.dll" || (
    echo regsvr32 failed. The DLL may have been built without the "%WHAT%" handler.
    exit /b 1
)

echo Registered from %DEST%\QoiShellExt.dll:
if /i "%WHAT%"=="all"       echo   - thumbnail provider and preview handler for .qoi
if /i "%WHAT%"=="thumbnail" echo   - thumbnail provider for .qoi
if /i "%WHAT%"=="preview"   echo   - preview handler for .qoi
echo.
echo Remaining steps:
echo  1. If another .qoi thumbnail or preview handler is installed, turn it off.
echo     Many of them re-register at startup and take the association back.
echo  2. Clear the thumbnail cache to see the change:
echo       del /q "%%LocalAppData%%\Microsoft\Windows\Explorer\thumbcache_*.db"
echo     ^(restart Explorer afterwards, or use Disk Cleanup^)
