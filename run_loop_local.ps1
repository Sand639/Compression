# run_loop_local.ps1
# ローカルで Claude Code を自動再開ループさせる（Windows PowerShell用）。
# 5時間制限で落ちたら一定時間待ってから --continue で再開し、最大30セッション回す。
#
# 使い方
#   1) このファイルと compress_prompt_local.md を、main.cpp と data があるフォルダに置く
#   2) PowerShell で  powershell -ExecutionPolicy Bypass -File .run_loop_local.ps1
#
# 注意 フラグ名は版で変わる。実行前に `claude --help` で
#       --continue  --dangerously-skip-permissions の有無を確認すること。
#       完全無人にしたくない場合は --dangerously-skip-permissions を外して、
#       対話で承認しながら回す（その場合この自動ループは使わず、Claude Code に直接プロンプトを貼る）。

Set-Location -Path $PSScriptRoot

$prompt   = Get-Content -Raw .compress_prompt_local.md
$maxRuns  = 30
$waitSec  = 18000   # 5時間。制限リセットを確実に待つなら少し長めでもよい

for ($i = 1; $i -le $maxRuns; $i++) {
    Write-Host === session $i  $maxRuns  $(Get-Date) === -ForegroundColor Cyan

    # STOP ファイルがあれば手動停止
    if (Test-Path .STOP) {
        Write-Host STOP file found. exiting. -ForegroundColor Yellow
        break
    }

    if ($i -eq 1) {
        claude -p $prompt --dangerously-skip-permissions
    } else {
        claude -p 続きから。PROGRESS_COMPRESS.md と LEDGER.md を読んで次の改善イテレーションを進めて。 `
               --continue --dangerously-skip-permissions
    }
    Write-Host exit code $LASTEXITCODE

    if ($i -lt $maxRuns) {
        Write-Host waiting $($waitSec)s before next session... -ForegroundColor DarkGray
        Start-Sleep -Seconds $waitSec
    }
}
Write-Host done. -ForegroundColor Green