@echo off
rem Register (or remove) the daily BugBot scheduled task on THIS computer.
rem
rem   install_schedule.bat              -> create daily task at 09:00
rem   install_schedule.bat 12:30        -> create daily task at 12:30
rem   install_schedule.bat /delete      -> remove the task
rem
rem Run it from an elevated (Administrator) prompt. Re-run on whichever machine
rem should host the daily runs.

setlocal
set "TASKNAME=BugBot"
set "RUNNER=%~dp0run_bugbot.bat"
set "TIME=%~1"

if /i "%TIME%"=="/delete" (
    schtasks /delete /tn "%TASKNAME%" /f
    echo Removed scheduled task "%TASKNAME%".
    goto :eof
)

if "%TIME%"=="" set "TIME=09:00"

schtasks /create /tn "%TASKNAME%" /tr "\"%RUNNER%\"" /sc daily /st "%TIME%" /f
if errorlevel 1 (
    echo Failed to create the task. Try running this from an Administrator prompt.
    exit /b 1
)
echo Created daily task "%TASKNAME%" at %TIME% running: %RUNNER%
echo To change the host machine later, just re-run this .bat there.
endlocal