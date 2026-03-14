@echo off
:: Register two test accounts (shadow and stephen) used by the other example scripts.
:: Run this once before trying the friend/block examples.
::
:: Accounts created:
::   npid=Shadow     password=1234     onlineName="Shadow"     email=george@test.local
::   npid=Stephen    password=12345    onlineName="Stephen"    email=stephen@test.local

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

echo [setup] Registering Shadow ...
"%EXE%" %HOST% %PORT% register Shadow 1234 Shadow george@test.local
if %ERRORLEVEL% NEQ 0 (
    echo [setup] Shadow registration failed ^(account may already exist^)
)

echo.
echo [setup] Registering Stephen ...
"%EXE%" %HOST% %PORT% register Stephen 12345 Stephen stephen@test.local
if %ERRORLEVEL% NEQ 0 (
    echo [setup] Stephen registration failed ^(account may already exist^)
)

echo.
echo [setup] Done. Run the other scripts to test friend and block commands.
endlocal
pause