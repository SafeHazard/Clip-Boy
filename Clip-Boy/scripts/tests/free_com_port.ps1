# free_com_port.ps1 -- kill any orphaned overnight-test python (pass process +
# its test_bridge session children) so a timeout-killed phase can't hold COM11
# and cascade-fail the next phase. Safe to run BETWEEN phases (no phase active).
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -match 'overnight_integration|test_bridge|tool_suite' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3
