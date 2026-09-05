#!/usr/bin/env pwsh
#
#   vw-install.Tests.ps1
#
#   Unit tests for the Windows INSTALLER (scripts/vw-install.ps1) — the script
#   shipped inside every release zip (and published as a release asset), and to
#   which the installed updater hands the actual file placement.
#
#   なぜここを厚くテストするか: **このスクリプトが「配置の手順」の唯一の持ち主**に
#   なったから。ここが取りこぼすと、利用者の Plug-Ins に半端なプラグインが残る
#   （M21 で本体 .vwpayload が増えたときに実際に起きた事故で、この仕組みはその再発を
#   止めるためにある）。したがって中心の検査は 1 つ:
#
#       **zip の直下にあるものが、列挙されていなくても全部入ること。**
#
#   The script is DOT-SOURCEd (its dispatch is guarded, see its tail) so the real
#   functions run in-process. Only the GitHub REST boundary needs faking; the
#   placement itself (Copy-Item / Rename-Item / the rename-aside swap) runs for
#   real against temp directories, which is exactly the part worth testing.
#
#   The PowerShell counterpart of tests/vw-install.test.sh — same harness, same
#   policy on missing tools. No Pester.
#

Set-StrictMode -Version Latest

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $Here '..' 'scripts' 'vw-install.ps1'

# Missing-dependency policy, matching the other script harnesses: skip locally,
# hard-fail in CI (VW_REQUIRE_SCRIPT_TESTS).
$RequireTools = -not ([string]::IsNullOrEmpty($env:VW_REQUIRE_SCRIPT_TESTS) -or
    ($env:VW_REQUIRE_SCRIPT_TESTS -in @('0', 'off', 'OFF', 'false', 'FALSE', 'no', 'NO')))

function Skip-Or-Fail([string] $Reason) {
    if ($RequireTools) {
        Write-Error "vw-install.Tests.ps1: $Reason (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)."
        exit 1
    }
    Write-Output "SKIP vw-install.Tests.ps1: $Reason."
    exit 0
}

if (-not (Test-Path -LiteralPath $Script)) {
    Skip-Or-Fail "$Script not found"
}

# Scratch folders. VW_PLUGINS_DIR is set BEFORE dot-sourcing so the script's
# top-level default (%APPDATA%, absent on a Linux CI runner) is never evaluated.
$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("vwinsttest-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$env:VW_PLUGINS_DIR = Join-Path $Work 'plugins'

. $Script
$ErrorActionPreference = 'Continue'

# ---------------------------------------------------------------------------
# Tiny assertion harness (same shape as tests/vw-update.Tests.ps1).
# ---------------------------------------------------------------------------
$script:TestsRun = 0
$script:TestsFailed = 0
$script:Current = '(none)'

function T([string] $name) { $script:Current = $name }

function Fail([string] $label, [string] $detail) {
    $script:TestsFailed++
    Write-Output ("FAIL [{0}] {1}" -f $script:Current, $label)
    if ($detail) { Write-Output $detail }
}

function CheckEq($actual, $expected, [string] $label = 'values differ') {
    $script:TestsRun++
    if ("$actual" -ne "$expected") {
        Fail $label ("  expected: {0}`n  actual:   {1}" -f $expected, $actual)
    }
}

function CheckPath([string] $path, [string] $label) {
    $script:TestsRun++
    if (-not (Test-Path -LiteralPath $path)) {
        Fail $label ("  missing path: {0}" -f $path)
    }
}

function CheckNoPath([string] $path, [string] $label) {
    $script:TestsRun++
    if (Test-Path -LiteralPath $path) {
        Fail $label ("  unexpected path: {0}" -f $path)
    }
}

# ---------------------------------------------------------------------------
# Fixtures. New-Tree builds a realistic unpacked release layout:
#   <name>.vlb          the shell (a DLL)
#   <name>.vwpayload    the payload
#   <name>.brand-new    **a file no caller enumerates** — the point of this suite
#   vw-install.ps1      the installer itself (must NOT be installed)
# ---------------------------------------------------------------------------
function New-Tree([string] $dir, [string] $name) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Set-Content -LiteralPath (Join-Path $dir "$name.vlb") -Value 'dll' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir "$name.vwpayload") -Value 'payload' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir "$name.brand-new") -Value 'future' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir 'vw-install.ps1') -Value '# installer' -NoNewline
}

$Name = 'HomeskzIfcImportDev'

# ===========================================================================
# Install-Tree — **the core promise: everything at the root goes in, listed or
# not; the installer itself does not.**
# ===========================================================================
T 'Install-Tree installs every entry at the root, including unlisted ones'
$src = Join-Path $Work 'tree-all'
New-Tree $src $Name
$script:PluginsDir = Join-Path $Work 'plugins-all'
CheckEq (Install-Tree $src $Name) $true 'Install-Tree succeeds'
CheckPath (Join-Path $script:PluginsDir "$Name.vlb") 'the shell landed'
CheckPath (Join-Path $script:PluginsDir "$Name.vwpayload") 'the payload landed'
CheckPath (Join-Path $script:PluginsDir "$Name.brand-new") 'a file nobody enumerated still landed'
CheckNoPath (Join-Path $script:PluginsDir 'vw-install.ps1') 'the installer itself is not installed'

T 'Install-Tree refuses an archive without the expected shell'
$src = Join-Path $Work 'tree-wrong'
New-Tree $src 'SomethingElse'
$script:PluginsDir = Join-Path $Work 'plugins-wrong'
CheckEq (Install-Tree $src $Name) $false 'Install-Tree fails on a mismatched archive'
CheckNoPath (Join-Path $script:PluginsDir 'SomethingElse.vwpayload') 'nothing is placed when the check fails'

T 'Install-Tree replaces what is already installed'
$src = Join-Path $Work 'tree-replace'
New-Tree $src $Name
$script:PluginsDir = Join-Path $Work 'plugins-replace'
New-Item -ItemType Directory -Force -Path $script:PluginsDir | Out-Null
Set-Content -LiteralPath (Join-Path $script:PluginsDir "$Name.vlb") -Value 'OLD' -NoNewline
CheckEq (Install-Tree $src $Name) $true 'Install-Tree succeeds over an existing install'
CheckEq (Get-Content -LiteralPath (Join-Path $script:PluginsDir "$Name.vlb") -Raw) 'dll' 'the shell was replaced'

# ===========================================================================
# Get-PluginName — the plug-in name is read from the archive, so -Name is
# optional for a manual install.
# ===========================================================================
T 'Get-PluginName reads the plug-in name off the shell module'
CheckEq (Get-PluginName (Join-Path $Work 'tree-all')) $Name 'name comes from <name>.vlb'

T 'Get-PluginName is empty when there is no shell in the directory'
$emptyDir = Join-Path $Work 'empty-dir'
New-Item -ItemType Directory -Force -Path $emptyDir | Out-Null
CheckEq (Get-PluginName $emptyDir) '' 'no module -> empty'

# ===========================================================================
# Get-InstalledShellId — the "nothing installed -> empty" branch.
# ===========================================================================
T 'Get-InstalledShellId is empty when nothing is installed'
$script:PluginsDir = Join-Path $Work 'nowhere'
CheckEq (Get-InstalledShellId $Name) '' 'absent stamp -> empty'

T 'Get-InstalledShellId reads the sidecar stamp'
$script:PluginsDir = Join-Path $Work 'plugins-stamp'
New-Item -ItemType Directory -Force -Path $script:PluginsDir | Out-Null
Set-Content -LiteralPath (Join-Path $script:PluginsDir "$Name.shell-id") -Value "abc123def456`n"
CheckEq (Get-InstalledShellId $Name) 'abc123def456' 'trimmed sidecar value'

# ===========================================================================
# Get-ReleaseZip — resolve the distribution zip out of a release. Exact name
# first, then the "*.vlb.zip" fallback that keeps an OLD installed updater
# working after the asset is renamed.
# ===========================================================================
$Release = @'
{
  "assets": [
    { "name": "notes.txt", "browser_download_url": "https://example.test/dl/notes.txt" },
    { "name": "HomeskzIfcImport.vlb.zip",
      "browser_download_url": "https://example.test/dl/HomeskzIfcImport.vlb.zip" }
  ]
}
'@ | ConvertFrom-Json

T 'Get-ReleaseZip finds the asset by exact plug-in name'
$hit = Get-ReleaseZip $Release 'HomeskzIfcImport'
CheckEq $hit.Url 'https://example.test/dl/HomeskzIfcImport.vlb.zip' 'exact match wins'
CheckEq $hit.Name 'HomeskzIfcImport' 'reports the plug-in name'

T 'Get-ReleaseZip falls back to any *.vlb.zip and reports its plug-in name'
$hit = Get-ReleaseZip $Release ''
CheckEq $hit.Url 'https://example.test/dl/HomeskzIfcImport.vlb.zip' 'suffix match yields the url'
CheckEq $hit.Name 'HomeskzIfcImport' 'name comes off the asset name'

T 'Get-ReleaseZip returns null when the release carries no distribution zip'
$noZip = '{ "assets": [ { "name": "notes.txt", "browser_download_url": "https://example.test/n" } ] }' | ConvertFrom-Json
CheckEq (Get-ReleaseZip $noZip '') $null 'no candidate -> null'

# ===========================================================================
# Read-Option — **unknown options must be ignored, not fatal.** A NEWER updater
# may hand an OLDER in-zip installer an option it has never heard of; refusing it
# would break exactly the update path this whole design exists to keep open.
# ===========================================================================
T 'Read-Option ignores unknown options and their values'
$script:Machine = $false
$opt = Read-Option @('-AFutureFlag', '-AFutureOption', 'value', '-Name', 'X', '-Machine')
CheckEq $opt.Name 'X' 'known options still bind after unknown ones'
CheckEq $script:Machine $true '-Machine was seen'

T 'Read-Option binds every documented option'
$script:Machine = $false
$opt = Read-Option @('-From', 'd', '-Zip', 'z', '-Url', 'u', '-Tag', 't')
CheckEq $opt.From 'd' '-From'
CheckEq $opt.Zip 'z' '-Zip'
CheckEq $opt.Url 'u' '-Url'
CheckEq $opt.Tag 't' '-Tag'

# ===========================================================================
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
Write-Output '---------------------------------------------------------------'
if ($script:TestsFailed -eq 0) {
    Write-Output ("PASS: all {0} checks passed." -f $script:TestsRun)
    exit 0
}
Write-Output ("FAIL: {0} of {1} checks failed." -f $script:TestsFailed, $script:TestsRun)
exit 1
