@echo off
setlocal
rem Builds SteelBattery.exe and runs the tests. Needs Visual Studio (Build Tools) with C++.
set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
set CF=/nologo /std:c++20 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Foout\

cl %CF% /O2 /Feout\decode_test.exe tests\decode_test.cpp src\battery.cpp setupapi.lib hid.lib cfgmgr32.lib || exit /b 1
out\decode_test.exe || exit /b 1
cl %CF% /O2 /Feout\history_test.exe tests\history_test.cpp src\history.cpp || exit /b 1
out\history_test.exe || exit /b 1
cl %CF% /O2 /Feout\oled_preview.exe tests\oled_preview.cpp src\oled.cpp src\render.cpp src\history.cpp ^
   user32.lib gdi32.lib gdiplus.lib winhttp.lib ole32.lib || exit /b 1

cl %CF% /O1 /Feout\SteelBattery.exe src\main.cpp src\battery.cpp src\render.cpp src\settings.cpp src\history.cpp src\oled.cpp ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest ^
   user32.lib gdi32.lib gdiplus.lib shell32.lib advapi32.lib setupapi.lib hid.lib cfgmgr32.lib winhttp.lib ole32.lib || exit /b 1
echo Built out\SteelBattery.exe
