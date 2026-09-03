@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem Builds a release artifact: a zip a user can extract and install without a
rem compiler. The version comes from QoiShellExt.rc, so there is one place to
rem bump it.

for /f "tokens=3" %%v in ('findstr /b /c:"#define VER_STRING" QoiShellExt.rc') do set VER=%%v
set VER=%VER:"=%
if "%VER%"=="" (
    echo Could not read VER_STRING from QoiShellExt.rc.
    exit /b 1
)

set NAME=QoiShellExt-%VER%-x64
set STAGE=dist\%NAME%
set ZIP=dist\%NAME%.zip

echo Building %NAME% ...
call "%~dp0build.cmd" all || exit /b 1

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%ZIP%" del /q "%ZIP%"
mkdir "%STAGE%" || exit /b 1

copy /y "build\QoiShellExt.dll" "%STAGE%\" >nul || exit /b 1
copy /y "install.cmd"           "%STAGE%\" >nul || exit /b 1
copy /y "uninstall.cmd"         "%STAGE%\" >nul || exit /b 1
copy /y "LICENSE"               "%STAGE%\" >nul || exit /b 1
copy /y "README.md"             "%STAGE%\" >nul || exit /b 1
copy /y "README.pt-BR.md"       "%STAGE%\" >nul || exit /b 1

powershell -NoProfile -Command ^
    "Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%' -Force" || exit /b 1

echo.
echo Artifact: %ZIP%
for /f "tokens=1" %%h in ('powershell -NoProfile -Command ^
    "(Get-FileHash '%ZIP%' -Algorithm SHA256).Hash"') do echo SHA256:   %%h
echo.
echo The zip carries the DLL next to install.cmd, so the user runs
echo install.cmd as administrator - no build step, no compiler.
