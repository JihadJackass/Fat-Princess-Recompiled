@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem  Fat Princess Recomp - Windows build
rem  Needs: Visual Studio 2022 (Community or Build Tools) with the workloads
rem    - "Desktop development with C++"
rem    - "C++ Clang Compiler for Windows"   (individual component)
rem    - "C++ CMake tools for Windows"       (brings cmake + ninja)
rem  Output: build\FatPrincessRecomp.exe     Log: build.log
rem  Optional: set JOBS=n before running to limit parallel compiles
rem            (each generated file can take 2-4 GB RAM while compiling).
rem ===========================================================================
cd /d "%~dp0"
set "LOG=%~dp0build.log"
echo [%date% %time%] build started > "%LOG%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found >> "%LOG%"
  echo ERROR: Visual Studio 2022 not found. See README.md.
  pause & exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo ERROR: Visual Studio found, but without the C++ tools. See README.md.
  echo ERROR: no VC tools >> "%LOG%"
  pause & exit /b 1
)
echo Visual Studio: %VSDIR%
echo VSDIR=%VSDIR% >> "%LOG%"

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0build_vcvars.log" 2>&1
if errorlevel 1 ( echo ERROR: vcvars64 failed, see build_vcvars.log & pause & exit /b 1 )

rem vcvars does not always put VS's bundled clang, cmake and ninja on PATH
set "CMKDIR=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSDIR%\VC\Tools\Llvm\x64\bin;%CMKDIR%\Ninja;%CMKDIR%\CMake\bin;%PATH%"
where clang-cl >nul 2>&1
if errorlevel 1 (
  echo ERROR: clang-cl missing. In the Visual Studio Installer, add the
  echo        individual component "C++ Clang Compiler for Windows".
  pause & exit /b 1
)
where ninja >nul 2>&1 || ( echo ERROR: ninja missing - add "C++ CMake tools for Windows". & pause & exit /b 1 )
where cmake >nul 2>&1 || ( echo ERROR: cmake missing - add "C++ CMake tools for Windows". & pause & exit /b 1 )

if not defined JOBS (
  for /f %%m in ('powershell -NoProfile -Command "[math]::Max(1,[math]::Min([Environment]::ProcessorCount,[math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/3GB)))"') do set JOBS=%%m
)
echo Parallel jobs: %JOBS%   (override with: set JOBS=n)

echo.
echo Configuring...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl >> "%LOG%" 2>&1
if errorlevel 1 ( echo ERROR: configure failed - send build.log & pause & exit /b 1 )

echo Compiling about 3 million lines of recompiled code. The first build takes a while.
cmake --build build -j %JOBS% >> "%LOG%" 2>&1
if errorlevel 1 ( echo ERROR: build failed - send build.log & pause & exit /b 1 )

echo [%date% %time%] build OK >> "%LOG%"
echo.
echo Build OK. Start the game with FatPrincessLauncher.exe (or run.bat).
pause
