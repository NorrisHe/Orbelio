param([string]$Destination)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1"
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBuild = Join-Path $taskRoot 'out\build\windows-package'
$taskMingwBin = Split-Path -Parent (Get-Command g++.exe -CommandType Application).Source
$taskDependencyBin = Join-Path $taskRoot '.tools\installed\x64-mingw-dynamic\bin'
$taskQtTools = & (Join-Path $taskMingwBin 'qtpaths6.exe') --query QT_INSTALL_LIBEXECS
if ($LASTEXITCODE -ne 0) { throw 'Cannot locate Qt deployment tools' }
$env:PATH = "$($taskQtTools.Trim());" + $env:PATH
if (-not $Destination) {
    $Destination = Join-Path $taskRoot ('out\packages\Orbelio-windows-x64-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$Destination = [System.IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $Destination) { throw "Package destination already exists: $Destination" }

function Copy-RuntimeDependencies([string]$Directory) {
    $taskQueue = [System.Collections.Generic.Queue[string]]::new()
    Get-ChildItem -LiteralPath $Directory -Recurse -File |
        Where-Object Extension -In '.exe', '.dll' | ForEach-Object { $taskQueue.Enqueue($_.FullName) }
    $taskVisited = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    while ($taskQueue.Count) {
        $taskFile = $taskQueue.Dequeue()
        if (-not $taskVisited.Add($taskFile)) { continue }
        # objdump loses non-ASCII characters in absolute argv paths on Windows.
        Push-Location -LiteralPath (Split-Path -Parent $taskFile)
        try {
            $taskImports = & (Join-Path $taskMingwBin 'objdump.exe') -p ([System.IO.Path]::GetFileName($taskFile))
        } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect runtime dependencies: $taskFile" }
        foreach ($taskLine in $taskImports) {
            if ($taskLine -notmatch 'DLL Name:\s+(\S+)') { continue }
            $taskName = $Matches[1]
            if ($taskName -match '^(api|ext)-ms-') { continue }
            $taskTarget = Join-Path $Directory $taskName
            if (Test-Path -LiteralPath $taskTarget) { continue }
            $taskSource = @($taskDependencyBin, $taskMingwBin) |
                ForEach-Object { Join-Path $_ $taskName } | Where-Object { Test-Path -LiteralPath $_ } |
                Select-Object -First 1
            if ($taskSource) {
                Copy-Item -LiteralPath $taskSource -Destination $taskTarget
                $taskQueue.Enqueue($taskTarget)
            } elseif (-not (Test-Path -LiteralPath (Join-Path "$env:SystemRoot\System32" $taskName))) {
                throw "Missing runtime dependency $taskName imported by $taskFile"
            }
        }
    }
}

function Copy-Notices([string]$Directory) {
    $taskNotices = Join-Path $Directory 'licenses'
    New-Item -ItemType Directory -Path $taskNotices -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $taskRoot 'src\apps\viewer\assets\icons\LICENSE') -Destination (Join-Path $taskNotices 'icons-LICENSE')
    $taskShare = Join-Path $taskRoot '.tools\installed\x64-mingw-dynamic\share'
    Get-ChildItem -LiteralPath $taskShare -Directory | ForEach-Object {
        $taskCopyright = Join-Path $_.FullName 'copyright'
        if (Test-Path -LiteralPath $taskCopyright) {
            Copy-Item -LiteralPath $taskCopyright -Destination (Join-Path $taskNotices ($_.Name + '-copyright'))
        }
    }
    $taskMingwLicenses = Join-Path (Split-Path -Parent $taskMingwBin) 'share\licenses'
    foreach ($taskName in @('qt6-base', 'qt6-declarative', 'gcc-libs', 'vulkan-loader')) {
        Copy-Item -LiteralPath (Join-Path $taskMingwLicenses $taskName) -Destination (Join-Path $taskNotices $taskName) -Recurse
    }
}

Push-Location $taskRoot
try {
    cmake --preset windows-release -B $taskBuild -DVCPKG_MANIFEST_INSTALL=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Package configure failed' }
    cmake --build $taskBuild --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Package build failed' }
    ctest --test-dir $taskBuild --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Package tests failed' }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $taskExecutable = Join-Path $Destination 'orbelio_viewer.exe'
    Copy-Item -LiteralPath (Join-Path $taskBuild 'bin\orbelio_viewer.exe') -Destination $taskExecutable
    Copy-Item -LiteralPath (Join-Path $taskBuild 'bin\shaders') -Destination (Join-Path $Destination 'shaders') -Recurse
    & (Join-Path $taskMingwBin 'windeployqt.exe') --verbose 0 --release --compiler-runtime --no-translations --qmldir (Join-Path $taskRoot 'src\apps\viewer') --dir $Destination $taskExecutable
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
    @('[Paths]', 'Prefix=.', 'Plugins=.', 'QmlImports=qml', 'Libraries=.') |
        Set-Content -LiteralPath (Join-Path $Destination 'qt.conf') -Encoding utf8
    Copy-RuntimeDependencies $Destination
    Copy-Notices $Destination
    $taskRevision = git rev-parse HEAD
    $taskDirty = [bool](git status --porcelain --untracked-files=normal)
    @{
        schemaVersion = 1
        revision = $taskRevision.Trim()
        sourceModified = $taskDirty
        buildId = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
        builtAtUtc = [DateTime]::UtcNow.ToString('o')
        configuration = 'Release'
        architecture = 'x64'
        formats = @('PLY', 'Compressed PLY', 'SPLAT', 'KSPLAT', 'SPZ', 'SOG', 'PLY Sequence (.gsseq)', 'FDU4D intermediate (.fdu4d)')
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Destination 'version.json') -Encoding utf8
    @'
Orbelio Windows x64

Run orbelio_viewer.exe. A Vulkan 1.3 capable GPU and its vendor driver are required.
Open a model from the top-left menu, or use --model <path>. The guitar sample is embedded.
Use the model menu for Inspector, and the pulse button for performance and resource controls.
PLY Sequence manifests (.gsseq) provide playback, timestamps, single step and Seek.
Use --help for CPU/GPU/cache budgets. FDU checkpoints require conversion to .fdu4d before opening.
version.json identifies this build. licenses/ contains the bundled third-party notices.
'@ | Set-Content -LiteralPath (Join-Path $Destination 'Readme.txt') -Encoding utf8
    Write-Output $Destination
} finally { Pop-Location }
