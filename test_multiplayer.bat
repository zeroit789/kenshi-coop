@echo off
REM ES: Prueba manual de dos clientes en el mismo PC: mata Kenshi y el servidor, arranca
REM     KenshiMP.Server.exe desde la carpeta del .bat, lanza dos instancias de Kenshi desde la ruta de
REM     Steam por defecto en Program Files y vigila cada 10 s si alguno se cierra. Ojo: el mensaje habla
REM     del puerto 7777, pero el resto del proyecto usa 27800 por defecto.
REM EN: Manual two-client test on the same PC: kills Kenshi and the server, starts KenshiMP.Server.exe
REM     from the .bat folder, launches two Kenshi instances from the default Steam path under Program Files
REM     and checks every 10 s whether either one closed. Note: the message mentions port 7777, but the
REM     rest of the project uses 27800 by default.
echo ========================================
echo   Kenshi-Online Multiplayer Test
echo   Thread Safety Fix - Phase 1
echo ========================================
echo.

REM ES: Matar instancias previas de Kenshi y del servidor
:: Kill any existing Kenshi instances
echo [1] Cleaning up existing processes...
taskkill /F /IM kenshi_x64.exe 2>nul
taskkill /F /IM KenshiMP.Server.exe 2>nul
timeout /t 2 /nobreak >nul

REM ES: Arrancar el servidor
:: Start server
echo.
echo [2] Starting KenshiMP Server on port 7777...
start "KenshiMP Server" /MIN cmd /c "cd /d "%~dp0" && KenshiMP.Server.exe"
timeout /t 3 /nobreak >nul

REM ES: Lanzar la instancia 1 de Kenshi, host
:: Launch Kenshi instance 1 (Host)
echo.
echo [3] Launching Kenshi Instance 1 (HOST)...
echo     - Should auto-connect as host
echo     - Wait for main menu, then New Game or Load
start "Kenshi Host" /D "C:\Program Files (x86)\Steam\steamapps\common\Kenshi" kenshi_x64.exe
timeout /t 10 /nobreak >nul

REM ES: Lanzar la instancia 2 de Kenshi, cliente
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

REM ES: Bucle de vigilancia de cierres
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
