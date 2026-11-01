@echo off
rem Opens the NeighborCraft launcher window (install, update, uninstall, play, skin).
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0NeighborCraft.ps1"
