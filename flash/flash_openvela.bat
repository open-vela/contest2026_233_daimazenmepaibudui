@echo off
rem ============================================================
rem  Openvela flash helper for SF32LB52-DevKit-LCD
rem
rem  KEY: the SFBL bootrom REQUIRES the partition table (ftab)
rem       at 0x12000000, otherwise it will not boot the image.
rem
rem  Usage:  flash_openvela.bat [nuttx.bin]
rem         (defaults to nuttx.bin in this directory)
rem ============================================================
setlocal
set WORK=%~dp0
cd /d "%WORK%"

if "%~1"=="" (set IMG=%WORK%nuttx.bin) else (set IMG=%~1)
if not exist "%IMG%" (
    echo [ERROR] firmware image not found: %IMG%
    echo Put nuttx.bin here or pass it as argument.
    pause
    exit /b 1
)

rem locate sftool: PATH -> SDK install -> local
set SFTOOL=sftool.exe
where sftool.exe >nul 2>nul || set SFTOOL=C:\Users\ahs\.sifli\tools\sftool\0.1.16\sftool.exe
if not exist "%SFTOOL%" if exist "%WORK%sftool.exe" set SFTOOL=%WORK%sftool.exe
if not exist "%SFTOOL%" (
    echo [ERROR] sftool not found. Install from
    echo   https://github.com/OpenSiFli/sftool/releases ^(0.2.5 recommended^)
    pause
    exit /b 1
)

set /p input=please input the serial port num:
"%SFTOOL%" -p COM%input% -c SF32LB52 -m nor --before default_reset --after soft_reset write_flash "%WORK%ftab.bin@0x12000000" "%IMG%@0x12010000"
echo.
echo Done. Open serial at 1000000 8N1, expect: SFBL / ABCD / NuttShell (NSH)
pause >nul
