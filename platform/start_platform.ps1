# Inicializa a Central de Instrumentacao 4-QAM (Backend FastAPI + Interface Web)
$ErrorActionPreference = "Stop"

$PyExe = "C:\Users\gerson.marques\.espressif\python_env\idf4.4_py3.8_env\Scripts\python.exe"
$BackendApp = Join-Path $PSScriptRoot "backend\app.py"

if (-not (Test-Path $PyExe)) {
    Write-Error "Python do ESP-IDF nao encontrado em $PyExe"
    exit 1
}

Write-Host "Iniciando Central de Instrumentacao 4-QAM em http://127.0.0.1:8420 ..." -ForegroundColor Cyan
Write-Host "Pressione Ctrl+C para encerrar. Feche outros monitores seriais das portas COM." -ForegroundColor Yellow

Start-Job -ScriptBlock {
    Start-Sleep -Seconds 2
    Start-Process "http://127.0.0.1:8420"
} | Out-Null

& $PyExe $BackendApp
