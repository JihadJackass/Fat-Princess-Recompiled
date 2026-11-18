@echo off
rem bench.bat with the CPU profiler on (profile.txt every 30 s).
set "FP_PROFILE=1"
call "%~dp0bench.bat"
