@echo off
rem Daily BugBot runner. Invokes run_bugbot.ps1 in this folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_bugbot.ps1"