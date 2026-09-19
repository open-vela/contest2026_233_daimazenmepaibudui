@echo off
rem ============================================================================
rem  usb_nosleep.cmd  --  stop Windows from power-saving the board's native USB
rem                       device (a very common cause of Code 10 / FAILED_START
rem                       that comes and goes).
rem
rem  RIGHT-CLICK -> "Run as administrator".
rem
rem  It clears the "Allow the computer to turn off this device to save power"
rem  checkbox for both VID_584E device instances by writing
rem        PnPCapabilities = 0x18
rem  into each device instance's "Device Parameters" key. 0x18 = 24 = disable
rem  power management for that device (D0 not powered off, no wake).
rem
rem  Effect is visible in Device Manager: the device's "Power Management" tab
rem  disappears / the checkbox is greyed out.
rem
rem  This does NOT touch drivers. To undo, delete the PnPCapabilities value or
rem  set it to 0 in the same key.
rem
rem  After running it: unplug USB -> power-cycle the board -> wait for the
rem  firmware -> plug USB in. (Never press RESET while the USB cable is
rem  plugged: a board soft-reset leaves the host side in FAILED_START.)
rem ============================================================================
setlocal
echo == current PnPCapabilities values ==
powershell -NoProfile -Command "Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '*VID_584E*' } | ForEach-Object { $p = $_.PSPath; Get-ChildItem $p -ErrorAction SilentlyContinue | ForEach-Object { $dp = Join-Path $_.PSPath 'Device Parameters'; $v = (Get-ItemProperty -Path $dp -Name PnPCapabilities -ErrorAction SilentlyContinue).PnPCapabilities; Write-Host ($_.PSChildName + '  PnPCapabilities=' + $v) } }"

echo.
echo == writing PnPCapabilities = 0x18 (24) for both instances ==
powershell -NoProfile -Command "Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '*VID_584E*' } | ForEach-Object { $p = $_.PSPath; Get-ChildItem $p -ErrorAction SilentlyContinue | ForEach-Object { $dp = Join-Path $_.PSPath 'Device Parameters'; if (-not (Test-Path $dp)) { New-Item -Path $dp -Force | Out-Null }; New-ItemProperty -Path $dp -Name PnPCapabilities -PropertyType DWord -Value 24 -Force | Out-Null; Write-Host ('set 24 on ' + $dp) } }"

echo.
echo == verify ==
powershell -NoProfile -Command "Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '*VID_584E*' } | ForEach-Object { $p = $_.PSPath; Get-ChildItem $p -ErrorAction SilentlyContinue | ForEach-Object { $dp = Join-Path $_.PSPath 'Device Parameters'; $v = (Get-ItemProperty -Path $dp -Name PnPCapabilities -ErrorAction SilentlyContinue).PnPCapabilities; Write-Host ($_.PSChildName + '  PnPCapabilities=' + $v) } }"

echo.
echo Now: unplug USB, power-cycle the board, wait for firmware, plug USB in.
pause
