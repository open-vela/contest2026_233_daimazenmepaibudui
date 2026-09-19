@echo off
rem ============================================================================
rem  netnat_on.cmd  --  replace ICS with the built-in Windows NAT (WinNat) for
rem                     the board's USB-RNDIS subnet.
rem
rem  RIGHT-CLICK -> "Run as administrator".
rem
rem  Why: ICS kept half-failing today -- its address came back, but its DNS
rem  proxy never bound (nothing listens on 192.168.137.1:53) and even a direct
rem  query to 223.5.5.5 through it got no reply, i.e. its NAT/forwarding was
rem  broken too. WinNat does plain IP NAT and nothing else, which is exactly
rem  what the board needs: the board itself is already configured to ask
rem  223.5.5.5 for DNS (firmware change), so no DNS proxy is required.
rem
rem  What it does (idempotent):
rem    1) turns ICS sharing OFF on both connections (WLAN + RNDIS)
rem    2) gives the RNDIS adapter a static 192.168.137.1/24
rem       (the board is statically 192.168.137.2 with gateway 192.168.137.1)
rem    3) creates a WinNat: internal prefix 192.168.137.0/24, internet via the
rem       host's default route (WLAN)
rem    4) shows the result and pings the board
rem
rem  To undo: Disable ICS -> re-enable sharing in the adapter's Sharing tab,
rem           and run:  Remove-NetNat -Name ZhiAi -Confirm:$false
rem ============================================================================
setlocal
echo == 1) turn ICS sharing OFF (avoid two NATs fighting) ==
powershell -NoProfile -Command "$n = New-Object -ComObject HNetCfg.HNetShare; foreach ($c in @($n.EnumEveryConnection)) { $p = $n.NetConnectionProps.Invoke($c); if ($p.DeviceName -like '*NDIS*' -or $p.DeviceName -like '*Wi-Fi*' -or $p.DeviceName -like '*Wireless*' -or $p.Name -eq 'WLAN') { $cfg = $n.INetSharingConfigurationForINetConnection.Invoke($c); if ($cfg.SharingEnabled) { $cfg.DisableSharing(); Write-Host ('sharing off: ' + $p.Name) } else { Write-Host ('already off: ' + $p.Name) } } }"

echo.
echo == 2) static 192.168.137.1/24 on the RNDIS adapter ==
powershell -NoProfile -Command "$a = Get-NetAdapter | Where-Object { $_.InterfaceDescription -like '*NDIS*' }; if (-not $a) { Write-Host 'ERROR: RNDIS adapter not found (is the board plugged in?)'; exit 1 }; $idx = $a.ifIndex; Write-Host ('RNDIS ifIndex = ' + $idx); Remove-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -Confirm:$false -ErrorAction SilentlyContinue; New-NetIPAddress -InterfaceIndex $idx -IPAddress 192.168.137.1 -PrefixLength 24 -ErrorAction SilentlyContinue | Out-Null; Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 | Select-Object IPAddress,PrefixLength | Format-Table -AutoSize"

echo.
echo == 3) create the WinNat ==
powershell -NoProfile -Command "Remove-NetNat -Name ZhiAi -Confirm:$false -ErrorAction SilentlyContinue; New-NetNat -Name ZhiAi -InternalIPInterfaceAddressPrefix 192.168.137.0/24 | Out-Null; Start-Service WinNat -ErrorAction SilentlyContinue; Get-Service WinNat | Select-Object Name,Status | Format-Table -AutoSize; Get-NetNat | Select-Object Name,InternalIPInterfaceAddressPrefix,Active | Format-Table -AutoSize"

echo.
echo == 4) verify ==
powershell -NoProfile -Command "Start-Sleep -Seconds 3; Get-NetConnectionProfile | Select-Object InterfaceAlias,NetworkCategory | Format-Table -AutoSize"
ping -n 3 192.168.137.2

echo.
echo Expected: WinNat Running, ZhiAi Active, 192.168.137.1 present, ping replies.
echo Then tell the assistant: the board will be re-tested with net_test.
pause
