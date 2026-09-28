@echo off
rem Hollow launcher: double-click for the menu, or "hollow <command>" (see hollow.py).
setlocal
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
%PY% "%~dp0hollow.py" %*
set "ERR=%ERRORLEVEL%"
if "%ERR%"=="9009" echo Python 3 is needed: https://www.python.org/downloads/ (tick "Add python.exe to PATH" when installing)
if not "%ERR%"=="0" if "%~1"=="" pause
exit /b %ERR%
