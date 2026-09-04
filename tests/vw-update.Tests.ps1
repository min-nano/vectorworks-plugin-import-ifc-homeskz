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
    { "name": "HomeskzIfcImport.vlb.zip",
      "browser_download_url": "https://example.test/dl/HomeskzIfcImport.vlb.zip" },
    { "name": "notes.txt",
      "browser_download_url": "https://example.test/dl/notes.txt" }
  ]
}
'@

$script:FakeReleasesJson = @'
[
  { "tag_name": "stable", "name": "stable", "target_commitish": "zzz9999",
    "assets": [ { "name": "HomeskzIfcImport.vlb.zip",
                  "browser_download_url": "https://example.test/dl/stable.zip" } ] },
  { "tag_name": "dev-feature-x", "name": "feature/x", "target_commitish": "aaa1111ccc",
    "assets": [ { "name": "HomeskzIfcImportDev.vlb.zip",
                  "browser_download_url": "https://example.test/dl/x.zip" } ] },
  { "tag_name": "dev-feature-y", "name": "feature/y", "target_commitish": "bbb2222ddd",
    "assets": [ { "name": "HomeskzIfcImportDev.vlb.zip",
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
    # 本体と殻の ID も、実際のリリース zip と同じように入れる。**殻だけ入れて本体を
    # 取りこぼす退行**を捕まえるため（src/PayloadAbi.h）。
    Set-Content -LiteralPath (Join-Path $stage "$vlbName.vwpayload") -Value 'payload' -NoNewline
    Set-Content -LiteralPath (Join-Path $stage "$vlbName.shell-id") -Value 'abc123def456' -NoNewline
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -Force
    Remove-Item -LiteralPath $stage -Recurse -Force
}
$GoodZip = Join-Path $Work 'good.zip'
$BadZip  = Join-Path $Work 'bad.zip'
New-BuildZip $GoodZip 'HomeskzIfcImportDev'
New-BuildZip $BadZip  'WrongName'

# ===========================================================================
# Get-AssetUrl / Get-Short — the pure helpers.
# ===========================================================================
T 'Get-AssetUrl finds the matching asset'
$rel = $script:FakeStableJson | ConvertFrom-Json
CheckEq (Get-AssetUrl $rel 'HomeskzIfcImport.vlb.zip') 'https://example.test/dl/HomeskzIfcImport.vlb.zip' 'returns the URL'

T 'Get-AssetUrl returns null for an unknown asset'
CheckEq (Get-AssetUrl $rel 'does-not-exist.zip') $null 'null when no asset matches'

T 'Get-Short takes the first 7 chars'
CheckEq (Get-Short 'abc1234def5678') 'abc1234' '7-char prefix'
CheckEq (Get-Short '') '' 'empty stays empty'

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
CheckContains $out 'latest=abc1234' 'latest is the 7-char commit prefix'
CheckContains $out 'url=https://example.test/dl/HomeskzIfcImport.vlb.zip' 'url line'

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
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'HomeskzIfcImportDev')
CheckContains $out 'ok' 'prints ok'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.vlb')) $true 'the .vlb landed'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.commit')) $true 'the .commit sidecar landed'
# **本体も入っていること。** 殻だけ入れて本体を取りこぼすと、次の起動でプラグインは
# 何もできなくなる（src/PayloadHost.cpp が「本体が見つかりません」と言うだけ）。
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.vwpayload')) $true 'the .vwpayload landed'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.shell-id')) $true 'the shell-id stamp landed'
# 入れた殻の ID を先に出す。プラグインはこれを自分の VW_SHELL_ID と突き合わせて、
# **本体の読み直しで済むなら再起動を尋ねない**（src/UpdaterParse.h）。
CheckContains $out 'installed-shell=abc123def456' 'prints the installed shell id'

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
# Get-PluginZipUrl — the distribution zip is found by exact name, and STILL found
# after the asset is renamed. **これが効かないと、アセット名を変えた瞬間に
# インストール済みの古いアップデータからは何も落とせなくなる**（利用者は手で落とす
# しかなくなる）。
# ===========================================================================
T 'Get-PluginZipUrl prefers the exact asset name'
$relExact = $script:FakeStableJson | ConvertFrom-Json
CheckEq (Get-PluginZipUrl $relExact 'HomeskzIfcImport') 'https://example.test/dl/HomeskzIfcImport.vlb.zip' 'exact match wins'

T 'Get-PluginZipUrl still finds the zip after the asset was renamed'
$relRenamed = @'
{
  "target_commitish": "abc1234def5678",
  "assets": [
    { "name": "notes.txt", "browser_download_url": "https://example.test/dl/notes.txt" },
    { "name": "SomethingElse.vlb.zip",
      "browser_download_url": "https://example.test/dl/renamed.zip" }
  ]
}
'@ | ConvertFrom-Json
CheckEq (Get-PluginZipUrl $relRenamed 'HomeskzIfcImport') 'https://example.test/dl/renamed.zip' 'falls back to any *.vlb.zip'

# ===========================================================================
# do-install の委譲 — **この変更の要**。落とした zip に vw-install.ps1 が入っていたら、
# 配置はそちらへ渡し、その機械可読な出力をそのまま流す。自前の配置（予備）は使わない。
#
# 偽インストーラは「自分が呼ばれた証拠」を残して ok を出すだけ。**自前の配置なら必ず
# 置かれるはずの .vlb が置かれていないこと**を見て、委譲が起きたと判定する。
# ===========================================================================
function New-BuildZipWithInstaller([string] $zipPath, [string] $vlbName, [string] $installerBody) {
    $stage = Join-Path $Work ("stage-inst-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Set-Content -LiteralPath (Join-Path $stage "$vlbName.vlb") -Value 'dll' -NoNewline
    Set-Content -LiteralPath (Join-Path $stage "$vlbName.vwpayload") -Value 'payload' -NoNewline
    # param() ブロックを持たない = 未知の名前付き引数も $args に落ちるだけで束縛エラーに
    # ならない（本物の vw-install.ps1 と同じ作法）。
    Set-Content -LiteralPath (Join-Path $stage 'vw-install.ps1') -Value $installerBody
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -Force
    Remove-Item -LiteralPath $stage -Recurse -Force
}

$SavedPluginsDir = $VW_PLUGINS_DIR

T 'Invoke-DoInstall hands the placement to the installer that came with the zip'
$VW_PLUGINS_DIR = Join-Path $Work 'plugins-delegated'
New-Item -ItemType Directory -Force -Path $VW_PLUGINS_DIR | Out-Null
$env:VW_TEST_MARKER = Join-Path $Work 'installer-ran.txt'
$DelegatedZip = Join-Path $Work 'delegated.zip'
New-BuildZipWithInstaller $DelegatedZip 'HomeskzIfcImportDev' @'
Set-Content -LiteralPath $env:VW_TEST_MARKER -Value 'ran' -NoNewline
Write-Output 'installed-shell=from-installer'
Write-Output 'ok'
'@
$script:FakeDownloadFail = $false
$script:FakeDownloadZip = $DelegatedZip
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'HomeskzIfcImportDev')
CheckContains $out 'installed-shell=from-installer' "the installer's lines are passed through"
CheckContains $out 'ok' 'ok is passed through'
CheckEq (Test-Path -LiteralPath $env:VW_TEST_MARKER) $true 'the bundled installer actually ran'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.vlb')) $false 'the built-in placement was NOT used'

T 'Invoke-DoInstall passes an installer error through unchanged'
$VW_PLUGINS_DIR = Join-Path $Work 'plugins-delegated-err'
New-Item -ItemType Directory -Force -Path $VW_PLUGINS_DIR | Out-Null
$ErrZip = Join-Path $Work 'delegated-err.zip'
New-BuildZipWithInstaller $ErrZip 'HomeskzIfcImportDev' "Write-Output 'error=インストーラからの理由'"
$script:FakeDownloadZip = $ErrZip
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'HomeskzIfcImportDev')
CheckContains $out 'error=インストーラからの理由' "the installer's error reaches the plug-in"
CheckNotContains $out 'ok' 'no ok line'

T 'Invoke-DoInstall falls back to its own placement when the installer says nothing'
$VW_PLUGINS_DIR = Join-Path $Work 'plugins-mute'
New-Item -ItemType Directory -Force -Path $VW_PLUGINS_DIR | Out-Null
$MuteZip = Join-Path $Work 'delegated-mute.zip'
New-BuildZipWithInstaller $MuteZip 'HomeskzIfcImportDev' "Write-Output 'something unexpected'"
$script:FakeDownloadZip = $MuteZip
$out = AsText (Invoke-DoInstall 'https://example.test/dl/x.zip' 'HomeskzIfcImportDev')
CheckContains $out 'ok' 'the built-in placement reported success'
CheckEq (Test-Path -LiteralPath (Join-Path $VW_PLUGINS_DIR 'HomeskzIfcImportDev.vwpayload')) $true 'a mute installer never counts as done'

$VW_PLUGINS_DIR = $SavedPluginsDir

# ===========================================================================
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
Write-Output '---------------------------------------------------------------'
if ($script:TestsFailed -eq 0) {
    Write-Output ("PASS: all {0} checks passed." -f $script:TestsRun)
    exit 0
}
Write-Output ("FAIL: {0} of {1} checks failed." -f $script:TestsFailed, $script:TestsRun)
exit 1
