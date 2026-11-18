@echo off
rem ===========================================================================
rem  Fat Princess Recomp - run with graphics capture
rem  Same as run.bat, but saves 10 frames (frame 300, then every 200 frames)
rem  to the rsxcap folder in the game folder: a draw list per frame plus
rem  pictures of the screen and every off-screen render target. Play normally
rem  for 2-3 minutes, then quit.
rem ===========================================================================
cd /d "%~dp0.."
if exist rsxcap rmdir /s /q rsxcap
set "RSX_CAP=300:10:200"
set "RSX_CAP_DIR=%CD%\rsxcap"
call "%CD%\run.bat"
