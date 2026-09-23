@echo off
REM ES: Script de compilacion del proyecto: comprueba CMake y Visual Studio 2019/2022, inicializa los
REM     submodulos de lib/ si faltan, configura CMake x64 en build/, compila todo en Release y ejecuta
REM     KenshiMP.UnitTest.exe. Uso: build.bat desde la raiz del repo. Sale con codigo 1 si algo falla.
REM EN: Project build script: checks CMake and Visual Studio 2019/2022, initializes the lib/ submodules
REM     if missing, configures CMake x64 into build/, builds everything in Release and runs
REM     KenshiMP.UnitTest.exe. Usage: build.bat from the repo root. Exits with code 1 on failure.
setlocal enabledelayedexpansion

echo.
echo  ========================================
echo   KenshiMP / Kenshi-Online Build Script
echo  ========================================
echo.

REM ES: Comprobar requisitos: CMake en el PATH
:: ── Check prerequisites ──
where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found in PATH.
    echo         Install CMake 3.20+ from https://cmake.org/download/
    echo         Or install via: winget install Kitware.CMake
    goto :fail
)

REM ES: Detectar Visual Studio: elige el generador de CMake segun la version instalada
:: ── Detect Visual Studio ──
set "VS_GEN="
if exist "%ProgramFiles%\Microsoft Visual Studio\2022" (
    set "VS_GEN=Visual Studio 17 2022"
    echo [OK] Found Visual Studio 2022
) else if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019" (
    set "VS_GEN=Visual Studio 16 2019"
    echo [OK] Found Visual Studio 2019
) else (
    echo [ERROR] Visual Studio 2019 or 2022 not found.
    echo         Install Visual Studio with "Desktop development with C++" workload.
    goto :fail
)

REM ES: Comprobar submodulos: si falta lib/enet los inicializa con git
:: ── Check submodules ──
if not exist "lib\enet\CMakeLists.txt" (
    echo [WARN] Submodules not initialized. Running git submodule update...
    git submodule update --init --recursive
    if errorlevel 1 (
        echo [ERROR] Failed to initialize submodules.
        echo         Make sure git is installed and you cloned with: git clone --recursive
        goto :fail
    )
)

REM ES: Configurar CMake para x64
:: ── Configure ──
echo.
echo [1/3] Configuring CMake (x64 Release)...
if not exist "build" mkdir build
cmake -G "%VS_GEN%" -A x64 -S . -B build
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    goto :fail
)

REM ES: Compilar todos los objetivos en Release
:: ── Build ──
echo.
echo [2/3] Building all targets (Release)...
cmake --build build --config Release
if errorlevel 1 (
    echo [ERROR] Build failed.
    goto :fail
)

REM ES: Ejecutar los tests unitarios; un fallo solo avisa, no aborta
:: ── Run tests ──
echo.
echo [3/3] Running unit tests...
build\bin\Release\KenshiMP.UnitTest.exe
if errorlevel 1 (
    echo [WARN] Some unit tests failed.
) else (
    echo [OK] All tests passed.
)

echo.
echo  ========================================
echo   BUILD SUCCESSFUL
echo  ========================================
echo.
echo  Output binaries in: build\bin\Release\
echo.
echo  Key files:
echo    KenshiMP.Core.dll      - Client plugin (auto-deployed to Kenshi dir)
echo    KenshiMP.Server.exe    - Dedicated server (auto-deployed to Kenshi dir)
echo    KenshiMP.Injector.exe  - Launcher / installer
echo.
echo  To open in Visual Studio:
echo    build\KenshiMP.sln
echo.
goto :end

REM ES: Salida con error / EN: error exit
:fail
echo.
echo  BUILD FAILED - see errors above.
echo.
exit /b 1

REM ES: Fin normal / EN: normal end
:end
endlocal
