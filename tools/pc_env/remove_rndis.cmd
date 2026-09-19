@echo off
rem ============================================================================
rem  remove_rndis.cmd  --  remove the stale "Remote NDIS Compatible Device"
rem                       instance (the one stuck in Code 10 / FAILED_START)
rem
rem  RIGHT-CLICK -> "Run as administrator" (pnputil needs elevation).
rem
rem  Why this is needed: the RNDIS function device is stuck in a failed PnP
rem  state. Windows keeps that instance state (and the network binding) even
rem  after the cable is unplugged, so every re-plug retries against the same
rem  broken state. Removing the instance makes the next enumeration create a
rem  BRAND NEW one.
rem
rem  Safe? Yes. "Remote NDIS Compatible Device" is a Windows in-box driver
rem  (usbrndis6) that ships with Windows, so it comes back automatically on
rem  the next plug-in -- no download needed. This only removes the device
rem  INSTANCE, not the driver package.
rem
rem  After it runs: power-cycle the board (not just reset), wait until the
rem  firmware is fully up, THEN plug the USB cable in.
rem ============================================================================
setlocal
set RNDIS_ID=USB\VID_584E&PID_5342&MI_00\7&810DA22&3&0000

echo == before ==
powershell -NoProfile -Command "Get-PnpDevice -InstanceId '*VID_584E*' | Select-Object FriendlyName,Status,Problem | Format-Table -AutoSize"

echo.
echo == removing device instance %RNDIS_ID% ==
pnputil /remove-device "%RNDIS_ID%"

echo.
echo == after (the VID_584E entries should be gone) ==
powershell -NoProfile -Command "Get-PnpDevice -InstanceId '*VID_584E*' | Select-Object FriendlyName,Status,Problem | Format-Table -AutoSize"

echo.
echo Now: power-cycle the board, wait for the firmware to come up, then plug USB.
echo Then tell the assistant to verify (both VID_584E OK + 192.168.137.1 + ping).
pause
