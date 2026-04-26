@echo off
:: Look up a specific player's score on a leaderboard by their account ID
:: (the server-issued user_id that comes back in LoginReply). This is the
:: AccountId-routed counterpart of score-get-npid.bat and corresponds to
:: the PS4 SDK's sceNpScoreGetRankingByAccountId path.
::
:: The server resolves the account ID directly against its primary key,
:: with no npid-string lookup needed. Returns the same rank-entry shape
:: as score-get-npid (or an empty/zero entry if that account has no score
:: on this board).
::
:: Usage:
::   score-get-accountid.bat <npid> <password> <comId> <boardId> <target_accountId> [pcId]
::
:: pcId defaults to 0 when not specified.
:: Pass 0 for <target_accountId> to look up your own score.
::
:: Examples:
::   :: look up account 5 (whatever user_id 5 is on your server)
::   score-get-accountid.bat Shadow 1234 NPWR12345_00 1 5
::
::   :: look up yourself (resolved from LoginReply.user_id)
::   score-get-accountid.bat Shadow 1234 NPWR12345_00 1 0
::
::   :: same but with explicit pcId
::   score-get-accountid.bat Shadow 1234 NPWR12345_00 1 5 0

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~5"=="" (
    echo Usage: score-get-accountid.bat ^<npid^> ^<password^> ^<comId^> ^<boardId^> ^<target_accountId^> [pcId]
    echo   Hint: use 0 for ^<target_accountId^> to look up yourself.
    exit /b 1
)

set PCID=0
if not "%~6"=="" set PCID=%~6

echo [score-get-accountid] looking up accountId=%~5 (pcId=%PCID%) on board %~4
echo.

"%EXE%" %HOST% %PORT% score-get-accountid "%~1" "%~2" "%~3" "%~4" "%~5" "%PCID%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
