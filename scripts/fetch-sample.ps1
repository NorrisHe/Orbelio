param([ValidateSet('guitar','biker','hotel-culpture','skull')][string[]]$Name = @('guitar','biker','hotel-culpture','skull'))
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskCatalog = Get-Content -LiteralPath (Join-Path $taskRoot 'samples\catalog.json') -Raw | ConvertFrom-Json
foreach ($taskName in $Name) {
    $taskModel = $taskCatalog.models | Where-Object name -EQ $taskName
    $taskAsset = Join-Path $taskRoot "samples\$($taskModel.file)"
    if (Test-Path -LiteralPath $taskAsset) {
        if ((Get-FileHash -LiteralPath $taskAsset -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskModel.sha256) {
            throw "Existing $taskName model differs from the pinned sample; inspect it before replacing."
        }
        "$taskName already present; SHA-256 verified."
        continue
    }
    $taskTemporary = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString() + '.download')
    $taskUrl = "https://raw.githubusercontent.com/playcanvas/engine/$($taskCatalog.commit)/examples/assets/splats/$taskName.compressed.ply"
    try {
        curl.exe --fail --location --retry 2 --connect-timeout 15 --max-time 120 --silent --show-error $taskUrl --output $taskTemporary
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $taskName" }
        if ((Get-Item -LiteralPath $taskTemporary).Length -ne $taskModel.bytes -or
            (Get-FileHash -LiteralPath $taskTemporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskModel.sha256) {
            throw "Sample size or SHA-256 mismatch: $taskName"
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $taskAsset) -Force | Out-Null
        Move-Item -LiteralPath $taskTemporary -Destination $taskAsset
        "$taskName downloaded; SHA-256 verified."
    } finally {
        if (Test-Path -LiteralPath $taskTemporary) { Remove-Item -LiteralPath $taskTemporary }
    }
}
