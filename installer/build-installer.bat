@echo off
setlocal

echo ============================================
echo  Motion CLI - Build Installer
echo ============================================
echo.

set "NSIS="
if exist "C:\Program Files (x86)\NSIS\makensis.exe" (
    set "NSIS=C:\Program Files (x86)\NSIS\makensis.exe"
) else if exist "C:\Program Files\NSIS\makensis.exe" (
    set "NSIS=C:\Program Files\NSIS\makensis.exe"
) else (
    where makensis >nul 2>&1
    if %ERRORLEVEL% equ 0 (
        set "NSIS=makensis"
    )
)

if not defined NSIS (
    echo NSIS not found!
    echo.
    echo Install from: https://nsis.sourceforge.io/Download
    echo.
    pause
    exit /b 1
)

cd /d "%~dp0\.."

echo Building motioncli.exe ...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo Creating installer ...
"%NSIS%" installer\installer.nsi
if %ERRORLEVEL% neq 0 (
    echo Installer build failed!
    pause
    exit /b 1
)

echo.
echo Done: motioncli-setup.exe
echo.
pause
