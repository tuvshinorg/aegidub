#!/usr/bin/env powershell
# Configure a Windows release build of aegidub the same way locally and in CI.
#
#   powershell tools\ci-configure.ps1 build-release [-OfficialRelease]

param (
  [Parameter(Position = 0, Mandatory = $true)]
  [string]$BuildDir,
  [switch]$OfficialRelease
)

$ErrorActionPreference = 'Stop'
$SourceRoot = Join-Path $PSScriptRoot '..' | Resolve-Path

# LuaJIT's build runs sed, which Git for Windows ships
if (!(Get-Command sed -ErrorAction SilentlyContinue)) {
  $GitUsrBin = Join-Path $Env:ProgramFiles 'Git\usr\bin'
  if (Test-Path (Join-Path $GitUsrBin 'sed.exe')) { $Env:PATH = "$Env:PATH;$GitUsrBin" }
  else { throw 'sed was not found; install Git for Windows' }
}
Push-Location $SourceRoot
try {
  $options = @(
    '--buildtype=release'
    '-Ddefault_library=static'
    '--force-fallback-for=zlib,harfbuzz,freetype2,fribidi,libpng'
    '--wrap-mode=default'
    '-Dfallback_nasm=true'
    '-Dfreetype2:harfbuzz=disabled'
    '-Dharfbuzz:freetype=disabled'
    '-Dharfbuzz:cairo=disabled'
    '-Dharfbuzz:glib=disabled'
    '-Dharfbuzz:gobject=disabled'
    '-Dharfbuzz:tests=disabled'
    '-Dharfbuzz:docs=disabled'
    '-Dfribidi:tests=false'
    '-Dfribidi:docs=false'
    '-Dlibass:fontconfig=disabled'
  )
  if ($OfficialRelease) { $options += '-Dofficial_release=true' }

  if (Test-Path (Join-Path $BuildDir 'build.ninja')) {
    python -m mesonbuild.mesonmain setup --reconfigure $BuildDir @options
  } else {
    python -m mesonbuild.mesonmain setup $BuildDir @options
  }
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
  Pop-Location
}
