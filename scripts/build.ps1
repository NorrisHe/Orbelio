param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug', [switch]$Test)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1"
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskPreset = 'windows-' + $Configuration.ToLowerInvariant()
Push-Location $taskRoot
try {
    cmake --preset $taskPreset
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    cmake --build --preset $taskPreset
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    if ($Test) {
        ctest --preset $taskPreset
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    }
} finally { Pop-Location }
