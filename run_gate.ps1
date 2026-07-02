# run_gate.ps1 - bwt.exe full gate (compress -> extract -> SHA-256 verify)
# ASCII only: PS 5.1 misreads BOM-less UTF-8 scripts as ANSI.
$ErrorActionPreference = 'Stop'
Set-Location 'D:\GitHub\Compression'

Remove-Item -Recurse -Force data_restored* -ErrorAction SilentlyContinue
Remove-Item -Force data.arc -ErrorAction SilentlyContinue

# stdin via ASCII files + cmd redirect (PS piping adds BOM and breaks mode select)
[IO.File]::WriteAllText("$PWD\gate_in1.txt", "1`r`ndata`r`n", [Text.Encoding]::ASCII)
[IO.File]::WriteAllText("$PWD\gate_in2.txt", "2`r`ndata.arc`r`n", [Text.Encoding]::ASCII)

# ===== compress: data -> data.arc =====
$sw = [System.Diagnostics.Stopwatch]::StartNew()
cmd /c ".\bwt.exe < gate_in1.txt" | Select-Object -Last 5
if (-not (Test-Path data.arc)) { Write-Host "[FAIL] data.arc not created"; exit 1 }
$arcSize = (Get-Item data.arc).Length
Write-Host ("COMPRESS_DONE data.arc = {0} B  [{1}s]" -f $arcSize, [math]::Round($sw.Elapsed.TotalSeconds))

# ===== extract: data.arc -> data_restored =====
$sw.Restart()
cmd /c ".\bwt.exe < gate_in2.txt" | Select-Object -Last 3
if (-not (Test-Path data_restored)) { Write-Host "[FAIL] data_restored not created"; exit 1 }
Write-Host ("EXTRACT_DONE  [{0}s]" -f [math]::Round($sw.Elapsed.TotalSeconds))

# ===== SHA-256 verify =====
$files = @('TeraPad.exe','explosion.wav','hal.bmp','wagahaiwa_nekodearu.txt','yuuki_256.bmp')
$okCount = 0
foreach ($f in $files) {
    $h1 = (Get-FileHash "data\$f" -Algorithm SHA256).Hash
    $h2 = (Get-FileHash "data_restored\$f" -Algorithm SHA256).Hash
    if ($h1 -eq $h2) { $okCount++; Write-Host "  SHA OK   $f" }
    else { Write-Host "  SHA FAIL $f" }
}
Write-Host ("SHA_RESULT {0}/5" -f $okCount)
if ($okCount -eq 5) { Write-Host ("GATE_RESULT PASS  score={0}" -f $arcSize) } else { Write-Host "GATE_RESULT FAIL"; exit 1 }
