param([string]$MingwRoot)
$ErrorActionPreference = 'Stop'
if (-not $MingwRoot) {
    $taskCompiler = Get-Command g++.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $taskCompiler) { throw 'MinGW g++.exe not found in PATH. Add your MinGW bin directory to PATH.' }
    $MingwRoot = Split-Path -Parent (Split-Path -Parent $taskCompiler.Source)
}
if (-not (Test-Path -LiteralPath (Join-Path $MingwRoot 'bin\g++.exe'))) { throw "MinGW toolchain not found: $MingwRoot" }
$taskRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = "$MingwRoot\bin;$taskRoot\.tools\installed\x64-mingw-dynamic\bin;$taskRoot\.tools\installed\x64-mingw-dynamic\debug\bin;" + $env:PATH
$env:VCPKG_DISABLE_METRICS = '1'
$env:QT_PLUGIN_PATH = "$MingwRoot\share\qt6\plugins"
$env:QML_IMPORT_PATH = "$MingwRoot\share\qt6\qml"
$env:VK_LAYER_PATH = "$MingwRoot\bin"
$env:QSG_RENDER_LOOP = 'threaded'
$env:QT_FORCE_STDERR_LOGGING = '1'
