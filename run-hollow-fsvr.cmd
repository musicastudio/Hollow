@echo off
rem Starts the FSVR standalone (the FS1R editor). It reads ..\FSVR\plugin\skin live, so saves in the editor show at once.
call "%~dp0hollow.cmd" run %*
if errorlevel 1 pause
