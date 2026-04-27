@echo off
:: Fetch a score's attached game-data blob by account ID. This is the
:: AccountId-routed counterpart of the NpId-routed GetScoreData path and
:: corresponds to PS4 SDK's sceNpScoreGetGameDataByAccountId.
::
:: Prereq: a previous score-record-data call must have attached a blob to
:: (boardId, accountId, pcId). If there's no attached blob, the server
:: returns NotFound.
::
:: Usage:
::   score-get-data-accountid.bat <npid> <password> <comId> <boardId> <target_accountId> [pcId]
::
:: pcId defaults to 0 when not specified.
:: Pass 0 for <target_accountId> to look up your own blob.
::
:: Examples:
::   :: look up game-data blob for account 5
::   score-get-data-accountid.bat Shadow 1234 NPWR12345_00 1 5
::
::   :: look up your own game-data blob
::   score-get-data-accountid.bat Shadow 1234 NPWR12345_00 1 0

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~5"=="" (
    echo Usage: score-get-data-accountid.bat ^<npid^> ^<password^> ^<comId^> ^<boardId^> ^<target_accountId^> [pcId]
    echo   Hint: use 0 for ^<target_accountId^> to look up your own blob.
    exit /b 1
)

set PCID=0
if not "%~6"=="" set PCID=%~6

echo [score-get-data-accountid] fetching blob for accountId=%~5 (pcId=%PCID%) on board %~4
echo.

"%EXE%" %HOST% %PORT% score-get-data-accountid "%~1" "%~2" "%~3" "%~4" "%~5" "%PCID%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
