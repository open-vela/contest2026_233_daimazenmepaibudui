@echo off
rem ============================================================================
rem  ics_on.cmd  --  (re)bind Internet Connection Sharing: <internet adapter>
rem                  --> the board's USB-RNDIS adapter ("以太网 2").
rem
rem  RIGHT-CLICK -> "Run as administrator".
rem
rem  Why: ICS loses its binding whenever the RNDIS device instance is removed /
rem  re-created (e.g. after uninstalling it in Device Manager) or when the Wi-Fi
rem  network changes. Symptom: the board still gets link + its own
rem  192.168.137.2, but the PC no longer has 192.168.137.1, so ICS DNS and
rem  everything else from the board times out.
rem
rem  It first PRINTS every network connection (name / device / status) so you can
rem  see which pair it will use, then enables sharing:
rem      public  side = the connection whose device looks like Wi-Fi
rem      private side = the connection whose device is Remote NDIS
rem  Then it waits and prints the resulting 192.168.137.1 address.
rem
rem  Safe to re-run. To undo, turn sharing off in the adapter's Sharing tab.
rem ============================================================================
setlocal
echo == all network connections (name ^| device ^| status) ==
powershell -NoProfile -Command "$n = New-Object -ComObject HNetCfg.HNetShare; foreach ($c in @($n.EnumEveryConnection)) { $p = $n.NetConnectionProps.Invoke($c); Write-Host ($p.Name + ' | ' + $p.DeviceName + ' | ' + $p.Status) }"

echo.
echo == enabling sharing: Wi-Fi (public) -> Remote NDIS (private) ==
powershell -NoProfile -Command "$n = New-Object -ComObject HNetCfg.HNetShare; $pub = $null; $priv = $null; foreach ($c in @($n.EnumEveryConnection)) { $p = $n.NetConnectionProps.Invoke($c); if ($p.DeviceName -like '*NDIS*') { $priv = $c } elseif ($p.DeviceName -like '*Wi-Fi*' -or $p.DeviceName -like '*Wireless*' -or $p.Name -eq 'WLAN') { $pub = $c } }; if (-not $pub) { Write-Host 'ERROR: could not find the Wi-Fi connection'; exit 1 }; if (-not $priv) { Write-Host 'ERROR: could not find the RNDIS connection (is the board plugged in?)'; exit 1 }; $cp = $n.INetSharingConfigurationForINetConnection.Invoke($pub); $cq = $n.INetSharingConfigurationForINetConnection.Invoke($priv); if ($cp.SharingEnabled) { $cp.DisableSharing(); Start-Sleep -Seconds 1 }; if ($cq.SharingEnabled) { $cq.DisableSharing(); Start-Sleep -Seconds 1 }; $cp.EnableSharing(0); Start-Sleep -Seconds 1; $cq.EnableSharing(1); Write-Host 'sharing enabled (public + private)'"

echo.
echo == waiting for ICS to apply ==
powershell -NoProfile -Command "Start-Sleep -Seconds 6; Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.137.*' } | Select-Object IPAddress,InterfaceAlias | Format-Table -AutoSize"

echo.
echo == ICS DNS proxy check ==
nslookup broker.emqx.io 192.168.137.1

echo.
echo == ping the board ==
ping -n 3 192.168.137.2

echo.
echo Expected: 192.168.137.1 present, DNS resolves, ping replies.
echo If the address is still missing, unplug/replug the USB cable once and re-run.
pause
