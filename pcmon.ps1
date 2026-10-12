# Kirim CPU, RAM, GPU (%) ke ESP32 lewat serial, tiap 1 detik.
# Cara pakai:  powershell -ExecutionPolicy Bypass -File pcmon.ps1 -Port COM5
param([string]$Port = "COM5")

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
# Matikan DTR/RTS supaya ESP32-C3 tidak ter-reset saat port dibuka
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()

# GPU hanya terbaca kalau ada kartu NVIDIA (nvidia-smi); selain itu tampil "--"
$hasNv = Get-Command nvidia-smi -ErrorAction SilentlyContinue

Write-Host "Mengirim data ke $Port ... (Ctrl+C untuk berhenti)"
try {
  while ($true) {
    $cpu = [int](Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average

    $os  = Get-CimInstance Win32_OperatingSystem
    $ram = [int](100 * ($os.TotalVisibleMemorySize - $os.FreePhysicalMemory) / $os.TotalVisibleMemorySize)

    $gpu = -1
    if ($hasNv) {
      $out = (nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits | Select-Object -First 1)
      if ($out) { $gpu = [int]("$out".Trim()) }
    }

    $sp.WriteLine("$cpu,$ram,$gpu")
    Start-Sleep -Seconds 1
  }
}
finally {
  $sp.Close()
}
