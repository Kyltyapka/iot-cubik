$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BackendDir = Join-Path $Root "backend"
$NodeRedDir = Join-Path $Root "node-red"
$MosquittoConfig = Join-Path $Root "mosquitto.conf"
$EnvPath = Join-Path $BackendDir ".env"
$MosquittoExe = "mosquitto"
$PythonExe = "python"

if (Test-Path "C:\Program Files\mosquitto\mosquitto.exe") {
    $MosquittoExe = "C:\Program Files\mosquitto\mosquitto.exe"
}

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    if (Get-Command py -ErrorAction SilentlyContinue) {
        $PythonExe = "py"
    } else {
        Write-Host "Python was not found. Install Python 3.11+ or add it to PATH." -ForegroundColor Red
        exit 1
    }
}

if (-not (Test-Path $EnvPath)) {
    Write-Host "Missing backend\.env. Copy backend\.env.example to backend\.env and fill it first." -ForegroundColor Red
    exit 1
}

$apiKeyLine = Get-Content $EnvPath | Where-Object { $_ -match "^API_KEY=" } | Select-Object -First 1
if (-not $apiKeyLine) {
    Write-Host "API_KEY is missing in backend\.env." -ForegroundColor Red
    exit 1
}

$apiKey = $apiKeyLine.Split("=", 2)[1].Trim()

Start-Process powershell.exe -WorkingDirectory $BackendDir -ArgumentList @(
    "-NoExit",
    "-Command",
    "if (Test-Path .\.venv\Scripts\Activate.ps1) { . .\.venv\Scripts\Activate.ps1 }; $PythonExe app.py"
)

Start-Process powershell.exe -WorkingDirectory $Root -ArgumentList @(
    "-NoExit",
    "-Command",
    "& '$MosquittoExe' -c '$MosquittoConfig' -v"
)

Start-Process powershell.exe -WorkingDirectory $Root -ArgumentList @(
    "-NoExit",
    "-Command",
    "`$env:API_KEY='$apiKey'; if (Get-Command node-red.cmd -ErrorAction SilentlyContinue) { node-red.cmd -u '$NodeRedDir' '$NodeRedDir\flow.json' } else { node-red -u '$NodeRedDir' '$NodeRedDir\flow.json' }"
)

Write-Host "Started backend, Mosquitto, and Node-RED in separate PowerShell windows."
Write-Host "Dashboard: http://127.0.0.1:3000"
Write-Host "Node-RED:  http://127.0.0.1:1880"
