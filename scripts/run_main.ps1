<#
.SYNOPSIS
	Run HyperVR python\main.py with the full CPython that has hypervr installed.
#>
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

$candidates = @(
	"$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
	"$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
	"$env:LOCALAPPDATA\Programs\Python\Python310\python.exe"
)
$py = $null
foreach ($c in $candidates) {
	if (Test-Path $c) {
		& $c -c "import hypervr" 2>$null
		if ($LASTEXITCODE -eq 0) {
			$py = $c
			break
		}
	}
}
if (-not $py) {
	throw "No working hypervr install found. Run: .\scripts\install.ps1 -Python"
}

Write-Host "Using $py"
Set-Location $Root
& $py (Join-Path $Root "python\main.py") @args
