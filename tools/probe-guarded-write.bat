@echo off
REM OPT-IN PRE-RELEASE CHECK - run it by hand before a release that touches the GPU Assignment tab's writes.
REM It WRITES TWO DUMMY VALUES to the current user's GPU preferences
REM (HKCU\Software\Microsoft\DirectX\UserGpuPreferences, paths under C:\GameOptimizerProbe\gwp\) and removes them.
REM It refuses to run if either value already exists, touches no other value, and checks that by fingerprinting
REM every value of the key before and after.
REM NOT PART OF GATE A, because it touches the registry: Gate A must stay safe to run anywhere, any number of times.
REM What it proves: tests\guarded_write_probe.cpp, header. Exit code = the probe's: 0 only if every scenario
REM matched and the fingerprint matched; 90 no compiler; 91 the build failed.
setlocal
call "D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 90
set "ROOT=%~dp0.."
if not exist "%ROOT%\build\objgwp" mkdir "%ROOT%\build\objgwp"
cl /nologo /EHsc /Od /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /MT ^
 /I"%ROOT%\src" /Fo"%ROOT%\build\objgwp\\" /Fe"%ROOT%\build\guarded_write_probe.exe" ^
 "%ROOT%\tests\guarded_write_probe.cpp" "%ROOT%\src\gpu_pref.cpp" "%ROOT%\src\procwatch.cpp" "%ROOT%\src\util.cpp" ^
 /link /SUBSYSTEM:CONSOLE dxgi.lib ktmw32.lib advapi32.lib user32.lib ole32.lib shell32.lib shlwapi.lib psapi.lib
if errorlevel 1 (
  echo PROBE_BUILD=FAIL
  exit /b 91
)
"%ROOT%\build\guarded_write_probe.exe"
set "RESULT=%ERRORLEVEL%"
echo PROBE_EXIT=%RESULT%
exit /b %RESULT%
