@echo off
setlocal
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at "%VCVARS%"
  exit /b 90
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: vcvars64 failed
  exit /b 91
)
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
pushd "%ROOT%\build"
cl /nologo /EHsc /O2 /std:c++17 /W3 /Fe:topology_probe.exe "%ROOT%\tools\probe\topology_probe.cpp"
set RC=%ERRORLEVEL%
popd
echo BUILD_EXIT=%RC%
exit /b %RC%
