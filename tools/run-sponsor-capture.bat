@echo off
setlocal
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found & exit /b 90 )
call "%VCVARS%" >nul 2>&1
set "ROOT=%~dp0.."
set "OUT=%~f1"
if "%OUT%"=="" set "OUT=%ROOT%\build\capture-gdi"
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%ROOT%\build\objsponsor" mkdir "%ROOT%\build\objsponsor"
pushd "%ROOT%\build"
cl /nologo /utf-8 /EHsc /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /DCD_SPONSOR_HARNESS ^
   /I"%ROOT%\src" /I"%ROOT%\tests" /Fo"objsponsor\\" /Fe"sponsor_capture_harness.exe" ^
   "%ROOT%\tests\sponsor_capture_harness.cpp" ^
   "%ROOT%\src\sponsor.cpp" "%ROOT%\src\banner_host.cpp" ^
   "%ROOT%\src\online_banner.cpp" "%ROOT%\src\online_banner_machine.cpp" "%ROOT%\src\online_banner_client.cpp" ^
   "%ROOT%\src\theme.cpp" "%ROOT%\src\util.cpp" "%ROOT%\src\qr_magnifier.cpp" ^
   /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib advapi32.lib ^
   shlwapi.lib winhttp.lib windowscodecs.lib bcrypt.lib msimg32.lib gdiplus.lib delayimp.lib /DELAYLOAD:gdiplus.dll
set RC=%ERRORLEVEL%
if not "%RC%"=="0" ( popd & exit /b %RC% )
popd
"%ROOT%\build\sponsor_capture_harness.exe" "%OUT%" %2 %3 %4 %5
set RC=%ERRORLEVEL%
exit /b %RC%
