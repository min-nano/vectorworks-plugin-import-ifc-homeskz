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
#     * Get-Manifest       the R2 boundary. Replaced with a stub that returns
#                          objects parsed from fixture JSON (exactly what the real
#                          Invoke-RestMethod would hand back), or throws to
#                          simulate an unreachable bucket.
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

# Likewise the distribution base URL: it is injected at BUILD time (CMake
# substitutes @VW_UPDATE_BASE_URL@), so the repository copy has none. Supply one
# through the environment, exactly as a manual run would. The "not configured"
# branch is covered by its own test at the end.
$env:VW_BASE_URL = 'https://dist.example.test'

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
$script:FakeIndexJson = $null
$script:FakeDownloadZip = $null
$script:FakeDownloadFail = $false

function Get-Manifest([string] $key) {
    if ($script:FakeApiFail) { throw 'offline' }
    if ($key -eq 'stable/manifest.json') { return ($script:FakeStableJson | ConvertFrom-Json) }
    if ($key -eq 'dev/index.json') { return ($script:FakeIndexJson | ConvertFrom-Json) }
    throw "unexpected key: $key"
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
# The two JSON objects the bucket serves (see scripts/r2-publish.sh for the
# schema): the stable manifest, and the dev index listing one entry per branch.
$script:FakeStableJson = @'
{
  "schema": 1,
  "channel": "stable",
  "branch": "main",
  "commit": "abc1234def5678",
  "short": "abc1234",
  "built": "2026-08-01T00:00:00Z",
  "mac": "https://dist.example.test/stable/abc1234/HomeskzIfcImport.vwlibrary.zip",
  "win": "https://dist.example.test/stable/abc1234/HomeskzIfcImport.vlb.zip"
}
'@

# feature/y deliberately has NO "short" (the commit-prefix fallback must kick
# in), feature/z has no download URLs (must be skipped), and the last entry has
# no "branch" (the slug must stand in as the display name).
$script:FakeIndexJson = @'
{
  "schema": 1,
  "generated": "2026-08-01T00:00:00Z",
  "builds": [
    { "branch": "feature/x", "slug": "feature-x",
      "commit": "aaa1111ccc", "short": "aaa1111",
      "mac": "https://dist.example.test/dev/feature-x/aaa1111/HomeskzIfcImportDev.vwlibrary.zip",
      "win": "https://dist.example.test/dev/feature-x/aaa1111/HomeskzIfcImportDev.vlb.zip" },
    { "branch": "feature/y", "slug": "feature-y",
      "commit": "bbb2222ddd",
      "mac": "https://dist.example.test/dev/feature-y/bbb2222/HomeskzIfcImportDev.vwlibrary.zip",
      "win": "https://dist.example.test/dev/feature-y/bbb2222/HomeskzIfcImportDev.vlb.zip" },
    { "branch": "feature/z", "slug": "feature-z",
      "commit": "ccc3333eee", "short": "ccc3333" },
    { "slug": "no-branch-field",
      "commit": "ddd4444fff", "short": "ddd4444",
      "mac": "https://dist.example.test/dev/no-branch-field/ddd4444/HomeskzIfcImportDev.vwlibrary.zip",
      "win": "https://dist.example.test/dev/no-branch-field/ddd4444/HomeskzIfcImportDev.vlb.zip" }
  ]
}
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
New-BuildZip $GoodZip 'HomeskzIfcImportDev'
New-BuildZip $BadZip  'WrongName'

# ===========================================================================
# Get-Field / Get-Short / Get-BuildId / Get-BuildName — the pure helpers.
# ===========================================================================
$manifest = $script:FakeStableJson | ConvertFrom-Json
$entries = ($script:FakeIndexJson | ConvertFrom-Json).builds

T 'Get-Field reads a present property'
CheckEq (Get-Field $manifest 'win') 'https://dist.example.test/stable/abc1234/HomeskzIfcImport.vlb.zip' 'returns the URL'

T 'Get-Field returns null for an absent property'
CheckEq (Get-Field $manifest 'does-not-exist') $null 'null when the field is missing'
CheckEq (Get-Field $null 'win') $null 'null object -> null'

T 'Get-Short takes the first 7 chars'
CheckEq (Get-Short 'abc1234def5678') 'abc1234' '7-char prefix'
CheckEq (Get-Short '') '' 'empty stays empty'

T 'Get-BuildId prefers short and falls back to the commit prefix'
CheckEq (Get-BuildId $manifest) 'abc1234' 'manifest short'
CheckEq (Get-BuildId $entries[0]) 'aaa1111' 'entry short'
CheckEq (Get-BuildId $entries[1]) 'bbb2222' 'first 7 chars of commit'

T 'Get-BuildName prefers branch and falls back to slug'
CheckEq (Get-BuildName $entries[0]) 'feature/x' 'branch'
CheckEq (Get-BuildName $entries[3]) 'no-branch-field' 'slug when branch is absent'

# ===========================================================================
# Get-InstalledCommit — reads the real "<name>.commit" sidecar (no OS tool).
# ===========================================================================
T 'Get-InstalledCommit reads the sidecar commit'
Set-Content -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImport.commit') -Value "abc1234`n"
CheckEq (Get-InstalledCommit 'HomeskzIfcImport') 'abc1234' 'trimmed sidecar value'

T 'Get-InstalledCommit is none when the sidecar is absent'
CheckEq (Get-InstalledCommit 'HomeskzIfcImportDev') 'none' 'absent sidecar -> none'

# ===========================================================================
# q-stable — installed / latest / url, and the offline / incomplete paths.
# ===========================================================================
T 'Invoke-QStable reports installed, 7-char latest and the asset url'
$script:FakeApiFail = $false
$out = AsText (Invoke-QStable)
CheckContains $out 'installed=abc1234' 'installed line (from sidecar)'
CheckContains $out 'latest=abc1234' 'latest is the 7-char build id'
CheckContains $out 'url=https://dist.example.test/stable/abc1234/HomeskzIfcImport.vlb.zip' 'url line (Windows asset)'
CheckNotContains $out 'HomeskzIfcImport.vwlibrary.zip' 'the macOS asset is not offered to Windows'

T 'Invoke-QStable emits an error line when the manifest is unreachable'
$script:FakeApiFail = $true
$out = AsText (Invoke-QStable)
CheckContains $out 'error=' 'offline -> error= line'
CheckNotContains $out 'latest=' 'no latest when offline'
$script:FakeApiFail = $false

# ===========================================================================
# q-dev — installed line + one TSV row per indexed build that has a downloadable
# asset.
# ===========================================================================
T 'Invoke-QDev lists only indexed builds that have a downloadable asset'
$out = AsText (Invoke-QDev)
CheckContains $out ("build`taaa1111`tfeature/x`thttps://dist.example.test/dev/feature-x/aaa1111/HomeskzIfcImportDev.vlb.zip") 'feature/x row'
CheckContains $out ("build`tbbb2222`tfeature/y`thttps://dist.example.test/dev/feature-y/bbb2222/HomeskzIfcImportDev.vlb.zip") 'feature/y row (short derived from commit)'
CheckNotContains $out 'feature/z' 'asset-less build is skipped'
CheckContains $out ("build`tddd4444`tno-branch-field`t") 'slug stands in when branch is absent'

T 'Invoke-QDev emits an error line when the index is unreachable'
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
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'HomeskzIfcImportDev')
CheckEq $out 'ok' 'prints ok'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.vlb')) $true 'the .vlb landed'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.commit')) $true 'the .commit sidecar landed'

T 'Invoke-DoInstall reports a download failure'
$script:FakeDownloadFail = $true
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'HomeskzIfcImportDev')
CheckContains $out 'error=' 'download failure -> error= line'
$script:FakeDownloadFail = $false

T 'Invoke-DoInstall reports a zip missing the expected .vlb'
$script:FakeDownloadZip = $BadZip
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'HomeskzIfcImportDev')
CheckContains $out 'error=' 'wrong .vlb name -> error= line'

T 'Invoke-DoInstall rejects missing arguments'
$out = AsText (Invoke-DoInstall '' '')
CheckContains $out 'error=' 'empty args -> error= line'

# ===========================================================================
# No distribution base URL configured — the state of the repository copy, whose
# @VW_UPDATE_BASE_URL@ placeholder is only substituted when CMake bundles the
# script. Both query modes must say so instead of reaching the network. Loaded
# into a CHILD scope so the real Get-Manifest runs rather than the stub above.
# ===========================================================================
T 'the query modes report a missing base URL'
$SavedBase = $env:VW_BASE_URL
$env:VW_BASE_URL = ''
$out = AsText (& { . $Script; Invoke-QStable; Invoke-QDev })
$env:VW_BASE_URL = $SavedBase
CheckContains $out 'error=配布先 URL' 'unset VW_BASE_URL -> error= line'
CheckNotContains $out 'installed=' 'nothing else is printed'

# ===========================================================================
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
Write-Output '---------------------------------------------------------------'
if ($script:TestsFailed -eq 0) {
    Write-Output ("PASS: all {0} checks passed." -f $script:TestsRun)
    exit 0
}
Write-Output ("FAIL: {0} of {1} checks failed." -f $script:TestsFailed, $script:TestsRun)
exit 1
