param([string]$ValidationRoot = (Join-Path $PSScriptRoot '..\..\OrbelioValidation'))
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Path (Join-Path $taskRoot 'artifacts') -Force | Out-Null
$taskLog = Join-Path $taskRoot 'artifacts\gs-verification.log'
Set-Content -LiteralPath $taskLog -Value 'Orbelio static GS verification: Debug + Release' -Encoding utf8
& "$PSScriptRoot\format.ps1" -Check *>&1 | Tee-Object -FilePath $taskLog -Append
foreach ($taskConfiguration in @('Debug','Release')) {
    "Configuration: $taskConfiguration" | Tee-Object -FilePath $taskLog -Append
    & "$PSScriptRoot\build.ps1" -Configuration $taskConfiguration -Test *>&1 | Tee-Object -FilePath $taskLog -Append
    & "$ValidationRoot\scripts\build.ps1" -Configuration $taskConfiguration -OrbelioRoot $taskRoot *>&1 | Tee-Object -FilePath $taskLog -Append
    & "$ValidationRoot\scripts\run.ps1" -Configuration $taskConfiguration -OrbelioRoot $taskRoot -Smoke *>&1 | Tee-Object -FilePath $taskLog -Append
}
