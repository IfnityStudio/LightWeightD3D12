@echo off
REM Run from the repository root, no matter where the batch file was launched from.
cd /d "%~dp0.."

REM Prefer the Windows Python launcher because Python itself may not be on PATH.
py -3 scripts\create_sample.py

REM Keep the window open so errors and generated paths remain visible.
pause
