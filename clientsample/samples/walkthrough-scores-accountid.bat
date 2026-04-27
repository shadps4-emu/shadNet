@echo off
:: AccountId-routed score walkthrough.
::
:: Demonstrates the AccountId path through the complete record→query→
:: get-game-data flow. Mirrors walkthrough-scores.bat but uses the
:: server-side user_id instead of the npid string for every lookup.
::
:: Why this matters: the AccountId path is what PS4 games use when they
:: track peers by the PSN account ID rather than the online ID. On this
:: server the account ID is the user_id column returned in LoginReply, so
:: once you've logged in once you can resolve both self and any known
:: peer by their numeric ID without round-tripping through an npid lookup.
::
:: Assumes setup-test-accounts.bat has already been run.
:: Prereq: Shadow (user_id 1) and Stephen (user_id 2) exist. If your
:: assignments differ, edit SHADOW_ID and STEPHEN_ID below.

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe
set COMID=NPWR00000_00
set BOARD=1

:: Adjust these to match the user_id values on your server. The first
:: account created via setup-test-accounts.bat is user_id 1, the second
:: is 2, etc.
set SHADOW_ID=1
set STEPHEN_ID=2

echo ============================================================
echo  Step 1: Shadow records 50000 with a game-data blob
echo ============================================================
"%EXE%" %HOST% %PORT% score-record Shadow 1234 %COMID% %BOARD% 0 50000 "AccountId demo"
echo.

echo ============================================================
echo  Step 2: Stephen records 75000
echo ============================================================
"%EXE%" %HOST% %PORT% score-record Stephen 12345 %COMID% %BOARD% 0 75000 "Pushing Shadow"
echo.

echo ============================================================
echo  Step 3: Look up Stephen's score by account ID (%STEPHEN_ID%)
::   Expected: Stephen's entry with rank=1, score=75000
echo ============================================================
"%EXE%" %HOST% %PORT% score-get-accountid Shadow 1234 %COMID% %BOARD% %STEPHEN_ID%
echo.

echo ============================================================
echo  Step 4: Shadow looks up his own score by account ID (pass 0 = self)
::   The sample resolves 0 to our LoginReply.user_id automatically.
echo ============================================================
"%EXE%" %HOST% %PORT% score-get-accountid Shadow 1234 %COMID% %BOARD% 0
echo.

echo ============================================================
echo  Step 5: Look up a non-existent account (99)
::   Expected: empty rank entry, the server returns an empty-npid sentinel
echo ============================================================
"%EXE%" %HOST% %PORT% score-get-accountid Shadow 1234 %COMID% %BOARD% 99
echo.

echo ============================================================
echo  Step 6: Compare and fetch the same board via range to confirm
::   leaderboard contents now include accountId in each entry.
echo ============================================================
"%EXE%" %HOST% %PORT% score-range Shadow 1234 %COMID% %BOARD% 1 10
echo.

echo ============================================================
echo  Walkthrough complete.
echo ============================================================
endlocal

pause
