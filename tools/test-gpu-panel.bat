@echo off
setlocal
call "D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 90
set "ROOT=%~dp0.."
if not exist "%ROOT%\build\objpanel" mkdir "%ROOT%\build\objpanel"
pushd "%ROOT%\build"
cl /nologo /EHsc /Od /Zi /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /MT ^
 /I"%ROOT%\src" /Fo"objpanel\\" /Fe"gpu_panel_tests.exe" ^
 "%ROOT%\tests\gpu_panel_regression.cpp" "%ROOT%\src\gpu_pref.cpp" "%ROOT%\src\theme.cpp" ^
 "%ROOT%\src\config.cpp" "%ROOT%\src\util.cpp" "%ROOT%\src\procwatch.cpp" "%ROOT%\src\topology.cpp" ^
 /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib shlwapi.lib psapi.lib ole32.lib gdi32.lib comctl32.lib msimg32.lib
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" goto done
"%ROOT%\build\gpu_panel_tests.exe"
set "RESULT=%ERRORLEVEL%"
:done
popd
echo PANEL_TEST_EXIT=%RESULT%
exit /b %RESULT%
