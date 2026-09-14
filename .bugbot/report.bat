@echo off
rem Quick launcher for the BugBot report capture script.
rem Usage: report.bat            (interactive prompts)
rem        report.bat "a bug note"  (fast capture, defaults to bug/medium)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0report.ps1" %*