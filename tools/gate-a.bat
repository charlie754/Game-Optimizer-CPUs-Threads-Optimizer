@echo off
REM Gate A: the build is clean, the affinity-mask ban holds IN THE BINARY, tests pass.
setlocal enabledelayedexpansion
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
set "ROOT=%~dp0.."
set FAIL=0

echo === GATE A.1 build ===
REM The build's EXIT CODE is the gate, not the mere existence of an exe.
REM Measured 2026-08-28: this check used to print PASS whenever build\GameOptimizer.exe existed.
REM A run where build.bat died with "LNK1104: cannot open file" - the app was running and
REM holding the output file - therefore reported GATE_A1=PASS, and GATE_A2 then inspected the
REM STALE binary and passed on that too. A gate that silently reports on the PREVIOUS build is
REM worse than no gate. Exit code AND artifact freshness are both checked now.
call "%ROOT%\tools\build.bat"
set BUILDRC=%ERRORLEVEL%
if not "%BUILDRC%"=="0" (
  echo GATE_A1=FAIL build.bat exited %BUILDRC%
  set FAIL=1
) else if not exist "%ROOT%\build\GameOptimizer.exe" (
  echo GATE_A1=FAIL no exe produced
  set FAIL=1
) else (
  set "NEWEST="
  for /f "delims=" %%F in ('dir /b /o-d /a-d "%ROOT%\src\*.cpp" "%ROOT%\src\*.h" 2^>nul') do if not defined NEWEST set "NEWEST=%%F"
  set "FIRST="
  for /f "delims=" %%A in ('dir /b /o-d /a-d "%ROOT%\build\GameOptimizer.exe" "%ROOT%\src\!NEWEST!" 2^>nul') do if not defined FIRST set "FIRST=%%A"
  if /i "!FIRST!"=="GameOptimizer.exe" (
    echo GATE_A1=PASS build ok, exe newer than every source
  ) else (
    echo GATE_A1=FAIL exe is STALE - !NEWEST! is newer than the binary
    set FAIL=1
  )
)

echo.
echo === GATE A.2 SetProcessAffinityMask ban, checked in the BUILT BINARY ===
REM Grepping source cannot tell a call from a comment - applier.h's own header comment names
REM the banned API while explaining the ban, which made a source grep fail on a correct tree.
REM The import table cannot be faked by a comment. The POSITIVE CONTROL proves the check can
REM see these symbols at all, so it cannot pass vacuously.
if not exist "%ROOT%\build\GameOptimizer.exe" (
  echo GATE_A2=FAIL no binary to inspect
  set FAIL=1
) else (
  dumpbin /nologo /imports "%ROOT%\build\GameOptimizer.exe" > "%ROOT%\build\imports.txt" 2>&1
  findstr /C:"SetProcessDefaultCpuSets" "%ROOT%\build\imports.txt" >nul 2>&1
  if errorlevel 1 (
    echo GATE_A2=FAIL positive control missing - SetProcessDefaultCpuSets is not imported,
    echo             so this check proves nothing. Investigate before trusting a PASS.
    set FAIL=1
  ) else (
    findstr /C:"SetProcessAffinityMask" "%ROOT%\build\imports.txt" >nul 2>&1
    if errorlevel 1 (
      echo GATE_A2=PASS SetProcessAffinityMask not imported; positive control present
    ) else (
      echo GATE_A2=FAIL SetProcessAffinityMask IS imported by the binary
      set FAIL=1
    )
  )
)

echo.
echo === GATE A.3 unit tests ===
call "%ROOT%\tools\build-tests.bat"
if errorlevel 1 (
  echo GATE_A3=FAIL test build
  set FAIL=1
) else (
  "%ROOT%\build\cd_tests.exe"
  if errorlevel 1 ( echo GATE_A3=FAIL tests & set FAIL=1 ) else ( echo GATE_A3=PASS )
)

echo.
echo === GATE A RESULT: FAIL=%FAIL% ===
exit /b %FAIL%
