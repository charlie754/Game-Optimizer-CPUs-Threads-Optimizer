@echo off
setlocal
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found & exit /b 90 )
call "%VCVARS%" >nul 2>&1
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%ROOT%\build\objbanner" mkdir "%ROOT%\build\objbanner"
pushd "%ROOT%\build"
cl /nologo /utf-8 /EHsc /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   /I"%ROOT%\src" /I"%ROOT%\tests" /Fo"objbanner\\" /Fe"online_banner_client_harness.exe" ^
   "%ROOT%\tests\online_banner_client_harness.cpp" ^
   "%ROOT%\src\online_banner.cpp" "%ROOT%\src\online_banner_machine.cpp" "%ROOT%\src\online_banner_client.cpp" ^
   /link /SUBSYSTEM:CONSOLE bcrypt.lib ole32.lib user32.lib winhttp.lib windowscodecs.lib
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    popd
    exit /b %RC%
)
.\online_banner_client_harness.exe
set RC=%ERRORLEVEL%
popd
exit /b %RC%
