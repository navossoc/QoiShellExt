@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\build mkdir ..\build
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE ^
   /Fo:..\build\ /Fd:..\build\ test.cpp /Fe:..\build\test.exe ^
   /link shlwapi.lib gdi32.lib ole32.lib user32.lib || exit /b 1
echo OK: ..\build\test.exe
