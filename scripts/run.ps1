param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [string]$Model,
    [switch]$Validation,
    [switch]$NoValidation,
    [ValidateRange(1,65536)][int]$InputBudgetMiB,
    [ValidateRange(1,65536)][int]$DecodeBudgetMiB,
    [ValidateRange(1,65536)][int]$CpuBudgetMiB,
    [ValidateRange(1,65536)][int]$GpuBudgetMiB,
    [ValidateRange(1,65536)][int]$SequenceCacheMiB,
    [ValidateRange(0,8000000)][int]$SplatBudget,
    [ValidateRange(0,8000000)][int]$UploadGaussiansPerFrame
)
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
$taskBudgetOptions = @{
    InputBudgetMiB = '--input-budget-mib'
    DecodeBudgetMiB = '--decode-budget-mib'
    CpuBudgetMiB = '--cpu-budget-mib'
    GpuBudgetMiB = '--gpu-budget-mib'
    SequenceCacheMiB = '--sequence-cache-mib'
    SplatBudget = '--splat-budget'
    UploadGaussiansPerFrame = '--upload-gaussians-per-frame'
}
foreach ($taskBudgetName in $taskBudgetOptions.Keys) {
    if ($PSBoundParameters.ContainsKey($taskBudgetName)) {
        $taskArguments += @($taskBudgetOptions[$taskBudgetName], [string]$PSBoundParameters[$taskBudgetName])
    }
}
& $taskExecutable @taskArguments
if ($LASTEXITCODE -ne 0) { throw "Viewer exited with code $LASTEXITCODE" }
