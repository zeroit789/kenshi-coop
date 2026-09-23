@echo off
:: ============================================================
::  run_autotest.bat - Lanzador del autotest de Kenshi Co-op
::  Llama al script Python con el Python 3.12 del sistema.
:: ============================================================
REM EN: run_autotest.bat - launcher of the Kenshi Co-op autotest.
REM     Calls the Python script with the system Python 3.12.
setlocal

:: Ruta absoluta al Python 3.12 de Zero
REM EN: Absolute path to Python 3.12; hardcoded to an old user profile, C:\Users\Zero, that may not exist.
set PYTHON="C:\Users\Zero\AppData\Local\Programs\Python\Python312\python.exe"

:: Carpeta de este .bat (donde vive autotest_kenshi.py)
REM EN: Folder of this .bat, where autotest_kenshi.py lives
set DIR=%~dp0

echo ============================================================
echo   AUTOTEST KENSHI CO-OP
echo ============================================================
echo.

:: Pasa todos los argumentos (%*) al script Python.
:: Ej: run_autotest.bat --no-attack   /   run_autotest.bat --dry-run
REM EN: Passes every argument to the Python script. E.g. run_autotest.bat --no-attack or --dry-run
%PYTHON% "%DIR%autotest_kenshi.py" %*

echo.
echo ============================================================
echo   Autotest finalizado. Revisa el log indicado arriba.
echo ============================================================
endlocal
