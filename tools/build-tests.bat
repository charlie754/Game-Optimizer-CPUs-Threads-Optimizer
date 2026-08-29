@echo off
REM Unit-test harness. Console subsystem. Links the pure TUs plus their dependencies.
setlocal enabledelayedexpansion
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found & exit /b 90 )
call "%VCVARS%" >nul 2>&1
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%ROOT%\build\objtest" mkdir "%ROOT%\build\objtest"
pushd "%ROOT%\build"
cl /nologo /EHsc /Od /Zi /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /DCD_TESTS /MT ^
   /I"%ROOT%\src" /Fo"objtest\\" /Fe"cd_tests.exe" ^
   "%ROOT%\tests\test_main.cpp" ^
   "%ROOT%\src\topology.cpp" "%ROOT%\src\config.cpp" "%ROOT%\src\util.cpp" ^
   "%ROOT%\src\engine.cpp" "%ROOT%\src\procwatch.cpp" "%ROOT%\src\applier.cpp" ^
   "%ROOT%\src\games.cpp" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib shlwapi.lib psapi.lib ole32.lib
set RC=%ERRORLEVEL%
popd
echo TESTBUILD_EXIT=%RC%
exit /b %RC%
