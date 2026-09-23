@echo off
REM ES: Desinstalador: localiza Kenshi, comprueba que no esta abierto, borra la DLL, el layout del panel,
REM     el servidor y el mod de data y mods, restaura Plugins_x64.cfg y el layout del menu desde las copias
REM     de seguridad, borra la carpeta de copias y opcionalmente la configuracion en APPDATA/KenshiMP.
REM     Uso: uninstall.bat. Ojo: no borra Kenshi_MultiplayerHUD.layout ni quita la linea de __mods.list.
REM EN: Uninstaller: finds Kenshi, checks it is not running, deletes the DLL, the panel layout, the server
REM     and the mod from data and mods, restores Plugins_x64.cfg and the menu layout from the backups,
REM     deletes the backup folder and optionally the config in APPDATA/KenshiMP.
REM     Usage: uninstall.bat. Note: it does not delete Kenshi_MultiplayerHUD.layout nor the __mods.list line.
setlocal enabledelayedexpansion
title KenshiMP Uninstaller
color 0C

echo.
echo  ============================================
echo   KenshiMP - Uninstaller
echo  ============================================
echo.

REM ES: Autodeteccion de la carpeta de Kenshi, o preguntar
:: ── Auto-detect Kenshi directory ──
set "KENSHI_DIR="

if exist "%~dp0kenshi_x64.exe" (
    set "KENSHI_DIR=%~dp0"
    goto :found
)
if exist "%~dp0..\kenshi_x64.exe" (
    set "KENSHI_DIR=%~dp0..\"
    goto :found
)
set "STEAM_KENSHI=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
if exist "%STEAM_KENSHI%\kenshi_x64.exe" (
    set "KENSHI_DIR=%STEAM_KENSHI%"
    goto :found
)
echo  Could not find Kenshi. Enter path:
set /p "KENSHI_DIR=Path: "
if not exist "%KENSHI_DIR%\kenshi_x64.exe" (
    echo  [ERROR] kenshi_x64.exe not found.
    pause
    exit /b 1
)

REM ES: Carpeta encontrada: quitar la barra final
REM EN: Folder found: strip the trailing backslash
:found
if "%KENSHI_DIR:~-1%"=="\" set "KENSHI_DIR=%KENSHI_DIR:~0,-1%"
echo  Found Kenshi at: %KENSHI_DIR%
echo.

REM ES: Comprobar si Kenshi esta en ejecucion
:: Check if running
tasklist /FI "IMAGENAME eq kenshi_x64.exe" 2>NUL | find /I "kenshi_x64.exe" >NUL
if %errorlevel% equ 0 (
    echo  [WARNING] Kenshi is running. Close it first.
    pause
    exit /b 1
)

set "BACKUP_DIR=%KENSHI_DIR%\KenshiMP_backup"

echo  Removing KenshiMP...
echo.

REM ES: Borrar la DLL
:: Remove DLL
if exist "%KENSHI_DIR%\KenshiMP.Core.dll" (
    del /F "%KENSHI_DIR%\KenshiMP.Core.dll"
    echo  [OK] Removed KenshiMP.Core.dll
)

REM ES: Borrar el layout del panel multijugador
:: Remove multiplayer panel layout
if exist "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerPanel.layout" (
    del /F "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerPanel.layout"
    echo  [OK] Removed Kenshi_MultiplayerPanel.layout
)

REM ES: Borrar el servidor
:: Remove server
if exist "%KENSHI_DIR%\KenshiMP.Server.exe" (
    del /F "%KENSHI_DIR%\KenshiMP.Server.exe"
    echo  [OK] Removed KenshiMP.Server.exe
)

REM ES: Borrar el mod de datos
:: Remove multiplayer mod
if exist "%KENSHI_DIR%\data\kenshi-online.mod" (
    del /F "%KENSHI_DIR%\data\kenshi-online.mod"
    echo  [OK] Removed data\kenshi-online.mod
)
if exist "%KENSHI_DIR%\mods\kenshi-online" (
    rmdir /S /Q "%KENSHI_DIR%\mods\kenshi-online"
    echo  [OK] Removed mods\kenshi-online\
)

REM ES: Restaurar las copias de seguridad
:: Restore backups
if exist "%BACKUP_DIR%\Plugins_x64.cfg.bak" (
    copy /Y "%BACKUP_DIR%\Plugins_x64.cfg.bak" "%KENSHI_DIR%\Plugins_x64.cfg" >nul
    echo  [OK] Restored original Plugins_x64.cfg
)

if exist "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" (
    copy /Y "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" >nul
    echo  [OK] Restored original Kenshi_MainMenu.layout
)

REM ES: Borrar la carpeta de copias
:: Clean up backup dir
if exist "%BACKUP_DIR%" (
    rmdir /S /Q "%BACKUP_DIR%" 2>nul
    echo  [OK] Removed backup folder
)

REM ES: Borrar la configuracion, previa confirmacion
:: Remove config
set "CONFIG_DIR=%APPDATA%\KenshiMP"
if exist "%CONFIG_DIR%" (
    echo.
    set /p "DELCONFIG=Delete KenshiMP config (%CONFIG_DIR%)? [y/N]: "
    if /i "!DELCONFIG!"=="y" (
        rmdir /S /Q "%CONFIG_DIR%"
        echo  [OK] Removed config folder
    )
)

echo.
echo  ============================================
echo   KenshiMP has been uninstalled.
echo   Your game is back to vanilla.
echo  ============================================
echo.
pause
