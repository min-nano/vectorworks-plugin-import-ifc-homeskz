#!/usr/bin/env pwsh
#
#   vw-uninstall.Tests.ps1
#
#   Unit tests for the Windows UNINSTALLER (scripts/vw-uninstall.ps1) — the script
#   that ships inside every release zip, is INSTALLED alongside the plug-in, and is
#   re-run by the NEXT release's installer to remove the one it replaces.
#
#   **このスイートが本当に守っているのは削除の安全性である。** ここは本リポジトリで
#   唯一「利用者のディスク上のものを消す」コードなので、中心の検査は 2 つ:
#
#     * **消してよいものだけを消す** — フォルダ名が一致し、かつ中に殻があるときだけ。
#       Plug-Ins そのものや無関係なフォルダを名指しされても消さない。
#     * **入っていなければ成功** — アップデートの入口で無条件に叩けること。
#
#   The script is DOT-SOURCEd (its dispatch is guarded, see its tail) so the real
#   functions run in-process. **Nothing is stubbed** — the deletion itself is what
#   is under test, and it runs for real against temp directories.
#
#   The PowerShell counterpart of tests/vw-uninstall.test.sh.
#

Set-StrictMode -Version Latest

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $Here '..' 'scripts' 'vw-uninstall.ps1'

$RequireTools = -not ([string]::IsNullOrEmpty($env:VW_REQUIRE_SCRIPT_TESTS) -or
    ($env:VW_REQUIRE_SCRIPT_TESTS -in @('0', 'off', 'OFF', 'false', 'FALSE', 'no', 'NO')))

function Skip-Or-Fail([string] $Reason) {
    if ($RequireTools) {
        Write-Error "vw-uninstall.Tests.ps1: $Reason (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)."
        exit 1
    }
    Write-Output "SKIP vw-uninstall.Tests.ps1: $Reason."
    exit 0
}

if (-not (Test-Path -LiteralPath $Script)) {
    Skip-Or-Fail "$Script not found"
}

# VW_PLUGINS_DIR は dot-source の前に置く（スクリプト先頭の既定値が %APPDATA% を
# 参照するため。Linux ランナーにそれは無い）。
$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("vwunintest-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$env:VW_PLUGINS_DIR = Join-Path $Work 'plugins'

. $Script

# **エラーの扱いはローカルと CI で変える。** これは「CI では緩めない」という
# VW_REQUIRE_SCRIPT_TESTS の方針（上記 $RequireTools）をそのまま延長したもの。
#
#   * ローカル（Continue）… 落ちた文があっても最後まで走り、失敗を一覧できる。
#   * CI（Stop）………………… 想定外のエラーでその場で終了し、exit 1 になる。
#
# Stop が要る理由: Continue だと**落ちた文の CheckXxx が呼ばれないまま**次へ進むので、
# 検査が空振りしたのに「PASS: all N checks」と出る。実際に `Join-Path 'C:\x' …`
# （Linux の pwsh に C: ドライブは無い）で 2 件が黙って抜け、N だけが減っていた。
# ローカルを Continue のままにしてあるのは、直すときは失敗を一覧できたほうが速いから。
$ErrorActionPreference = if ($RequireTools) { 'Stop' } else { 'Continue' }

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

function CheckPath([string] $path, [string] $label) {
    $script:TestsRun++
    if (-not (Test-Path -LiteralPath $path)) { Fail $label ("  missing path: {0}" -f $path) }
}

function CheckNoPath([string] $path, [string] $label) {
    $script:TestsRun++
    if (Test-Path -LiteralPath $path) { Fail $label ("  unexpected path: {0}" -f $path) }
}

function AsText($lines) { return (@($lines) -join "`n") }

$Name = 'HomeskzIfcImportDev'

# 実際のインストール後と同じ形を作る。
function New-Install([string] $root, [string] $name = $Name) {
    $own = Join-Path $root $name
    New-Item -ItemType Directory -Force -Path $own | Out-Null
    Set-Content -LiteralPath (Join-Path $own "$name.vlb") -Value 'dll' -NoNewline
    Set-Content -LiteralPath (Join-Path $own "$name.vwpayload") -Value 'payload' -NoNewline
    Set-Content -LiteralPath (Join-Path $own 'vw-uninstall.ps1') -Value '# uninstaller' -NoNewline
    return $own
}

# ===========================================================================
# Get-PluginDir — **インストーラと同じ規則**でなければならない。
# ===========================================================================
T "Get-PluginDir appends the plug-in's own folder"
CheckEq (Get-PluginDir (Join-Path $Work 'Plug-Ins') $Name) (Join-Path (Join-Path $Work 'Plug-Ins') $Name) 'Plug-Ins -> Plug-Ins\<name>'

T "Get-PluginDir does not nest when it is already the plug-in's folder"
$already = Join-Path (Join-Path $Work 'Plug-Ins') $Name
CheckEq (Get-PluginDir $already $Name) $already 'already there -> unchanged'

# ===========================================================================
# 取り除く — フォルダごと。その版が増やしたファイルも一緒に消えること。
# ===========================================================================
T 'Uninstall-PluginDir removes the whole plug-in folder'
$root = Join-Path $Work 'plugins-ok'
$own = New-Install $root
Set-Content -LiteralPath (Join-Path $own "$Name.some-future-file") -Value 'extra' -NoNewline
$script:PluginsDir = $root
CheckEq (Uninstall-PluginDir $own $Name) $true 'remove succeeds'
CheckNoPath $own 'the plug-in folder is gone'
CheckPath $root 'Plug-Ins itself is untouched'

# ===========================================================================
# **安全弁** — ここが本スイートの中心。
# ===========================================================================
T "Uninstall-PluginDir refuses a folder whose name is not the plug-in's"
$root = Join-Path $Work 'plugins-name'
$other = Join-Path $root 'SomethingElse'
New-Item -ItemType Directory -Force -Path $other | Out-Null
Set-Content -LiteralPath (Join-Path $other 'keep.txt') -Value 'keep me' -NoNewline
CheckEq (Uninstall-PluginDir $other $Name) $false 'refused'
CheckPath (Join-Path $other 'keep.txt') 'nothing was deleted'

T 'Uninstall-PluginDir refuses a folder that holds no shell'
# Plug-Ins そのものを名指しされた形。消すと利用者の他のプラグインごと消える。
$root = Join-Path $Work 'Plug-Ins'
New-Item -ItemType Directory -Force -Path (Join-Path $root 'SomeoneElsePlugin') | Out-Null
Set-Content -LiteralPath (Join-Path $root 'important.txt') -Value 'keep me' -NoNewline
CheckEq (Uninstall-PluginDir $root 'Plug-Ins') $false 'refused'
CheckPath (Join-Path $root 'important.txt') 'the other plug-ins are untouched'
CheckPath (Join-Path $root 'SomeoneElsePlugin') 'the other plug-ins are untouched'

# ===========================================================================
# 入っていなければ成功。
# ===========================================================================
T 'Uninstall-PluginDir succeeds when there is nothing installed'
CheckEq (Uninstall-PluginDir (Join-Path (Join-Path $Work 'nowhere') $Name) $Name) $true 'absent -> success'

# ===========================================================================
# Get-InstalledName — 名前を渡されなくても、置かれているものから割り出す。
# ===========================================================================
T 'Get-InstalledName finds the plug-in inside a Plug-Ins folder'
$root = Join-Path $Work 'plugins-guess'
$own = New-Install $root
CheckEq (Get-InstalledName $root) $Name 'found from <Plug-Ins>\<name>\<name>.vlb'

T "Get-InstalledName accepts the plug-in's own folder"
CheckEq (Get-InstalledName $own) $Name 'found from the folder itself'

T 'Get-InstalledName is empty when nothing is installed'
$empty = Join-Path $Work 'plugins-empty'
New-Item -ItemType Directory -Force -Path $empty | Out-Null
CheckEq (Get-InstalledName $empty) '' 'nothing -> empty'

# ===========================================================================
# Invoke-Main -Machine — the contract the installer reads: "removed=<path>" then
# "ok", or a single "error=<message>".
# ===========================================================================
T 'Invoke-Main -Machine removes the install and reports where'
$root = Join-Path $Work 'plugins-machine'
$own = New-Install $root
$out = AsText (Invoke-Main @('-Machine', '-PluginsDir', $root))
CheckContains $out "removed=$own" 'reports the removed path'
CheckContains $out 'ok' 'ok line'
CheckNoPath $own 'the plug-in folder is gone'

T 'Invoke-Main -Machine is a no-op success when nothing is installed'
$out = AsText (Invoke-Main @('-Machine', '-PluginsDir', $empty))
CheckEq $out 'ok' 'just ok — nothing to remove'

T 'Invoke-Main -Machine reports a refusal as error='
$root = Join-Path $Work 'plugins-refuse'
$own = Join-Path $root $Name
New-Item -ItemType Directory -Force -Path $own | Out-Null
Set-Content -LiteralPath (Join-Path $own 'stray.txt') -Value 'no shell here' -NoNewline
$out = AsText (Invoke-Main @('-Machine', '-PluginsDir', $root, '-Name', $Name))
CheckContains $out 'error=' 'refusal is reported on stdout'
CheckPath (Join-Path $own 'stray.txt') 'nothing was deleted'

T 'Invoke-Main -Machine tolerates options it does not know'
$root = Join-Path $Work 'plugins-future'
$own = New-Install $root
$out = AsText (Invoke-Main @('-Machine', '-PluginsDir', $root, '-AFutureOption', '42', '-AFutureFlag'))
CheckContains $out 'ok' 'unknown options do not stop the removal'
CheckNoPath $own 'the plug-in folder is gone'

T "Invoke-Main removes the plug-in when handed its own folder"
# アップデータ経由の呼ばれ方（プラグインは「いま自分が読み込まれたフォルダ」を渡す）。
$root = Join-Path $Work 'plugins-own'
$own = New-Install $root
$out = AsText (Invoke-Main @('-Machine', '-PluginsDir', $own, '-Name', $Name))
CheckContains $out 'ok' 'ok'
CheckNoPath $own 'the plug-in folder is gone'

# ===========================================================================
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
Write-Output '---------------------------------------------------------------'
if ($script:TestsFailed -eq 0) {
    Write-Output ("PASS: all {0} checks passed." -f $script:TestsRun)
    exit 0
}
Write-Output ("FAIL: {0} of {1} checks failed." -f $script:TestsFailed, $script:TestsRun)
exit 1
