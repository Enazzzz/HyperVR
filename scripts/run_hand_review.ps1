<#
.SYNOPSIS
	Run automated hand-only screenshot capture (desktop mode, no body/arms).
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
& $py (Join-Path $Root "python\hand_review.py") @args
$code = $LASTEXITCODE

$latestFile = Join-Path $Root "artifacts\hand_review\LATEST.txt"
if (Test-Path $latestFile) {
	$dir = (Get-Content $latestFile -TotalCount 1).Trim()
	if (Test-Path $dir) {
		Add-Type -AssemblyName System.Drawing
		Get-ChildItem $dir -Filter *.bmp | ForEach-Object {
			$img = [System.Drawing.Image]::FromFile($_.FullName)
			$png = [IO.Path]::ChangeExtension($_.FullName, ".png")
			$img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
			$img.Dispose()
		}
		Write-Host "PNG previews ready in $dir"
	}
}

exit $code
