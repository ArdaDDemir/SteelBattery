@echo off
setlocal
rem Copies the built app to a fixed per-user location, makes it start with Windows and (re)starts it.
rem Run again after every build.bat to update.
set "DEST=%LOCALAPPDATA%\Programs\SteelBattery"
if not exist "%~dp0out\SteelBattery.exe" (echo Once build.bat calistirin. & exit /b 1)

rem Polite close first (lets it give the taskbar space back), then force if still running.
taskkill /IM SteelBattery.exe >nul 2>nul
ping -n 3 127.0.0.1 >nul
taskkill /F /IM SteelBattery.exe >nul 2>nul

if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0out\SteelBattery.exe" "%DEST%\SteelBattery.exe" >nul || (echo Kopyalanamadi. & exit /b 1)
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v SteelBattery /t REG_SZ /d "\"%DEST%\SteelBattery.exe\"" /f >nul || exit /b 1
start "" "%DEST%\SteelBattery.exe"
echo Kuruldu: %DEST%\SteelBattery.exe  (Windows ile baslar)
