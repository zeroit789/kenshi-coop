@echo off
echo ========================================
echo   Kenshi-Online Multiplayer Test
echo   Thread Safety Fix - Phase 1
echo ========================================
echo.

:: Kill any existing Kenshi instances
echo [1] Cleaning up existing processes...
taskkill /F /IM kenshi_x64.exe 2>nul
taskkill /F /IM KenshiMP.Server.exe 2>nul
timeout /t 2 /nobreak >nul

:: Start server
echo.
echo [2] Starting KenshiMP Server on port 7777...
start "KenshiMP Server" /MIN cmd /c "cd /d "%~dp0" && KenshiMP.Server.exe"
timeout /t 3 /nobreak >nul

:: Launch Kenshi instance 1 (Host)
echo.
echo [3] Launching Kenshi Instance 1 (HOST)...
echo     - Should auto-connect as host
echo     - Wait for main menu, then New Game or Load
start "Kenshi Host" /D "C:\Program Files (x86)\Steam\steamapps\common\Kenshi" kenshi_x64.exe
timeout /t 10 /nobreak >nul

:: Launch Kenshi instance 2 (Client)
echo.
echo [4] Launching Kenshi Instance 2 (CLIENT)...
echo     - Should auto-connect as client
echo     - Wait for main menu, then New Game or Load
start "Kenshi Client" /D "C:\Program Files (x86)\Steam\steamapps\common\Kenshi" kenshi_x64.exe
timeout /t 5 /nobreak >nul

echo.
echo ========================================
echo   TEST RUNNING
echo ========================================
echo.
echo Both Kenshi instances are launching...
echo.
echo WHAT TO WATCH FOR:
echo   [SUCCESS] Both instances reach main menu
echo   [SUCCESS] Both can load/start game without crash
echo   [SUCCESS] Players see each other with FULL MODELS
echo   [SUCCESS] Movement is synchronized
echo   [SUCCESS] No crash for 30+ minutes
echo.
echo   [FAIL] Crash on load
echo   [FAIL] Players invisible
echo   [FAIL] Crash when moving
echo.
echo Press Ctrl+C to stop monitoring, or close this window.
echo.

:: Monitor for crashes
:monitor
timeout /t 10 /nobreak >nul
tasklist /FI "IMAGENAME eq kenshi_x64.exe" 2>NUL | find /I /N "kenshi_x64.exe">NUL
if "%ERRORLEVEL%"=="1" (
    echo.
    echo [ALERT] Kenshi process crashed or closed!
    echo Check logs: KenshiOnline_Client.log
    goto :end
)

tasklist /FI "IMAGENAME eq KenshiMP.Server.exe" 2>NUL | find /I /N "KenshiMP.Server.exe">NUL
if "%ERRORLEVEL%"=="1" (
    echo.
    echo [ALERT] Server process crashed or closed!
    echo Check logs: KenshiOnline_Server.log
    goto :end
)

echo [%time%] Monitoring... (Kenshi running, no crash detected)
goto :monitor

:end
echo.
echo Test ended.
pause
