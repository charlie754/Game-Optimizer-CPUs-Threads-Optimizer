@echo off
REM Syntax-check ONE translation unit.  Usage: cc.bat <path-to-cpp> [objdir]
setlocal
set "VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvars64.bat not found & exit /b 90 )
call "%VCVARS%" >nul 2>&1
set "ROOT=%~dp0.."
set "OBJDIR=%~2"
if "%OBJDIR%"=="" set "OBJDIR=%ROOT%\build\obj-check"
if not exist "%OBJDIR%" mkdir "%OBJDIR%" 2>nul
cl /nologo /c /EHsc /std:c++17 /W3 /permissive- /DUNICODE /D_UNICODE /I"%ROOT%\src" /Fo"%OBJDIR%\\" "%~1"
set RC=%ERRORLEVEL%
echo CC_EXIT=%RC%
exit /b %RC%
