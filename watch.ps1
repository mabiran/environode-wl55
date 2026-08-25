# watch.ps1 - live sensor view over the ST-LINK console.
#
#   .\watch.ps1                     # COM4, one full reading every 5 s
#   .\watch.ps1 -IntervalSec 10     # slower cadence
#   .\watch.ps1 -Port COM7          # another board
#
# Ctrl+C stops it. Needs the node awake: the script sends `nucleo sleep off`
# on start; if the node was mid-sleep and missed it, press B1 (sleep off) or
# reset the board and restart the script.
#
# Each reading takes ~3 s when WS is selected (the wind burst window), so the
# interval floor is 4 s. Only one program can hold the COM port - close this
# before flashing or using another console.
param(
  [string]$Port = 'COM4',
  [int]$IntervalSec = 5,
  [switch]$NoSleepOff
)

$sp = New-Object System.IO.Ports.SerialPort($Port, 115200, 'None', 8, 'One')
$sp.ReadTimeout = 200
$sp.NewLine = "`r`n"
$sp.DtrEnable = $true
try { $sp.Open() } catch { Write-Host "FAILED to open $Port : $_"; exit 1 }

try {
  $sp.DiscardInBuffer()
  if (-not $NoSleepOff) {
    $sp.WriteLine('nucleo sleep off')
    Start-Sleep -Milliseconds 700
    $sp.DiscardInBuffer()
  }
  $period = [Math]::Max(4, $IntervalSec)
  Write-Host "watching $Port - one reading every $period s, Ctrl+C to stop"
  while ($true) {
    $sp.WriteLine('nucleo sensors')
    Write-Host ("`n=== {0} ===" -f (Get-Date -Format 'HH:mm:ss'))
    $deadline = (Get-Date).AddSeconds($period)
    while ((Get-Date) -lt $deadline) {
      try { $c = $sp.ReadExisting(); if ($c) { Write-Host -NoNewline $c } } catch {}
      Start-Sleep -Milliseconds 100
    }
  }
}
finally { $sp.Close() }
