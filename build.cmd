@echo off
rem Builds bin\Overlook.exe (x64, static CRT, no runtime dependencies).
setlocal
cd /d "%~dp0"
rem Needs Visual Studio 2022 or later (any edition, or the Build Tools) with "Desktop development
rem with C++" and a Windows 11 SDK. Run from a Developer Command Prompt, or let vswhere find it.
where cl >nul 2>nul && goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
goto :build
:novs
echo Visual Studio with the C++ x64 tools was not found. Install "Desktop development with C++".
exit /b 1

:build

if not exist obj mkdir obj
if not exist bin mkdir bin

rc /nologo /fo obj\app.res res\app.rc || exit /b 1

rem /DEPENDENTLOADFLAG:0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32): the DLLs the exe imports load from System32 only,
rem never from the exe's own folder (often Downloads), where a planted dwmapi.dll would otherwise win.
cl /nologo /std:c++20 /utf-8 /O2 /MT /EHsc /W4 /permissive- /GS /sdl /guard:cf ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 ^
   /Foobj\ /Fdobj\ ^
   src\main.cpp src\settings.cpp src\desktop.cpp src\render.cpp src\wallpaper.cpp src\color.cpp src\weather.cpp src\weatherline.cpp src\selftest.cpp src\icons.cpp src\media.cpp src\inifile.cpp src\settingswindow.cpp src\settingsuia.cpp src\weatherfx.cpp src\net.cpp src\update.cpp obj\app.res ^
   /Fe:bin\Overlook.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /guard:cf /CETCOMPAT /DEPENDENTLOADFLAG:0x800 ^
   user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib shcore.lib ^
   d2d1.lib dwrite.lib windowscodecs.lib winhttp.lib wtsapi32.lib windowsapp.lib d3d11.lib dxgi.lib dcomp.lib dxguid.lib uuid.lib dwmapi.lib comdlg32.lib uiautomationcore.lib oleacc.lib || exit /b 1

copy /y fonts\OFL.txt bin\OFL.txt >nul
echo Built bin\Overlook.exe
