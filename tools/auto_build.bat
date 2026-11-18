@echo off
rem ===========================================================================
rem  Fat Princess Recomp - unattended build: runs build.bat with its closing
rem  pause answered, so the window closes by itself. Details in build.log;
rem  auto_build.done (in the game folder) says "ok" or "failed" at the end.
rem ===========================================================================
cd /d "%~dp0.."
if exist auto_build.done del /q auto_build.done
echo.| call build.bat > auto_build.out 2>&1
findstr /c:"Build OK" auto_build.out >nul && (echo ok> auto_build.done) || (echo failed> auto_build.done)
