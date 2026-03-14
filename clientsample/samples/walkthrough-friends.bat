@echo off
:: Full friend flow walkthrough.
::
:: Demonstrates the complete request -> accept cycle between Shadow and Stephen.
:: Assumes setup-test-accounts.bat has already been run.
::
:: Steps performed:
::   1. Shadow logs in                    (shows empty friend lists)
::   2. Shadow sends Stephen a friend request (Stephen gets FriendQuery notification)
::   3. Shadow logs in again              (shows Stephen in requests_sent)
::   4. Stephen accepts Shadow's request      (both get FriendNew notification)
::   5. Shadow logs in again              (shows Stephen as a friend)
::   6. Stephen logs in                      (shows Shadow as a friend)

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

echo ============================================================
echo  Step 1: Shadow logs in (no friends yet)
echo ============================================================
"%EXE%" %HOST% %PORT% login Shadow 1234
echo.

echo ============================================================
echo  Step 2: Shadow sends Stephen a friend request
echo ============================================================
"%EXE%" %HOST% %PORT% friend-add Shadow 1234 Stephen
echo.

echo ============================================================
echo  Step 3: Shadow logs in again (Stephen should appear in requests_sent)
echo ============================================================
"%EXE%" %HOST% %PORT% login Shadow 1234
echo.

echo ============================================================
echo  Step 4: Stephen accepts Shadow's request (mutual friendship formed)
echo ============================================================
"%EXE%" %HOST% %PORT% friend-add Stephen 12345 Shadow
echo.

echo ============================================================
echo  Step 5: Shadow logs in (Stephen should now appear as a friend)
echo ============================================================
"%EXE%" %HOST% %PORT% login Shadow 1234
echo.

echo ============================================================
echo  Step 6: Stephen logs in (Shadow should appear as a friend)
echo ============================================================
"%EXE%" %HOST% %PORT% login Stephen 12345
echo.

echo ============================================================
echo  Walkthrough complete.
echo ============================================================
endlocal
pause
