param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug', [string]$Model, [switch]$Validation, [switch]$NoValidation)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1"
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskConfig = $Configuration.ToLowerInvariant()
$taskExecutable = Join-Path $taskRoot "out\build\windows-$taskConfig\bin\orbelio_viewer.exe"
if (-not (Test-Path -LiteralPath $taskExecutable)) { throw 'Build first with scripts/build.ps1' }
$taskArguments = @()
if ($Validation -and $NoValidation) { throw 'Choose either -Validation or -NoValidation' }
# Release validation is opt-in; Debug enables it unless explicitly disabled.
if ($Validation -or ($Configuration -eq 'Debug' -and -not $NoValidation)) {
    $taskArguments += '--validation'
}
if ($Model) { $taskArguments += @('--model', $Model) }
& $taskExecutable @taskArguments
if ($LASTEXITCODE -ne 0) { throw "Viewer exited with code $LASTEXITCODE" }
