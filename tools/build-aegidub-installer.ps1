#!/usr/bin/env powershell
# Build the aegidub Windows installer from a release build.
#
#   powershell tools\build-aegidub-installer.ps1 [-BuildRoot build-release]
#
# Needs Inno Setup 6 (ISCC.exe on PATH or in its default folder). Downloads
# the Visual C++ runtime the installer bundles, and the English spell
# checking dictionary, into <BuildRoot>\installer-deps the first time.

param (
  [string]$BuildRoot = "build-release"
)

$ErrorActionPreference = 'Stop'

$SourceRoot = Join-Path $PSScriptRoot '..' | Resolve-Path
if (![System.IO.Path]::IsPathRooted($BuildRoot)) {
  $BuildRoot = Join-Path $SourceRoot $BuildRoot
}
$BuildRoot = Resolve-Path $BuildRoot

if (!(Test-Path (Join-Path $BuildRoot 'aegidub.exe'))) {
  throw "No aegidub.exe in $BuildRoot; build it first (ninja -C $BuildRoot aegidub.exe)"
}

$DepsDir = Join-Path $BuildRoot 'installer-deps'
New-Item -ItemType Directory -Path $DepsDir -Force | Out-Null

# Visual C++ runtime, installed by the installer only where it is missing
$RedistDir = Join-Path $DepsDir 'VC_redist'
$RedistPath = Join-Path $RedistDir 'VC_redist.x64.exe'
if (!(Test-Path $RedistPath)) {
  New-Item -ItemType Directory -Path $RedistDir -Force | Out-Null
  Write-Host 'Downloading the Visual C++ runtime...'
  Invoke-WebRequest 'https://aka.ms/vs/17/release/VC_redist.x64.exe' -OutFile $RedistPath -UseBasicParsing
}

# English spell checking dictionary
$DictionariesDir = Join-Path $DepsDir 'dictionaries'
if (!(Test-Path $DictionariesDir)) {
  New-Item -ItemType Directory -Path $DictionariesDir | Out-Null
  $DictBase = 'https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts'
  foreach ($ext in 'aff', 'dic') {
    Invoke-WebRequest "$DictBase/en_US.$ext" -OutFile (Join-Path $DictionariesDir "en_US.$ext") -UseBasicParsing
  }
}

$Iscc = Get-Command iscc -ErrorAction SilentlyContinue
if ($Iscc) {
  $Iscc = $Iscc.Source
} else {
  $Iscc = Join-Path ${Env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
  if (!(Test-Path $Iscc)) { throw 'Inno Setup 6 is not installed (ISCC.exe not found)' }
}

$Env:BUILD_ROOT = $BuildRoot
$Env:SOURCE_ROOT = $SourceRoot
& $Iscc /Qp (Join-Path $SourceRoot 'packages\win_installer\aegidub.iss')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Get-ChildItem $BuildRoot -Filter 'aegidub-*-setup.exe' | Sort-Object LastWriteTime | Select-Object -Last 1 |
  ForEach-Object { Write-Host "Installer: $($_.FullName)" }
