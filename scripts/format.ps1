param([switch]$Check, [string[]]$Root = @('src', 'tests'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1"
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskFormatter = Get-Command clang-format -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $taskFormatter) { throw 'clang-format not found. Install mingw-w64-clang-x86_64-clang or add its bin directory to PATH.' }
Push-Location $taskRoot
try {
    # third_party is excluded by its own DisableFormat and must never be bulk-reformatted:
    # local edits there are tracked against upstream file hashes.
    $taskFiles = @()
    foreach ($taskDirectory in $Root) {
        $taskFiles += Get-ChildItem -Path $taskDirectory -Recurse -File -Include '*.cpp', '*.hpp', '*.h', '*.cc' |
            Where-Object { $_.FullName -notmatch '\\third_party\\|\\out\\|\\artifacts\\|\\\.tools\\' }
    }
    if ($taskFiles.Count -eq 0) { throw "No C++ sources found under: $($Root -join ', ')" }
    "clang-format ($($taskFormatter.Source)) over $($taskFiles.Count) files$(if ($Check) { ' -- check only' } else { '' })"
    $taskStyle = @('--style=file', '--fallback-style=none')
    if ($Check) {
        & $taskFormatter.Source @taskStyle --dry-run --Werror $taskFiles.FullName
        if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed. Run .\scripts\format.ps1 before verifying.' }
    } else {
        & $taskFormatter.Source @taskStyle -i $taskFiles.FullName
        if ($LASTEXITCODE -ne 0) { throw 'clang-format failed' }
    }
    'Formatting OK'
} finally { Pop-Location }
