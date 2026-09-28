@echo off
rem Opens the skin editor in your browser on FSVR's skin. Leave this window open while you edit; close it to stop.
rem Optional: another skin folder to open, e.g. editor.cmd D:\MySynth\skin (the Skin button switches later).
call "%~dp0hollow.cmd" editor %*
if errorlevel 1 pause
