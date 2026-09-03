@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /wd4244 /Fo:..\build\ /Fd:..\build\ gen.c /Fe:..\build\gen.exe || exit /b 1
echo OK: ..\build\gen.exe
