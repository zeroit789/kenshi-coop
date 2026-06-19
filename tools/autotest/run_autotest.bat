@echo off
:: ============================================================
::  run_autotest.bat - Lanzador del autotest de Kenshi Co-op
::  Onyx ejecuta esto. Llama al script Python con el Python 3.12 del sistema.
:: ============================================================
setlocal

:: Ruta absoluta al Python 3.12 de Zero
set PYTHON="C:\Users\Zero\AppData\Local\Programs\Python\Python312\python.exe"

:: Carpeta de este .bat (donde vive autotest_kenshi.py)
set DIR=%~dp0

echo ============================================================
echo   AUTOTEST KENSHI CO-OP
echo ============================================================
echo.

:: Pasa todos los argumentos (%*) al script Python.
:: Ej: run_autotest.bat --no-attack   /   run_autotest.bat --dry-run
%PYTHON% "%DIR%autotest_kenshi.py" %*

echo.
echo ============================================================
echo   Autotest finalizado. Revisa el log indicado arriba.
echo ============================================================
endlocal
