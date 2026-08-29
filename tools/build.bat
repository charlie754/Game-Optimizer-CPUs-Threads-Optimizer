@echo off
REM Full build.  Produces build\GameOptimizer.exe
setlocal enabledelayedexpansion
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found at "%VCVARS%" & exit /b 90 )
call "%VCVARS%" >nul 2>&1
call "%~dp0sdkbin.bat"
if errorlevel 1 ( echo ERROR: Windows SDK bin not found ^(mt.exe^) & exit /b 93 )
set "ROOT=%~dp0.."
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
if not exist "%ROOT%\build\obj" mkdir "%ROOT%\build\obj"
set "SRCS="
for %%F in ("%ROOT%\src\*.cpp") do set "SRCS=!SRCS! "%%F""
if "!SRCS!"=="" ( echo ERROR: no sources in src\ & exit /b 92 )
pushd "%ROOT%\build"
cl /nologo /EHsc /O2 /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /MT ^
   /I"%ROOT%\src" /Fo"obj\\" /Fe"GameOptimizer.exe" !SRCS! ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%\src\GameOptimizer.manifest" ^
   user32.lib shell32.lib gdi32.lib advapi32.lib comctl32.lib ole32.lib shlwapi.lib psapi.lib comdlg32.lib msimg32.lib
set RC=%ERRORLEVEL%
popd

REM The WebView2 loader, next to the exe.
REM
REM It is loaded with LoadLibraryW at run time, never imported, so a missing copy costs the
REM sponsor strip its animation and NOTHING else - the app still starts and falls back to the
REM GDI strip. That is why this copy failing is a warning and not a build failure: it must
REM never be able to break a build that is otherwise green.
if "%RC%"=="0" (
  if exist "%ROOT%\third_party\webview2\x64\WebView2Loader.dll" (
    copy /y "%ROOT%\third_party\webview2\x64\WebView2Loader.dll" "%ROOT%\build\WebView2Loader.dll" >nul
    if errorlevel 1 ( echo WARNING: could not copy WebView2Loader.dll next to the exe )
  ) else (
    echo WARNING: third_party\webview2\x64\WebView2Loader.dll is missing - the sponsor strip
    echo          will fall back to GDI at run time.
  )
)

echo BUILD_EXIT=%RC%
exit /b %RC%
