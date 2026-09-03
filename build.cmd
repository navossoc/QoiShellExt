@echo off
setlocal

rem Usage: build.cmd [all|thumbnail|preview]
rem
rem "all" (the default) builds both handlers into one DLL; which of them gets
rem installed is still a choice made at install time. The other two produce a
rem DLL that only carries that one handler.

set FEATURES=%~1
if "%FEATURES%"=="" set FEATURES=all

if /i "%FEATURES%"=="all" (
    set DEFS=
) else if /i "%FEATURES%"=="thumbnail" (
    set DEFS=/DQOI_ENABLE_THUMBNAIL
) else if /i "%FEATURES%"=="preview" (
    set DEFS=/DQOI_ENABLE_PREVIEW
) else (
    echo Unknown option "%FEATURES%". Use: all, thumbnail or preview.
    exit /b 1
)

set VSDEV="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VSDEV% (
    echo vcvars64.bat not found at %VSDEV%
    exit /b 1
)
call %VSDEV% >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

rc /nologo /fo build\QoiShellExt.res QoiShellExt.rc || exit /b 1

rem /MT: no dependency on the redistributable runtime - the DLL is loaded by
rem the shell's own surrogate hosts, which will not deploy anything for us.
cl /nologo /std:c++17 /O2 /GL /MT /EHsc /W4 /GS /DNDEBUG /DUNICODE /D_UNICODE %DEFS% ^
   /Fo:build\ /Fd:build\ ^
   /LD QoiImage.cpp ThumbnailProvider.cpp PreviewHandler.cpp ShellExt.cpp ^
   /Fe:build\QoiShellExt.dll ^
   /link /DEF:exports.def /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   build\QoiShellExt.res ^
   shlwapi.lib gdi32.lib msimg32.lib ole32.lib advapi32.lib shell32.lib user32.lib ^
   || exit /b 1

echo.
echo OK: build\QoiShellExt.dll (handlers: %FEATURES%)
