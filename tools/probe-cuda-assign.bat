@echo off
REM OPT-IN PRE-RELEASE CHECK - run it by hand before a release that touches src\gpu_cuda.cpp.
REM It WRITES ONE DUMMY APPLICATION PROFILE TO THE REAL NVIDIA DRIVER DATABASE
REM ("Game Optimizer - C:\GameOptimizerProbe\cuda\v1\gocudaassign.exe") and removes it again.
REM It backs the driver database up first, refuses to run if that profile or that executable is already
REM known to the driver, touches no other profile, and checks that by dumping every profile carrying
REM CUDA_EXCLUDED_GPUS_ID (0x10354FF8) before and after and comparing them.
REM NOT PART OF GATE A, and it must never be added to it: Gate A must stay safe to run anywhere, any
REM number of times, including on a machine with no NVIDIA driver at all.
REM What it proves: tests\cuda_assign_probe.cpp, header.
REM Exit code = the probe's: 0 every scenario as expected, the dump unchanged and the current database file
REM free of the dummy name; 1 unexpected, or the dump changed, or the current database still names the
REM dummy; 2 refused, the dummy already exists; 3 refused, a precondition failed; 4 CLEANUP FAILED.
REM 90 no compiler; 91 the build failed.
REM Pass "cleanup" through to run ONLY the guarded cleanup, for a run that died half-way:
REM   tools\probe-cuda-assign.bat cleanup
setlocal
call "D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 90
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%ROOT%\build\objcuda" mkdir "%ROOT%\build\objcuda"
cl /nologo /EHsc /Od /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /MT ^
 /I"%ROOT%\src" /Fo"%ROOT%\build\objcuda\\" /Fe"%ROOT%\build\cuda_assign_probe.exe" ^
 "%ROOT%\tests\cuda_assign_probe.cpp" "%ROOT%\src\gpu_cuda.cpp" "%ROOT%\src\gpu_pref.cpp" ^
 "%ROOT%\src\procwatch.cpp" "%ROOT%\src\util.cpp" ^
 /link /SUBSYSTEM:CONSOLE dxgi.lib ktmw32.lib advapi32.lib user32.lib ole32.lib shell32.lib shlwapi.lib psapi.lib
if errorlevel 1 (
  echo PROBE_BUILD=FAIL
  exit /b 91
)
REM Full path: this shell sets NoDefaultCurrentDirectoryInExePath=1, so a bare name would not resolve.
"%ROOT%\build\cuda_assign_probe.exe" %1
set "RESULT=%ERRORLEVEL%"
REM The probe prints its own PROBE_EXIT= line. This one is the process's real exit code, so the two
REM disagreeing would mean the output did not come from the run that ended.
echo PROBE_BAT_EXIT=%RESULT%
exit /b %RESULT%
