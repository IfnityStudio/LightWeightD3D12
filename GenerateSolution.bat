@echo off
setlocal

REM Run from the repository root, no matter where the batch file was launched from.
cd /d "%~dp0"

REM Prefer the Windows Python launcher because Python itself may not be on PATH.
where py >nul 2>nul
if %ERRORLEVEL%==0 (
    py -3 scripts\configure_solution.py
) else (
    python scripts\configure_solution.py
)

if errorlevel 1 (
    echo.
    echo Setup failed.
) else (
    echo.
    echo Setup finished.
)

REM Keep the window open when launched by double click.
pause
