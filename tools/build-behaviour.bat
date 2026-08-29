@echo off
setlocal
call "D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
pushd "%ROOT%\build"
cl /nologo /EHsc /O2 /std:c++17 /MT /Fe:behaviour_probe.exe "%ROOT%\tools\probe\behaviour_probe.cpp"
set RC=%ERRORLEVEL%
popd
echo BUILD_EXIT=%RC%
exit /b %RC%
