<#
.SYNOPSIS
	Configure and build HyperVR (xr_demo). Optionally install the Python module.

.NOTES
	Always use a full CPython install under %LOCALAPPDATA%\Programs\Python
	(needs Development headers). The Windows Store ``python`` stub cannot load
	``hypervr.pyd`` — run the game with Python 3.11 from Programs.
#>
param(
	[switch]$Python,
	[string]$Config = "Release",
	[string]$PythonExe = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

if (-not $env:VULKAN_SDK) {
	$sdkRoot = "C:\VulkanSDK"
	if (Test-Path $sdkRoot) {
		$latest = Get-ChildItem $sdkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
		if ($latest) {
			$env:VULKAN_SDK = $latest.FullName
			Write-Host "Using VULKAN_SDK=$($env:VULKAN_SDK)"
		}
	}
}

if (-not $env:VULKAN_SDK) {
	throw "VULKAN_SDK is not set. Install the LunarG Vulkan SDK."
}

$env:Path = "$($env:VULKAN_SDK)\Bin;$env:Path"

Write-Host "Configuring CMake..."
cmake -S . -B build -DHYPERVR_BUILD_DEMO=ON -DHYPERVR_BUILD_PYTHON_BINDINGS=ON
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "Building xr_demo ($Config)..."
cmake --build build --config $Config --target xr_demo
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$demo = Join-Path $Root "build\bin\$Config\xr_demo.exe"
if (-not (Test-Path $demo)) {
	$demo = Join-Path $Root "build\bin\xr_demo.exe"
}
Write-Host "Built: $demo"

if ($Python) {
	$py = if ($PythonExe) { $PythonExe } else { $null }
	if (-not $py) {
		$candidates = @(
			"$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
			"$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
			"$env:LOCALAPPDATA\Programs\Python\Python310\python.exe"
		)
		foreach ($c in $candidates) {
			if (Test-Path $c) { $py = $c; break }
		}
		if (-not $py) { $py = (Get-Command python).Source }
	}

	# Store / WindowsApps stubs have no headers and cannot load our .pyd.
	if ($py -match "WindowsApps|PythonSoftwareFoundation") {
		throw @"
Refusing Store/WindowsApps Python: $py

Install/use a full CPython instead, then re-run:
  .\scripts\install.ps1 -Python -PythonExe `"$env:LOCALAPPDATA\Programs\Python\Python311\python.exe`"
  & `"$env:LOCALAPPDATA\Programs\Python\Python311\python.exe`" python\main.py
"@
	}

	Write-Host "Building hypervr_python for $py ..."
	cmake -S . -B build "-DPython3_EXECUTABLE=$py" -DHYPERVR_BUILD_DEMO=ON -DHYPERVR_BUILD_PYTHON_BINDINGS=ON
	if ($LASTEXITCODE -ne 0) { throw "cmake configure (python) failed" }

	# Confirm CMake actually found Development.Module for this interpreter.
	$cache = Join-Path $Root "build\CMakeCache.txt"
	$details = Select-String -Path $cache -Pattern "FIND_PACKAGE_MESSAGE_DETAILS_Python3:INTERNAL=(.+)$" | Select-Object -First 1
	if (-not $details -or ($details.Line -notmatch "Development\.Module")) {
		throw "Python Development.Module not found for $py — install the matching Python with headers (custom install / full installer)."
	}
	Write-Host "CMake found Python with Development.Module ($py)"

	# Force the extension to relink against this Python (avoids stale 3.11 .pyd on a 3.10 install).
	$moduleCpp = Join-Path $Root "bindings\python\src\module.cpp"
	(Get-Item $moduleCpp).LastWriteTime = Get-Date

	cmake --build build --config $Config --target hypervr_python
	if ($LASTEXITCODE -ne 0) { throw "python module build failed" }

	$pyd = Get-ChildItem -Path (Join-Path $Root "build") -Recurse -Filter "hypervr.pyd" |
		Where-Object { $_.FullName -notmatch "\\CMakeFiles\\" } |
		Sort-Object LastWriteTime -Descending |
		Select-Object -First 1
	if (-not $pyd) { throw "hypervr.pyd not found after build" }

	$site = & $py -c "import site; print(site.getusersitepackages())"
	if (-not $site) { throw "could not resolve user site-packages for $py" }
	New-Item -ItemType Directory -Force -Path $site | Out-Null
	$dest = Join-Path $site "hypervr.pyd"
	Copy-Item -Force $pyd.FullName $dest
	Write-Host "Installed $($pyd.FullName) -> $dest"

	# Also copy into the interpreter's global site-packages when writable.
	$sysSite = & $py -c "import sysconfig; print(sysconfig.get_path('purelib'))"
	if ($sysSite -and (Test-Path $sysSite) -and ($sysSite -ne $site)) {
		try {
			Copy-Item -Force $pyd.FullName (Join-Path $sysSite "hypervr.pyd")
			Write-Host "Also installed -> $sysSite\hypervr.pyd"
		} catch {
			Write-Host "Skipping system site-packages copy: $_"
		}
	}

	& $py -c "import hypervr, pathlib; p=pathlib.Path(hypervr.__file__); print('import ok', p); print('mtime', p.stat().st_mtime)"
	if ($LASTEXITCODE -ne 0) { throw "hypervr import failed after install" }

	Write-Host ""
	Write-Host "Run the game with THIS interpreter (not Store python):"
	Write-Host "  & `"$py`" `"$Root\python\main.py`""
}

Write-Host "Done. Put on a headset and run the demo when an OpenXR runtime is active."
