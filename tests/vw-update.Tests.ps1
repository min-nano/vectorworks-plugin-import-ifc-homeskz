#!/usr/bin/env pwsh
#
#   vw-update.Tests.ps1
#
#   Unit tests for the Windows updater back end (scripts/vw-update.ps1). The
#   PowerShell counterpart of tests/vw-update.test.sh: it DOT-SOURCEs the script
#   (its dispatch is guarded, see the tail of vw-update.ps1) so the real
#   functions run in-process, and overrides just their outermost I/O leaves.
#
#   Only two seams need faking — everything else runs for real on Linux pwsh:
#
#     * Invoke-GH          the GitHub REST boundary. Replaced with a stub that
#                          returns objects parsed from fixture JSON (exactly what
#                          the real Invoke-RestMethod would hand back), or throws
#                          to simulate an unreachable API.
#     * Invoke-WebRequest  the asset download. Replaced with a stub that copies a
#                          local fixture .zip to -OutFile (or throws to simulate a
#                          failed download).
#
#   Get-InstalledCommit (reads a plain "<name>.commit" sidecar — no OS tool),
#   Expand-Archive, Copy-Item and the atomic-swap logic all run REAL against
#   temp files, so this covers more of the script than a pure-parse test would.
#
#   Like the C++ IUpdaterHost fake and the bash harness, this is a unit test:
#   the script is the unit, the stubs are the test doubles. It is NOT end-to-end
#   (that would run the real script on Windows against the live GitHub API). Uses
#   no Pester — a tiny in-file harness keeps it dependency-free, matching
#   tests/TestFramework.h and tests/vw-update.test.sh.
#

Set-StrictMode -Version Latest

$Here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $Here '..' 'scripts' 'vw-update.ps1'

# Missing-dependency policy, matching vw-update.test.sh: a developer box skips
# gracefully (exit 0), but in CI a silent skip would let the suite "pass"
# without running a single check. When VW_REQUIRE_SCRIPT_TESTS is set (the Tests
# workflow sets it), a missing prerequisite is a HARD FAILURE instead. Common
# falsy spellings count as unset so VW_REQUIRE_SCRIPT_TESTS=0 still means
# "skip is OK".
$RequireTools = -not ([string]::IsNullOrEmpty($env:VW_REQUIRE_SCRIPT_TESTS) -or
    ($env:VW_REQUIRE_SCRIPT_TESTS -in @('0', 'off', 'OFF', 'false', 'FALSE', 'no', 'NO')))

function Skip-Or-Fail([string] $Reason) {
    if ($RequireTools) {
        Write-Error "vw-update.Tests.ps1: $Reason (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)."
        exit 1
    }
    Write-Output "SKIP vw-update.Tests.ps1: $Reason."
    exit 0
}

if (-not (Test-Path -LiteralPath $Script)) {
    Skip-Or-Fail "$Script not found"
}

# Scratch plug-ins folder. Set VW_PLUGINS_DIR BEFORE dot-sourcing so the script's
# top-level default (which uses %APPDATA%, absent on a Linux CI runner) is never
# evaluated — the script reads this env var into its $VW_PLUGINS_DIR.
$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("vwtest-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$PluginsDir = Join-Path $Work 'plugins'
New-Item -ItemType Directory -Force -Path $PluginsDir | Out-Null
$env:VW_PLUGINS_DIR = $PluginsDir

# ---------------------------------------------------------------------------
# Load the script under test. Dot-sourcing brings its functions and top-level
# variables into THIS scope (so overriding a function is seen by the callers).
# The guarded dispatch does not run under dot-source. Relax the script's
# ErrorActionPreference='Stop' so an expected failure inside a stub cannot abort
# the harness; each function under test keeps its own try/catch.
# ---------------------------------------------------------------------------
. $Script
$ErrorActionPreference = 'Continue'

# ---------------------------------------------------------------------------
# Tiny assertion harness, styled after tests/TestFramework.h / vw-update.test.sh.
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

function CheckContains([string] $haystack, [string] $needle, [string] $label = 'substring not found') {
    $script:TestsRun++
    if (-not $haystack.Contains($needle)) {
        Fail $label ("  missing:  {0}`n  in:       {1}" -f $needle, $haystack)
    }
}

function CheckNotContains([string] $haystack, [string] $needle, [string] $label = 'substring present') {
    $script:TestsRun++
    if ($haystack.Contains($needle)) {
        Fail $label ("  unexpected: {0}`n  in:         {1}" -f $needle, $haystack)
    }
}

# Join a function's Write-Output lines into one string for substring checks.
function AsText($lines) { return (@($lines) -join "`n") }

# ---------------------------------------------------------------------------
# Stubs. Control which fixture / failure each returns via $script:Fake* vars set
# per test. Overriding these names here shadows the script's Invoke-GH and the
# real Invoke-WebRequest cmdlet within this shared (dot-sourced) scope.
# ---------------------------------------------------------------------------
$script:FakeApiFail = $false
$script:FakeStableJson = $null
$script:FakeReleasesJson = $null
$script:FakeDownloadZip = $null
$script:FakeDownloadFail = $false

function Invoke-GH([string] $subpath) {
    if ($script:FakeApiFail) { throw 'offline' }
    if ($subpath -eq 'releases/tags/stable') { return ($script:FakeStableJson | ConvertFrom-Json) }
    if ($subpath -like 'releases*') { return ($script:FakeReleasesJson | ConvertFrom-Json) }
    throw "unexpected subpath: $subpath"
}

function Invoke-WebRequest {
    param(
        [string] $Uri,
        [string] $OutFile,
        [switch] $UseBasicParsing,
        $TimeoutSec,
        [Parameter(ValueFromRemainingArguments = $true)] $Rest
    )
    if ($script:FakeDownloadFail) { throw 'download failed' }
    Copy-Item -LiteralPath $script:FakeDownloadZip -Destination $OutFile -Force
}

# ---------------------------------------------------------------------------
# Fixtures. The JSON the stubbed API returns, and real .zip archives for the
# do-install path. ($Work / $PluginsDir were created above, before dot-source.)
# ---------------------------------------------------------------------------
$script:FakeStableJson = @'
{
  "target_commitish": "abc1234def5678",
  "assets": [
    { "name": "SamplePlugin.vlb.zip",
      "browser_download_url": "https://example.test/dl/SamplePlugin.vlb.zip" },
    { "name": "notes.txt",
      "browser_download_url": "https://example.test/dl/notes.txt" }
  ]
}
'@

$script:FakeReleasesJson = @'
[
  { "tag_name": "stable", "name": "stable", "target_commitish": "zzz9999",
    "assets": [ { "name": "SamplePlugin.vlb.zip",
                  "browser_download_url": "https://example.test/dl/stable.zip" } ] },
  { "tag_name": "dev-feature-x", "name": "feature/x", "target_commitish": "aaa1111ccc",
    "assets": [ { "name": "SamplePluginDev.vlb.zip",
                  "browser_download_url": "https://example.test/dl/x.zip" } ] },
  { "tag_name": "dev-feature-y", "name": "feature/y", "target_commitish": "bbb2222ddd",
    "assets": [ { "name": "SamplePluginDev.vlb.zip",
                  "browser_download_url": "https://example.test/dl/y.zip" } ] },
  { "tag_name": "dev-nobuild", "name": "feature/z", "target_commitish": "ccc3333eee",
    "assets": [ { "name": "unrelated.zip",
                  "browser_download_url": "https://example.test/dl/z.zip" } ] }
]
'@

# Build a real "<bundle>.vlb" zip for the do-install tests: a staging dir holding
# the flat files a real release ships, compressed at the archive root.
function New-BuildZip([string] $zipPath, [string] $vlbName) {
    $stage = Join-Path $Work ("stage-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Set-Content -LiteralPath (Join-Path $stage "$vlbName.vlb") -Value 'dll' -NoNewline
    Set-Content -LiteralPath (Join-Path $stage "$vlbName.commit") -Value 'newcommit' -NoNewline
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -Force
    Remove-Item -LiteralPath $stage -Recurse -Force
}
$GoodZip = Join-Path $Work 'good.zip'
$BadZip  = Join-Path $Work 'bad.zip'
New-BuildZip $GoodZip 'SamplePluginDev'
New-BuildZip $BadZip  'WrongName'

# ===========================================================================
# Get-AssetUrl / Get-Short — the pure helpers.
# ===========================================================================
T 'Get-AssetUrl finds the matching asset'
$rel = $script:FakeStableJson | ConvertFrom-Json
CheckEq (Get-AssetUrl $rel 'SamplePlugin.vlb.zip') 'https://example.test/dl/SamplePlugin.vlb.zip' 'returns the URL'

T 'Get-AssetUrl returns null for an unknown asset'
CheckEq (Get-AssetUrl $rel 'does-not-exist.zip') $null 'null when no asset matches'

T 'Get-Short takes the first 7 chars'
CheckEq (Get-Short 'abc1234def5678') 'abc1234' '7-char prefix'
CheckEq (Get-Short '') '' 'empty stays empty'

# ===========================================================================
# Get-InstalledCommit — reads the real "<name>.commit" sidecar (no OS tool).
# ===========================================================================
T 'Get-InstalledCommit reads the sidecar commit'
Set-Content -LiteralPath (Join-Path $VW_PLUGINS_DIR 'SamplePlugin.commit') -Value "abc1234`n"
CheckEq (Get-InstalledCommit 'SamplePlugin') 'abc1234' 'trimmed sidecar value'

T 'Get-InstalledCommit is none when the sidecar is absent'
CheckEq (Get-InstalledCommit 'SamplePluginDev') 'none' 'absent sidecar -> none'

# ===========================================================================
# q-stable — installed / latest / url, and the offline / incomplete paths.
# ===========================================================================
T 'Invoke-QStable reports installed, 7-char latest and the asset url'
$script:FakeApiFail = $false
$out = AsText (Invoke-QStable)
CheckContains $out 'installed=abc1234' 'installed line (from sidecar)'
CheckContains $out 'latest=abc1234' 'latest is the 7-char commit prefix'
CheckContains $out 'url=https://example.test/dl/SamplePlugin.vlb.zip' 'url line'

T 'Invoke-QStable emits an error line when the API is unreachable'
$script:FakeApiFail = $true
$out = AsText (Invoke-QStable)
CheckContains $out 'error=' 'offline -> error= line'
CheckNotContains $out 'latest=' 'no latest when offline'
$script:FakeApiFail = $false

# ===========================================================================
# q-dev — installed line + one TSV row per dev-* build that has a downloadable
# asset (the stable release and the asset-less dev build are both skipped).
# ===========================================================================
T 'Invoke-QDev lists only dev-* builds that have a downloadable asset'
$out = AsText (Invoke-QDev)
CheckContains $out ("build`taaa1111`tfeature/x`thttps://example.test/dl/x.zip") 'feature/x row'
CheckContains $out ("build`tbbb2222`tfeature/y`thttps://example.test/dl/y.zip") 'feature/y row'
CheckNotContains $out 'feature/z' 'asset-less dev build is skipped'
CheckNotContains $out ("build`tzzz9999") 'the stable (non dev-*) release is skipped'

T 'Invoke-QDev emits an error line when the API is unreachable'
$script:FakeApiFail = $true
$out = AsText (Invoke-QDev)
CheckContains $out 'error=' 'offline -> error= line'
$script:FakeApiFail = $false

# ===========================================================================
# do-install — download + Expand-Archive + atomic swap into VW_PLUGINS_DIR, and
# its error paths. Uses the REAL Expand-Archive / Copy-Item; only the download is
# stubbed.
# ===========================================================================
T 'Invoke-DoInstall installs the .vlb and prints ok'
$script:FakeDownloadFail = $false
$script:FakeDownloadZip = $GoodZip
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'SamplePluginDev')
CheckEq $out 'ok' 'prints ok'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'SamplePluginDev.vlb')) $true 'the .vlb landed'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'SamplePluginDev.commit')) $true 'the .commit sidecar landed'

T 'Invoke-DoInstall reports a download failure'
$script:FakeDownloadFail = $true
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'SamplePluginDev')
CheckContains $out 'error=' 'download failure -> error= line'
$script:FakeDownloadFail = $false

T 'Invoke-DoInstall reports a zip missing the expected .vlb'
$script:FakeDownloadZip = $BadZip
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'SamplePluginDev')
CheckContains $out 'error=' 'wrong .vlb name -> error= line'

T 'Invoke-DoInstall rejects missing arguments'
$out = AsText (Invoke-DoInstall '' '')
CheckContains $out 'error=' 'empty args -> error= line'

# ===========================================================================
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
Write-Output '---------------------------------------------------------------'
if ($script:TestsFailed -eq 0) {
    Write-Output ("PASS: all {0} checks passed." -f $script:TestsRun)
    exit 0
}
Write-Output ("FAIL: {0} of {1} checks failed." -f $script:TestsFailed, $script:TestsRun)
exit 1
