@echo off
setlocal
rem Stops the app, removes it from Windows startup and deletes the installed copy.
set "DEST=%LOCALAPPDATA%\Programs\SteelBattery"
taskkill /IM SteelBattery.exe >nul 2>nul
ping -n 3 127.0.0.1 >nul
taskkill /F /IM SteelBattery.exe >nul 2>nul
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v SteelBattery /f >nul 2>nul
if exist "%DEST%" rmdir /s /q "%DEST%"
echo SteelBattery kaldirildi.
