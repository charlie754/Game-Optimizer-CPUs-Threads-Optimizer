@echo off
setlocal
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found & exit /b 90 )
call "%VCVARS%" >nul 2>&1
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%ROOT%\build\objwebview" mkdir "%ROOT%\build\objwebview"
pushd "%ROOT%\build"
cl /nologo /utf-8 /EHsc /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /DCD_WEBVIEW_HARNESS /MT ^
   /I"%ROOT%\src" /Fo"objwebview\\" /Fe"webview_host_harness.exe" ^
   "%ROOT%\tests\webview_host_harness.cpp" ^
   "%ROOT%\src\webview_host.cpp" "%ROOT%\src\banner_host.cpp" ^
   "%ROOT%\src\online_banner.cpp" "%ROOT%\src\online_banner_machine.cpp" "%ROOT%\src\online_banner_client.cpp" ^
   "%ROOT%\src\sponsor.cpp" "%ROOT%\src\theme.cpp" "%ROOT%\src\util.cpp" ^
   "%ROOT%\src\qr_magnifier.cpp" ^
   /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib ^
   advapi32.lib shlwapi.lib psapi.lib winhttp.lib windowscodecs.lib bcrypt.lib msimg32.lib gdiplus.lib
set RC=%ERRORLEVEL%
if not "%RC%"=="0" ( popd & exit /b %RC% )
if not exist "%ROOT%\third_party\webview2\x64\WebView2Loader.dll" (
  echo ERROR: third_party\webview2\x64\WebView2Loader.dll missing
  popd
  exit /b 91
)
copy /y "%ROOT%\third_party\webview2\x64\WebView2Loader.dll" "%ROOT%\build\WebView2Loader.dll" >nul
if errorlevel 1 ( popd & exit /b 92 )
popd
"%ROOT%\build\webview_host_harness.exe"
exit /b %ERRORLEVEL%
