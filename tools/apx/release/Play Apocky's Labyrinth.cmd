@echo off
rem Apocky's Labyrinth launcher: finds your own Steam copy of Barony and starts the game with its art.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher.ps1" %*
if errorlevel 1 pause
