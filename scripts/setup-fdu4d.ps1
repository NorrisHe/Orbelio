param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskEnvironment = Join-Path $taskRoot '.tools\fdu-python'
& $Python -m venv $taskEnvironment
if ($LASTEXITCODE -ne 0) { throw 'Could not create the FDU conversion Python environment' }
$taskPython = Join-Path $taskEnvironment 'Scripts\python.exe'
& $taskPython -m pip install 'torch>=2.6' 'numpy>=2.0' --index-url https://download.pytorch.org/whl/cpu
if ($LASTEXITCODE -ne 0) { throw 'Could not install CPU PyTorch and NumPy for FDU checkpoint conversion' }
