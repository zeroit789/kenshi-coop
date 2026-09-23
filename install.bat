@echo off
setlocal

REM ES: Instalacion de KenshiMP en desarrollo: copia la DLL y el servidor compilados a la carpeta de
REM     Kenshi, se asume que el repo esta dentro de la carpeta del juego, y activa el plugin de Ogre en
REM     Plugins_x64.cfg. Solo comprueba los layouts del menu, no los parchea. Ejecutar tras compilar.
REM EN: Development install of KenshiMP: copies the built DLL and server into the Kenshi folder, the repo
REM     is assumed to live inside the game folder, and enables the Ogre plugin in Plugins_x64.cfg. It only
REM     checks the menu layouts, it does not patch them. Run after building.
:: KenshiMP Install - Copies built files and configures Kenshi to load the Ogre plugin.
:: Run this after building (cmake --build build --config Release).

REM ES: Rutas: carpeta de Kenshi = carpeta padre del repo, binarios de build y ficheros a tocar.
REM EN: Paths: Kenshi folder = parent of the repo, build binaries and files to touch.
set KENSHI_DIR=%~dp0..
set BUILD_DLL=%~dp0build\bin\Release\KenshiMP.Core.dll
set BUILD_SERVER=%~dp0build\bin\Release\KenshiMP.Server.exe
set PLUGINS_CFG=%KENSHI_DIR%\Plugins_x64.cfg
set MAIN_MENU_LAYOUT=%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout
set MP_PANEL_LAYOUT=%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerPanel.layout

echo ============================================
echo  KenshiMP Install
echo ============================================
echo.

REM ES: 1. Copiar la DLL a la raiz de Kenshi
:: 1. Copy DLL to Kenshi root
if exist "%BUILD_DLL%" (
    copy /Y "%BUILD_DLL%" "%KENSHI_DIR%\KenshiMP.Core.dll" >nul 2>&1
    if errorlevel 1 (
        echo [!] Failed to copy DLL - is Kenshi running? Close it first.
    ) else (
        echo [OK] Copied KenshiMP.Core.dll
    )
) else (
    echo [!] DLL not found. Build first: cmake --build build --config Release
)

REM ES: 2. Copiar el servidor a la raiz de Kenshi
:: 2. Copy Server exe to Kenshi root
if exist "%BUILD_SERVER%" (
    copy /Y "%BUILD_SERVER%" "%KENSHI_DIR%\KenshiMP.Server.exe" >nul 2>&1
    if errorlevel 1 (
        echo [!] Failed to copy Server exe - is it running?
    ) else (
        echo [OK] Copied KenshiMP.Server.exe
    )
) else (
    echo [--] Server exe not built yet (optional)
)

REM ES: 3. Asegurar la linea Plugin=KenshiMP.Core en Plugins_x64.cfg
:: 3. Ensure Plugin=KenshiMP.Core is in Plugins_x64.cfg
findstr /C:"Plugin=KenshiMP.Core" "%PLUGINS_CFG%" >nul 2>&1
if errorlevel 1 (
    echo Plugin=KenshiMP.Core>> "%PLUGINS_CFG%"
    echo [OK] Added Plugin=KenshiMP.Core to Plugins_x64.cfg
) else (
    echo [OK] Plugins_x64.cfg already has KenshiMP.Core
)

REM ES: 4. Comprobar el boton MULTIPLAYER en el layout del menu principal. Ojo: el mensaje dice que lo
REM     anade, pero el script no lo anade.
REM EN: 4. Check the MULTIPLAYER button in the main menu layout. Note: the message says it adds it,
REM     but the script does not add it.
:: 4. Check MULTIPLAYER button in main menu layout
findstr /C:"MULTIPLAYER" "%MAIN_MENU_LAYOUT%" >nul 2>&1
if errorlevel 1 (
    echo [!] MULTIPLAYER button not in Kenshi_MainMenu.layout - adding it...
    echo     NOTE: Auto-patching the layout is fragile. Check manually if it breaks.
) else (
    echo [OK] MULTIPLAYER button present in main menu
)

REM ES: 5. Comprobar el layout del panel multijugador
:: 5. Check multiplayer panel layout
if exist "%MP_PANEL_LAYOUT%" (
    echo [OK] Kenshi_MultiplayerPanel.layout exists
) else (
    echo [!] Kenshi_MultiplayerPanel.layout is missing!
    echo     It should be at: data\gui\layout\Kenshi_MultiplayerPanel.layout
)

echo.
echo ============================================
echo  Done. Launch Kenshi to play!
echo ============================================
pause
