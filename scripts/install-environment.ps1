param(
    [string]$MsysRoot = 'C:\msys64',
    [ValidateSet('mingw64','ucrt64')][string]$Environment = 'mingw64'
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskPacman = Join-Path $MsysRoot 'usr\bin\pacman.exe'
if (-not (Test-Path -LiteralPath $taskPacman)) { throw "Install MSYS2 first from https://www.msys2.org, then rerun this script. Missing: $taskPacman" }
$taskPrefix = if ($Environment -eq 'ucrt64') { 'mingw-w64-ucrt-x86_64-' } else { 'mingw-w64-x86_64-' }
$taskPackages = @('gcc', 'clang', 'cmake', 'ninja', 'qt6-base', 'qt6-declarative', 'vulkan-headers', 'vulkan-loader', 'vulkan-validation-layers', 'shaderc', 'glslang', 'spirv-tools') | ForEach-Object { $taskPrefix + $_ }
# Git is provided by the user's native Git; MSYS2 has no matching mingw Git port.
& $taskPacman -S --needed --noconfirm @taskPackages
if ($LASTEXITCODE -ne 0) { throw 'MSYS2 dependency installation failed; check repository/system update state.' }
. "$PSScriptRoot\env.ps1" -MingwRoot (Join-Path $MsysRoot $Environment)
$taskVcpkg = Join-Path $taskRoot '.tools\vcpkg'
$taskBaseline = (Get-Content -LiteralPath (Join-Path $taskRoot 'vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
if (-not (Test-Path -LiteralPath $taskVcpkg)) {
    git clone --filter=blob:none --no-checkout https://github.com/microsoft/vcpkg.git $taskVcpkg
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg clone failed' }
    git -C $taskVcpkg checkout $taskBaseline
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg baseline checkout failed' }
}
$taskActual = git -C $taskVcpkg rev-parse HEAD
if ($taskActual.Trim() -ne $taskBaseline) { throw "Existing vcpkg checkout differs from the pinned baseline. Inspect $taskVcpkg before changing it." }
& (Join-Path $taskVcpkg 'bootstrap-vcpkg.bat') -disableMetrics
if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
Push-Location $taskRoot
try {
    & (Join-Path $taskVcpkg 'vcpkg.exe') install --triplet x64-mingw-dynamic --host-triplet x64-mingw-dynamic --x-install-root=.tools/installed
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg install failed' }
} finally { Pop-Location }
