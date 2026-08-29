@echo off
REM Locate the Windows SDK x64 bin directory and prepend it to PATH.
REM
REM Why this exists: on this machine vcvars64.bat does NOT put the SDK bin on PATH, because
REM the SDK lives on D: and the registry key vcvars reads to find it is absent. The symptom
REM is "LINK : fatal error LNK1158: cannot run 'mt.exe'" - a link failure that looks like a
REM code problem and is not. Probing by path is the fix; do not assume a standard root.
setlocal enabledelayedexpansion
set "FOUND="
for %%R in (
  "D:\Program Files (x86)\Windows Kits\10\bin"
  "C:\Program Files (x86)\Windows Kits\10\bin"
  "%ProgramFiles(x86)%\Windows Kits\10\bin"
) do (
  if not defined FOUND if exist %%R (
    for /f "delims=" %%V in ('dir /b /ad /o-n %%R 2^>nul') do (
      if not defined FOUND if exist "%%~R\%%V\x64\mt.exe" set "FOUND=%%~R\%%V\x64"
    )
  )
)
if not defined FOUND (
  echo SDKBIN: mt.exe not found under any known Windows Kits root 1>&2
  exit /b 1
)
endlocal & set "SDKBIN=%FOUND%" & set "PATH=%FOUND%;%PATH%"
exit /b 0
