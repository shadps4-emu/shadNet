@echo off
:: NPScore walkthrough — end-to-end demo using Shadow and Stephen.
::
:: Assumes setup-test-accounts.bat has already been run.

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe
set COMID=NPWR00000_00
set BOARD=1

echo ============================================================
echo  Step 1: Query board config (may show defaults if no scores yet)
echo ============================================================
"%EXE%" %HOST% %PORT% score-board Shadow 1234 %COMID% %BOARD%
echo.

echo ============================================================
echo  Step 2: Shadow records 50000
echo ============================================================
"%EXE%" %HOST% %PORT% score-record Shadow 1234 %COMID% %BOARD% 0 50000 "First run"
echo.

echo ============================================================
echo  Step 3: Stephen records 75000
echo ============================================================
"%EXE%" %HOST% %PORT% score-record Stephen 12345 %COMID% %BOARD% 0 75000 "Better run"
echo.

echo ============================================================
echo  Step 4: Shadow tries 40000 should fail with ScoreNotBest
echo ============================================================
"%EXE%" %HOST% %PORT% score-record Shadow 1234 %COMID% %BOARD% 0 40000 "Bad run"
echo.

echo ============================================================
echo  Step 5: Shadow records 90000 beats Stephen, should be rank 1
echo ============================================================
"%EXE%" %HOST% %PORT% score-record Shadow 1234 %COMID% %BOARD% 0 90000 "Personal best"
echo.

echo ============================================================
echo  Step 6: Fetch full leaderboard (top 10)
::   Expected: Shadow #1 90000, Stephen #2 75000
echo ============================================================
"%EXE%" %HOST% %PORT% score-range Shadow 1234 %COMID% %BOARD% 1 10
echo.

echo ============================================================
echo  Step 7: Look up Stephen's score by NP ID
echo ============================================================
"%EXE%" %HOST% %PORT% score-get-npid Shadow 1234 %COMID% %BOARD% Stephen
echo.

echo ============================================================
echo  Step 8: Look up Shadow's own score by NP ID
echo ============================================================
"%EXE%" %HOST% %PORT% score-get-npid Shadow 1234 %COMID% %BOARD% Shadow
echo.

echo ============================================================
echo  Walkthrough complete.
echo ============================================================
endlocal

pause
