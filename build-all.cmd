@echo off
rem Builds every format, 64-bit and 32-bit, into bin\<format>.
call "%~dp0hollow.cmd" build all
pause
