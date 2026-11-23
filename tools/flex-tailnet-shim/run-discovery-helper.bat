@echo off
rem Runs discovery_helper.py so SmartSDR, DAX and CAT for Windows can list
rem a radio on the tailnet. Close this window to stop it.
rem
rem The radio's tailnet name defaults to flex-radio. Set the TAILNET_SHIM
rem environment variable to change it (setx TAILNET_SHIM my-radio), or pass
rem --shim my-radio. Other arguments go to the helper.
rem
rem Uses Python from python.org. Under the Microsoft Store Python the helper
rem connected but its broadcasts never reached DAX or CAT.
setlocal
set "SHIM=flex-radio"
if defined TAILNET_SHIM set "SHIM=%TAILNET_SHIM%"
set "PY="
for /d %%D in ("%ProgramFiles%\Python3*" "%LOCALAPPDATA%\Programs\Python\Python3*") do (
  if exist "%%D\python.exe" set "PY=%%D\python.exe"
)
if not defined PY (
  echo Python was not found. Install it from python.org, not the Microsoft Store.
  pause
  exit /b 1
)
title Discovery helper - close this window to stop it
"%PY%" "%~dp0discovery_helper.py" --shim "%SHIM%" %*
echo.
echo The discovery helper has stopped.
pause
