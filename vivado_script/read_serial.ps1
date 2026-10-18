# Lit le port serie et enregistre dans logs\serie_*.txt   :  .\read_serial.ps1 -Port COM5
param([string]$Port = "COM5", [int]$Baud = 115200, [int]$Secondes = 300)
$log = "C:\Users\USER\Desktop\yolo_hw\logs\serie_$(Get-Date -Format yyyyMMdd_HHmmss).txt"
$sp = New-Object System.IO.Ports.SerialPort $Port,$Baud,"None",8,"One"
$sp.ReadTimeout = 500
$sp.Open()
Write-Host "Ecoute $Port -> $log  (Ctrl+C pour arreter)"
$fin = (Get-Date).AddSeconds($Secondes)
try {
  while ((Get-Date) -lt $fin) {
    try { $l = $sp.ReadLine(); Write-Host $l; Add-Content $log $l } catch [System.TimeoutException] {}
  }
} finally { $sp.Close() }
