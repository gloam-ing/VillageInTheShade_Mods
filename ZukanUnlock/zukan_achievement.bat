@echo off
setlocal
chcp 65001 >nul
title Zukan Achievement Unlock
cd /d "%~dp0"

set "SCRIPT=%~dp0zukan_achievement.py"
if not exist "%SCRIPT%" (
    echo ERROR: zukan_achievement.py not found.
    pause
    exit /b 1
)

set "PY="
if defined PY_CMD set "PY=%PY_CMD%"
if not defined PY (
    where py >nul 2>nul && set "PY=py -3"
)
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo Python not found. Trying winget install...
    winget install --id Python.Python.3.12 -e --silent ^
        --accept-package-agreements --accept-source-agreements >nul 2>nul
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo Please install Python from https://www.python.org/downloads/ and retry.
    pause
    exit /b 1
)

%PY% -c "import lz4" >nul 2>nul
if errorlevel 1 (
    echo Installing lz4...
    %PY% -m pip install --quiet lz4
    if errorlevel 1 (
        echo lz4 install failed. Check network and retry.
        pause
        exit /b 1
    )
)

set "SAVE=%SAVE_PATH%"
if not defined SAVE (
    echo.
    echo Drag your save file into this window and press Enter.
    set /p "SAVE=Save path: "
)
set "SAVE=%SAVE:"=%"
if not exist "%SAVE%" (
    echo Save file not found: %SAVE%
    pause
    exit /b 1
)

echo.
echo Working... (backup + write + verify)
%PY% "%SCRIPT%" "%SAVE%"
echo.
pause
endlocal
