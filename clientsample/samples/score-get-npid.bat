@echo off
:: Look up a specific player's score on a leaderboard by their NP ID.
::
:: Returns that player's rank entry (or an empty/zero entry if they have
:: no score on this board).
::
:: Usage:
::   score-get-npid.bat <npid> <password> <comId> <boardId> <target_npid> [pcId]
::
:: pcId defaults to 0 when not specified.
::
:: Examples:
::   score-get-npid.bat Shadow 1234 NPWR12345_00 1 Stephen
::   score-get-npid.bat Shadow 1234 NPWR12345_00 1 Stephen 0

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=..\rpcn-sample.exe

if "%~5"=="" (
    echo Usage: score-get-npid.bat ^<npid^> ^<password^> ^<comId^> ^<boardId^> ^<target_npid^> [pcId]
    exit /b 1
)

set PCID=0
if not "%~6"=="" set PCID=%~6

echo [score-get-npid] looking up %~5 (pcId=%PCID%) on board %~4
echo.

"%EXE%" %HOST% %PORT% score-get-npid "%~1" "%~2" "%~3" "%~4" "%~5" "%PCID%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
