@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  Fat Princess Recomp - console launcher (FatPrincessLauncher.exe is the
rem  normal way to play; this does the same without a window of its own).
rem  Log: run.log
rem  All settings live in fatprincess.ini (the launcher edits it).
rem ===========================================================================
cd /d "%~dp0"

set "GAMEDIR=%~dp0vfs\dev_hdd0\game\NPUA80139"
if not exist "%GAMEDIR%\USRDIR\GAME.elf" (
  echo ERROR: %GAMEDIR%\USRDIR\GAME.elf not found.
  pause & exit /b 1
)
if not exist "%GAMEDIR%\USRDIR\siege.qrk" (
  echo ERROR: siege.qrk missing. Copy it from RPCS3\dev_hdd0\game\NPUA80139\USRDIR\
  echo        into %GAMEDIR%\USRDIR\  ^(the launcher can do this for you^)
  pause & exit /b 1
)
if not exist build\FatPrincessRecomp.exe ( echo ERROR: build first - run build.bat & pause & exit /b 1 )

echo Starting Fat Princess... (log: run.log)
build\FatPrincessRecomp.exe > run.log 2>&1
echo Exit code %errorlevel% >> run.log
echo Game exited (code %errorlevel%). Log saved to run.log
pause
