@echo off
rem ===========================================================================
rem  Fat Princess Recomp - benchmark run that can be steered from outside
rem  the game (used for remote testing):
rem    pad.txt          a button mask to press (0x4000 = Cross, 0x0040 = Down,
rem                     0x0020 = Right, 0x0008 = Start); emptied when taken
rem    bench_shot.txt   create it to save the next frame to bench_frame.ppm
rem    bench_trace.txt  create it to record a 10 s timeline (bench_ftrace.txt)
rem    bench_quit.txt   create it to close the game cleanly
rem  Files live in the game folder (one up from tools). Log: bench.log.
rem  Settings still come from fatprincess.ini.
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0.."
set "ROOT=%CD%"
set "PAD_FILE=%ROOT%\pad.txt"
set "FP_EXIT_FILE=%ROOT%\bench_quit.txt"
set "FP_TRACE_FILE=%ROOT%\bench_trace.txt"
set "PS3RECOMP_VK_DUMP=%ROOT%\bench_frame.ppm"
set "PS3RECOMP_VK_DUMP_TRIGGER=%ROOT%\bench_shot.txt"
set "PS3_FTRACE=1000000:10:bench_ftrace.txt"
for %%f in (pad.txt bench_quit.txt bench_trace.txt bench_shot.txt bench_frame.ppm bench_ftrace.txt) do if exist %%f del /q %%f
build\FatPrincessRecomp.exe > bench.log 2>&1
echo Exit code %errorlevel% >> bench.log
