param([string]$ValidationRoot = (Join-Path $PSScriptRoot '..\..\OrbelioValidation'))
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Path (Join-Path $taskRoot 'artifacts') -Force | Out-Null
$taskRunStamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff')
$taskLog = Join-Path $taskRoot "artifacts\gs-verification-$taskRunStamp.log"
Set-Content -LiteralPath (Join-Path $taskRoot 'artifacts\gs-verification-latest.txt') -Value $taskLog -Encoding utf8
Set-Content -LiteralPath $taskLog -Value 'Orbelio verification: Debug product tests + Release regression suite' -Encoding utf8
try {
    & "$PSScriptRoot\format.ps1" -Check *>&1 | Tee-Object -FilePath $taskLog -Append
    'Configuration: Debug' | Tee-Object -FilePath $taskLog -Append
    & "$PSScriptRoot\build.ps1" -Configuration Debug -Test *>&1 | Tee-Object -FilePath $taskLog -Append

    'Configuration: Release' | Tee-Object -FilePath $taskLog -Append
    # The validation build also registers the product tests through add_subdirectory.
    & "$ValidationRoot\scripts\build.ps1" -Configuration Release -OrbelioRoot $taskRoot *>&1 | Tee-Object -FilePath $taskLog -Append
    & "$ValidationRoot\scripts\run.ps1" -Configuration Release -OrbelioRoot $taskRoot -Smoke *>&1 | Tee-Object -FilePath $taskLog -Append
    $taskValidationBuild = Join-Path $ValidationRoot 'out\build\windows-release'
    ctest --test-dir $taskValidationBuild --output-on-failure --no-tests=error *>&1 | Tee-Object -FilePath $taskLog -Append
    if ($LASTEXITCODE -ne 0) { throw 'Release regression tests failed' }
} catch {
    Add-Content -LiteralPath $taskLog -Value ($_ | Out-String) -Encoding utf8
    throw
}
