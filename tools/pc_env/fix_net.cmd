@echo off
rem ============================================================================
rem  fix_net.cmd  --  make the board's USB-RNDIS internet path work again.
rem                  Run this after switching Wi-Fi networks / moving location.
rem
rem  RIGHT-CLICK -> "Run as administrator".
rem
rem  What it does (all idempotent, safe to re-run):
rem    1) finds the RNDIS network interface (InterfaceDescription contains
rem       "Remote NDIS Compatible Device")
rem    2) sets its network category to Private -- otherwise Windows puts it in
rem       the "Unidentified / Public" profile and the firewall drops everything
rem       coming FROM the board (which breaks both ICS DNS and the mirror)
rem    3) adds an explicit inbound allow rule for that interface (belt &
rem       braces, works even if step 2 does not stick)
rem    4) restarts ICS (SharedAccess) so its NAT + DNS proxy rebind after a
rem       Wi-Fi change, then shows the 192.168.137.1 address
rem    5) checks the ICS DNS proxy by resolving a host through 192.168.137.1
rem    6) pings the board
rem
rem  Deliberately NOT used: Restart-NetAdapter (it wipes the ICS address).
rem ============================================================================
setlocal
echo == 1) locate the RNDIS interface ==
powershell -NoProfile -Command "$i = Get-NetAdapter | Where-Object { $_.InterfaceDescription -like '*NDIS*' }; if ($i) { $i | Select-Object ifIndex,Name,Status,InterfaceDescription | Format-Table -AutoSize; $i.ifIndex | Out-File -Encoding ascii %TEMP%\rndis_idx.txt } else { Write-Host 'no RNDIS adapter found'; '' | Out-File -Encoding ascii %TEMP%\rndis_idx.txt }"

echo.
echo == 2) set its network category to Private ==
powershell -NoProfile -Command "$idx = (Get-Content %TEMP%\rndis_idx.txt -ErrorAction SilentlyContinue | Select-Object -First 1); if ($idx) { try { Set-NetConnectionProfile -InterfaceIndex ([int]$idx) -NetworkCategory Private -ErrorAction Stop; Write-Host ('set to Private (ifIndex ' + $idx + ')') } catch { Write-Host ('could not set category: ' + $_.Exception.Message) } }"

echo.
echo == 3) explicit inbound allow rule for the RNDIS interface ==
echo    (two rules: one bound to the interface, one bound to the board's subnet --
echo     the subnet one survives the adapter being re-created on a replug, which
echo     resets the interface's network category back to Public)
powershell -NoProfile -Command "Get-NetFirewallRule -DisplayName 'ZhiAi RNDIS board inbound' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue; $idx = (Get-Content %TEMP%\rndis_idx.txt -ErrorAction SilentlyContinue | Select-Object -First 1); if ($idx) { New-NetFirewallRule -DisplayName 'ZhiAi RNDIS board inbound' -Direction Inbound -Action Allow -Profile Any -InterfaceAlias (Get-NetAdapter -InterfaceIndex ([int]$idx)).Name -ErrorAction SilentlyContinue | Out-Null; Write-Host ('interface rule ready (ifIndex ' + $idx + ')') } else { Write-Host 'no interface index, skipped interface rule' }; Get-NetFirewallRule -DisplayName 'ZhiAi board subnet inbound' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue; New-NetFirewallRule -DisplayName 'ZhiAi board subnet inbound' -Direction Inbound -Action Allow -Profile Any -RemoteAddress 192.168.137.0/24 -ErrorAction SilentlyContinue | Out-Null; Write-Host 'subnet rule ready (192.168.137.0/24, survives adapter re-creation)'"

echo.
echo == 3b) disable IPv6 on the RNDIS adapter ==
echo    (the board is IPv4-only: its RNDIS driver logs an ERROR line for every
echo     IPv6 packet Windows sends -- which floods the board's console at
echo     ~10-20 KB/s, burying nsh output and command echoes)
powershell -NoProfile -Command "$idx = (Get-Content %TEMP%\rndis_idx.txt -ErrorAction SilentlyContinue | Select-Object -First 1); if ($idx) { $n = (Get-NetAdapter -InterfaceIndex ([int]$idx)).Name; Disable-NetAdapterBinding -Name $n -ComponentID ms_tcpip6 -ErrorAction SilentlyContinue; $b = Get-NetAdapterBinding -Name $n -ComponentID ms_tcpip6 -ErrorAction SilentlyContinue; Write-Host ($n + '  ms_tcpip6 Enabled=' + $b.Enabled) } else { Write-Host 'no interface index, skipped' }"

echo.
echo == 4) restart ICS + show the ICS address ==
powershell -NoProfile -Command "Restart-Service SharedAccess -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 3; (Get-Service SharedAccess).Status; Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.137.*' } | Select-Object IPAddress,InterfaceAlias | Format-Table -AutoSize"

echo.
echo == 5) ICS DNS proxy check (must resolve) ==
nslookup broker.emqx.io 192.168.137.1

echo.
echo == 6) ping the board ==
ping -n 3 192.168.137.2

echo.
echo Done. Expected: SharedAccess Running, 192.168.137.1 present, DNS resolves,
echo ping replies. If DNS fails, run this again (ICS sometimes needs two tries
echo right after a Wi-Fi change).
pause
