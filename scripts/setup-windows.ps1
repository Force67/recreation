<#
.SYNOPSIS
  End-to-end dev setup for recreation on Windows (x64, MSVC).

.DESCRIPTION
  Acquires the dxc + glslang shader compilers, the freetype/harfbuzz libraries
  (via vcpkg), pkg-config and the build tools, fetches third-party deps and the
  sibling repos, then reports anything still missing. Safe to re-run.

  Run from an elevated "x64 Native Tools Command Prompt for VS 2022" (or any
  shell where MSVC is on PATH) so cl.exe is available.

.PARAMETER Check    Report only, change nothing.
.PARAMETER System   Install build tools + freetype/harfbuzz + pkg-config.
.PARAMETER Dxc      Download dxc + glslang.
.PARAMETER Deps     Fetch third-party deps + clone sibling repos.
  (no switch) does everything.
#>
[CmdletBinding()]
param(
  [switch]$Check,
  [switch]$System,
  [switch]$Dxc,
  [switch]$Deps,
  [switch]$Yes
)
$ErrorActionPreference = 'Stop'

$RepoDir = (Resolve-Path "$PSScriptRoot\..").Path
$DxcUrl     = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip'
$GlslangUrl = 'https://github.com/KhronosGroup/glslang/releases/download/main-tot/glslang-master-windows-Release.zip'
$ToolsDir   = Join-Path $RepoDir 'third_party\winbin'
$InCI       = [bool]$env:GITHUB_PATH

$script:Blockers = @()
function Log($m)  { Write-Host "==> $m" -ForegroundColor Blue }
function Ok($m)   { Write-Host "  ok   $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  warn $m" -ForegroundColor Yellow }
function Block($m){ Write-Host "  fail $m" -ForegroundColor Red; $script:Blockers += $m }
function Have($c) { return [bool](Get-Command $c -ErrorAction SilentlyContinue) }

# Put a directory on PATH: GITHUB_PATH under CI, otherwise just advise.
function Add-ToPath($dir) {
  if ($InCI) { Add-Content -Path $env:GITHUB_PATH -Value $dir }
  else { Warn "add to PATH: $dir" }
  $env:Path = "$dir;$env:Path"
}

function Install-Pkg($winget, $choco) {
  if (Have winget) { winget install --silent --accept-source-agreements --accept-package-agreements -e --id $winget }
  elseif (Have choco) { choco install $choco -y --no-progress }
  else { Block "no winget or choco - install $choco manually" }
}

function Do-System {
  if (-not (Have cmake)) { Log "installing cmake"; Install-Pkg 'Kitware.CMake' 'cmake' }
  # The Windows build uses the Visual Studio generator, so ninja is not required.
  if (-not (Have pkg-config)) { Log "installing pkg-config"; if (Have choco) { choco install pkgconfiglite -y --no-progress } else { Warn 'install pkg-config (choco install pkgconfiglite)' } }

  # freetype + harfbuzz via vcpkg (matches CI).
  $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
  if (-not $vcpkgRoot -and (Have vcpkg)) { $vcpkgRoot = Split-Path (Get-Command vcpkg).Source }
  if ($vcpkgRoot -and (Test-Path "$vcpkgRoot\vcpkg.exe")) {
    Log "installing freetype + harfbuzz via vcpkg"
    & "$vcpkgRoot\vcpkg.exe" install freetype:x64-windows harfbuzz:x64-windows
    $installed = Join-Path $vcpkgRoot 'installed\x64-windows'
    if ($InCI) { Add-Content -Path $env:GITHUB_ENV -Value "PKG_CONFIG_PATH=$installed\lib\pkgconfig" }
    Add-ToPath "$installed\bin"
    Ok "freetype + harfbuzz installed"
  } else {
    Block "vcpkg not found - install vcpkg and set VCPKG_INSTALLATION_ROOT, then: vcpkg install freetype:x64-windows harfbuzz:x64-windows"
  }
}

function Do-Dxc {
  New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
  if (-not (Have dxc)) {
    Log "downloading dxc"
    $z = Join-Path $env:TEMP 'dxc.zip'
    curl.exe -fsSL -o $z $DxcUrl
    Expand-Archive -Force $z "$ToolsDir\dxc"
    Add-ToPath "$ToolsDir\dxc\bin\x64"
    Ok "dxc installed"
  } else { Ok "dxc already on PATH" }
  if (-not (Have glslangValidator) -and -not (Have glslang)) {
    Log "downloading glslang"
    $z = Join-Path $env:TEMP 'glslang.zip'
    curl.exe -fsSL -o $z $GlslangUrl
    Expand-Archive -Force $z "$ToolsDir\glslang"
    Add-ToPath "$ToolsDir\glslang\bin"
    Ok "glslang installed"
  } else { Ok "glslang already on PATH" }
}

function Do-ThirdParty {
  Log "fetching third-party dependencies"
  # The get_*.sh helpers run under Git Bash, shipped with Git for Windows.
  bash "$RepoDir/tools/get_nanobuf.sh"
  bash "$RepoDir/tools/get_fidelityfx.sh"
  bash "$RepoDir/tools/get_nrd.sh"
  bash "$RepoDir/tools/get_jolt.sh"
}

function Clone-Sibling($name, $repo) {
  $dir = Join-Path $RepoDir "..\$name"
  if (Test-Path "$dir\CMakeLists.txt") { Ok "$name present ($dir)"; return }
  Log "cloning $name next to recreation"
  git clone --recurse-submodules $repo $dir
}

function Do-Siblings {
  Clone-Sibling 'zetanet' 'https://github.com/Force67/zetanet'
  Clone-Sibling 'libultragui' 'https://github.com/Force67/libultragui'
  git -C (Join-Path $RepoDir '..\zetanet') checkout develop 2>$null
  git -C (Join-Path $RepoDir '..\zetanet') submodule update --init --recursive 2>$null
}

function Do-Doctor {
  Log "checking the toolchain"
  if (Have cl) { Ok "MSVC (cl.exe)" } else { Block "MSVC not on PATH - install VS 2022 Build Tools (C++ workload) and run from the x64 Native Tools prompt" }
  if (Have cmake) { Ok "cmake" } else { Block "cmake missing - scripts\setup-windows.ps1 -System" }
  if (Have dxc) { Ok "dxc" } else { Block "dxc missing - scripts\setup-windows.ps1 -Dxc" }
  if ((Have glslangValidator) -or (Have glslang)) { Ok "glslang" } else { Block "glslang missing - scripts\setup-windows.ps1 -Dxc" }
  if (Have dotnet) { Ok "dotnet ($(dotnet --version))" } else { Warn "dotnet SDK 9 not found (only needed for C# scripting)" }
  if (Test-Path (Join-Path $RepoDir '..\zetanet\CMakeLists.txt')) { Ok "zetanet sibling" } else { Block "zetanet missing - scripts\setup-windows.ps1 -Deps" }
  if (Test-Path (Join-Path $RepoDir '..\libultragui\CMakeLists.txt')) { Ok "libultragui sibling" } else { Warn "libultragui missing - HUD/menus compile out (-Deps)" }
}

function Print-Report {
  Write-Host ''
  if ($script:Blockers.Count -eq 0) { Log "ready - configure with: cmake --preset windows"; return $true }
  Write-Host "  fail $($script:Blockers.Count) item(s) still block a build:" -ForegroundColor Red
  foreach ($b in $script:Blockers) { Write-Host "     - $b" -ForegroundColor Yellow }
  return $false
}

if ($Check) { Do-Doctor; if (Print-Report) { exit 0 } else { exit 1 } }

$any = $System -or $Dxc -or $Deps
$defaultAll = -not $any
if ($defaultAll) { $System = $Dxc = $Deps = $true }

if ($System) { Do-System }
if ($Dxc)    { Do-Dxc }
if ($Deps)   { Do-ThirdParty; Do-Siblings }
if ($defaultAll) { Do-Doctor; Print-Report | Out-Null }
