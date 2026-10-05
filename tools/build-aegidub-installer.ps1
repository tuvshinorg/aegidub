#!/usr/bin/env powershell
# Build the aegidub Windows installer from a release build.
#
#   powershell tools\build-aegidub-installer.ps1 [-BuildRoot build-release]
#
# Needs Inno Setup 6 (ISCC.exe on PATH or in its default folder). Downloads
# the Visual C++ runtime the installer bundles, and the English spell
# checking dictionary, into <BuildRoot>\installer-deps the first time.
#
# Code signing: set SIGN_PFX to a .pfx certificate and SIGN_PFX_PASSWORD to
# its password, and aegidub.exe, the installer and its uninstaller are all
# signed with signtool from the Windows SDK and timestamped.

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

# Replace older runtimes than the one bundled
$RedistVersion = [version](Get-Item $RedistPath).VersionInfo.ProductVersion
$IsccArgs = @("/DVC_MAJOR=$($RedistVersion.Major)", "/DVC_MINOR=$($RedistVersion.Minor)")

# Optional code signing
if ($Env:SIGN_PFX) {
  if (!(Test-Path $Env:SIGN_PFX)) { throw "SIGN_PFX points to a missing file: $Env:SIGN_PFX" }
  $SignTool = Get-Command signtool -ErrorAction SilentlyContinue
  if ($SignTool) {
    $SignTool = $SignTool.Source
  } else {
    $SignTool = Get-ChildItem "${Env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
      Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
    if (!$SignTool) { throw 'signtool.exe not found; install the Windows SDK' }
  }
  $SignArgs = @('sign', '/fd', 'SHA256', '/tr', 'http://timestamp.digicert.com', '/td', 'SHA256',
                '/f', $Env:SIGN_PFX, '/p', $Env:SIGN_PFX_PASSWORD)

  Write-Host 'Signing aegidub.exe...'
  & $SignTool @SignArgs (Join-Path $BuildRoot 'aegidub.exe')
  if ($LASTEXITCODE -ne 0) { throw 'Signing aegidub.exe failed' }

  # Inno Setup signs the installer and the uninstaller it embeds; $f is the file
  $quoted = ($SignArgs | ForEach-Object { if ($_ -match '\s') { "`$q$_`$q" } else { $_ } }) -join ' '
  $IsccArgs += '/DSIGN'
  $IsccArgs += "/Ssigntool=`$q$SignTool`$q $quoted `$q`$f`$q"
} else {
  Write-Host 'No SIGN_PFX set: building an unsigned installer.'
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
& $Iscc /Qp @IsccArgs (Join-Path $SourceRoot 'packages\win_installer\aegidub.iss')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Get-ChildItem $BuildRoot -Filter 'aegidub-*-setup.exe' | Sort-Object LastWriteTime | Select-Object -Last 1 |
  ForEach-Object { Write-Host "Installer: $($_.FullName)" }
