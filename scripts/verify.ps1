param([string]$ValidationRoot = (Join-Path $PSScriptRoot '..\..\OrbelioValidation'))
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Path (Join-Path $taskRoot 'artifacts') -Force | Out-Null
$taskRunStamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff')
$taskLog = Join-Path $taskRoot "artifacts\gs-verification-$taskRunStamp.log"
Set-Content -LiteralPath (Join-Path $taskRoot 'artifacts\gs-verification-latest.txt') -Value $taskLog -Encoding utf8
Set-Content -LiteralPath $taskLog -Value 'Orbelio verification: Debug + Release' -Encoding utf8
try {
    & "$PSScriptRoot\format.ps1" -Check *>&1 | Tee-Object -FilePath $taskLog -Append
    foreach ($taskConfiguration in @('Debug','Release')) {
        "Configuration: $taskConfiguration" | Tee-Object -FilePath $taskLog -Append
        & "$PSScriptRoot\build.ps1" -Configuration $taskConfiguration -Test *>&1 | Tee-Object -FilePath $taskLog -Append
        & "$ValidationRoot\scripts\build.ps1" -Configuration $taskConfiguration -OrbelioRoot $taskRoot *>&1 | Tee-Object -FilePath $taskLog -Append
        & "$ValidationRoot\scripts\run.ps1" -Configuration $taskConfiguration -OrbelioRoot $taskRoot -Smoke *>&1 | Tee-Object -FilePath $taskLog -Append
        $taskValidationBuild = Join-Path $ValidationRoot "out\build\windows-$($taskConfiguration.ToLowerInvariant())"
        ctest --test-dir $taskValidationBuild -R '^(ui\.sequence_layout|integration\.(camera|camera_tools|models|formats|sequence|fdu4d)_vulkan|formats.conformance|correctness\.(metrics|reference_vulkan))$' --output-on-failure --no-tests=error *>&1 | Tee-Object -FilePath $taskLog -Append
        if ($LASTEXITCODE -ne 0) { throw 'Viewer integration tests failed' }
    }
} catch {
    Add-Content -LiteralPath $taskLog -Value ($_ | Out-String) -Encoding utf8
    throw
}
