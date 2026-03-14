@echo off
:: Block flow walkthrough.
::
:: Demonstrates blocking a friend (severs the friendship) and then unblocking.
:: Assumes walkthrough-friends.bat has already been run so Shadow and Stephen
:: are mutual friends before this script starts.

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

echo ============================================================
echo  Step 1: Shadow logs in (Stephen should be a friend)
echo ============================================================
"%EXE%" %HOST% %PORT% login Shadow 1234
echo.

echo ============================================================
echo  Step 2: Shadow blocks Stephen (friendship severed, FriendLost sent to Stephen)
echo ============================================================
"%EXE%" %HOST% %PORT% block-add Shadow 1234 Stephen
echo.

echo ============================================================
echo  Step 3: Shadow logs in (Stephen should be in blocked list, not friends)
echo ============================================================
"%EXE%" %HOST% %PORT% login Shadow 1234
echo.

echo ============================================================
echo  Step 4: Stephen tries to add Shadow back (should fail with Blocked)
echo ============================================================
"%EXE%" %HOST% %PORT% friend-add Stephen 12345 Shadow
echo.

echo ============================================================
echo  Step 5: Shadow unblocks Stephen
echo ============================================================
"%EXE%" %HOST% %PORT% block-remove Shadow 1234 Stephen
echo.

echo ============================================================
echo  Step 6: Shadow logs in (Stephen should be gone from blocked list)
echo ============================================================
"%EXE%" %HOST% %PORT% login Shadow 1234
echo.

echo ============================================================
echo  Step 7: Shadow adds Stephen again (unblocking does not restore the friendship)
echo ============================================================
"%EXE%" %HOST% %PORT% friend-add Shadow 1234 Stephen
echo.
"%EXE%" %HOST% %PORT% friend-add Stephen 12345 Shadow
echo.

echo ============================================================
echo  Step 8: Shadow logs in (Stephen should be a friend again)
echo ============================================================
"%EXE%" %HOST% %PORT% login Shadow 1234
echo.

echo ============================================================
echo  Walkthrough complete.
echo ============================================================
endlocal
pause
