param()
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskAsset=Join-Path $taskRoot 'samples\guitar\guitar.compressed.ply'
$taskHash='04f61b72166394fdfb393dd589b0cb5a8b1b018820c73722278ddcdc2ffedc0f'
$taskUrl='https://raw.githubusercontent.com/playcanvas/engine/eddd61ea3faefec746907e4f4cf749aaa8a140d5/examples/assets/splats/guitar.compressed.ply'
if (Test-Path -LiteralPath $taskAsset) {
    if ((Get-FileHash -LiteralPath $taskAsset -Algorithm SHA256).Hash.ToLowerInvariant() -eq $taskHash) {
        Write-Output 'Guitar sample already present; SHA-256 verified.'
        exit 0
    }
    throw 'Existing model differs from the pinned sample; inspect it before replacing.'
}
$taskTemporary=$taskAsset+'.download'
try {
    Invoke-WebRequest -Uri $taskUrl -OutFile $taskTemporary
    if ((Get-FileHash -LiteralPath $taskTemporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskHash) { throw 'Sample SHA-256 mismatch' }
    Move-Item -LiteralPath $taskTemporary -Destination $taskAsset
} finally {
    if (Test-Path -LiteralPath $taskTemporary) { Remove-Item -LiteralPath $taskTemporary }
}
