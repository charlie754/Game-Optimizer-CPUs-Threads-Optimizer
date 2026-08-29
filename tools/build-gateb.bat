@echo off
REM Gate B end-to-end harness. Console subsystem.
REM
REM Links the probe against the product's own applier/engine/topology/config/procwatch, so
REM the checks exercise the SHIPPING code paths and not a re-implementation of them.
REM main.cpp / tray.cpp / settings.cpp / coremap.cpp are deliberately NOT compiled: they are
REM the GUI subsystem and bring a second wWinMain plus resource dependencies.
setlocal enabledelayedexpansion
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found & exit /b 90 )
call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvars64 failed & exit /b 91 )
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%ROOT%\build\objgateb" mkdir "%ROOT%\build\objgateb"
pushd "%ROOT%\build"
cl /nologo /EHsc /Od /Zi /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /MT ^
   /I"%ROOT%\src" /Fo"objgateb\\" /Fe"gateb_probe.exe" ^
   "%ROOT%\tools\probe\gateb_probe.cpp" ^
   "%ROOT%\src\applier.cpp" "%ROOT%\src\util.cpp" "%ROOT%\src\topology.cpp" ^
   "%ROOT%\src\config.cpp" "%ROOT%\src\procwatch.cpp" "%ROOT%\src\engine.cpp" ^
   "%ROOT%\src\games.cpp" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib shlwapi.lib psapi.lib ole32.lib
set RC=%ERRORLEVEL%
popd
echo BUILD_EXIT=%RC%
exit /b %RC%
