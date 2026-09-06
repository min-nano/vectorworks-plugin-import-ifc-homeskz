#!/usr/bin/env pwsh
#
#   vw-feedback.Tests.ps1
#
#   Unit tests for the Windows feedback back end (scripts/vw-feedback.ps1) — the
#   script the plug-in drives to post its run report to the pull request
#   (docs/DEV-NOTES.md M23). The PowerShell counterpart of
#   tests/vw-feedback.test.sh: it DOT-SOURCEs the script (its dispatch is
#   guarded, see the tail of vw-feedback.ps1) so the real functions run
#   in-process, and overrides just their outermost I/O leaves:
#
#     * Invoke-RestMethod        the GitHub REST boundary. Replaced with a stub
#                                that returns objects parsed from fixture JSON,
#                                records the URI and body, or throws to simulate
#                                a failure.
#     * Protect-/Unprotect-TokenText   the DPAPI boundary, which exists only on
#                                Windows. Replaced with a reversible transform so
#                                the REAL login / logout / lookup-order logic runs
#                                on the Linux CI runner.
#
#   Everything else — the token lookup order, the argument handling, the file
#   store, the wording of every machine-readable line — runs for real.
#
#   **トークンが出力へ漏れないこと**もここで確かめる（漏れたら PR コメントや診断ログに
#   載りうる）。Uses no Pester: a tiny in-file harness keeps it dependency-free,
#   matching tests/vw-update.Tests.ps1.
#

Set-StrictMode -Version Latest

$Here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $Here '..' 'scripts' 'vw-feedback.ps1'

# Missing-dependency policy, matching the other script harnesses.
$RequireTools = -not ([string]::IsNullOrEmpty($env:VW_REQUIRE_SCRIPT_TESTS) -or
    ($env:VW_REQUIRE_SCRIPT_TESTS -in @('0', 'off', 'OFF', 'false', 'FALSE', 'no', 'NO')))

function Skip-Or-Fail([string] $Reason) {
    if ($RequireTools) {
        Write-Error "vw-feedback.Tests.ps1: $Reason (VW_REQUIRE_SCRIPT_TESTS is set, refusing to skip)."
        exit 1
    }
    Write-Output "SKIP vw-feedback.Tests.ps1: $Reason."
    exit 0
}

if (-not (Test-Path -LiteralPath $Script)) {
    Skip-Or-Fail "$Script not found"
}

# Scratch store. Set VW_FEEDBACK_TOKEN_FILE BEFORE dot-sourcing so the script's
# %LOCALAPPDATA% default (absent on a Linux runner) is never used.
$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("vwfb-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$env:VW_FEEDBACK_TOKEN_FILE = Join-Path $Work 'token.dat'
$env:HOMESKZ_IFC_FEEDBACK_TOKEN = $null

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

function AsText($lines) { return (@($lines) -join "`n") }

# ---------------------------------------------------------------------------
# Stubs.
# ---------------------------------------------------------------------------

# DPAPI stand-in: reversible, deliberately NOT secure — this only has to prove
# that what login stores is what the lookup reads back.
function Protect-TokenText {
    param([string] $PlainText)
    return ('enc:' + $PlainText)
}

function Unprotect-TokenText {
    param([string] $Protected)
    if (-not $Protected.StartsWith('enc:')) { throw 'not encrypted by this stub' }
    return $Protected.Substring(4)
}

# gh CLI: absent unless a test turns it on (keeps the lookup order deterministic).
$script:FakeGhToken = $null
function Get-GhToken { return $script:FakeGhToken }

# The GitHub REST boundary.
$script:FakeApiFail = $false
$script:FakeApiResult = $null
$script:LastUri = ''
$script:LastBody = $null

function Invoke-RestMethod {
    param(
        [string] $Uri,
        [string] $Method,
        $Headers,
        [string] $ContentType,
        $Body,
        $TimeoutSec,
        [Parameter(ValueFromRemainingArguments = $true)] $Rest
    )
    $script:LastUri = $Uri
    $script:LastBody = $Body
    if ($script:FakeApiFail) { throw 'offline' }
    return $script:FakeApiResult
}

# Write a token hand-off file the way the plug-in does.
function New-TokenFile([string] $Text) {
    $path = Join-Path $Work ('handoff-' + [System.IO.Path]::GetRandomFileName())
    Set-Content -LiteralPath $path -Value $Text -NoNewline
    return $path
}

function Clear-Store {
    Remove-Item -LiteralPath $env:VW_FEEDBACK_TOKEN_FILE -Force -ErrorAction SilentlyContinue
}

# ===========================================================================
T 'token-status reports nothing when no token is configured'
Clear-Store
$script:FakeGhToken = $null
CheckEq (AsText (Invoke-TokenStatus)) "source=none`nok=no"

T 'login stores the token and deletes the hand-off file'
$handoff = New-TokenFile 'ghp_exampletoken'
CheckEq (AsText (Invoke-Login -TokenFile $handoff)) 'ok'
CheckEq (Test-Path -LiteralPath $handoff) $false 'the hand-off file must be deleted'
CheckEq (Get-StoredToken) 'ghp_exampletoken' 'the stored token reads back'
CheckEq (AsText (Invoke-TokenStatus)) "source=stored`nok=yes"

T 'token-status never prints the token itself'
CheckNotContains (AsText (Invoke-TokenStatus)) 'ghp_exampletoken'

T 'the environment variable takes priority over the store'
$env:HOMESKZ_IFC_FEEDBACK_TOKEN = 'env-secret-value'
CheckEq (AsText (Invoke-TokenStatus)) "source=env`nok=yes"
CheckEq (Resolve-Token) 'env-secret-value'
CheckNotContains (AsText (Invoke-TokenStatus)) 'env-secret-value'
$env:HOMESKZ_IFC_FEEDBACK_TOKEN = $null

T 'logout removes the store'
CheckEq (AsText (Invoke-Logout)) 'ok'
CheckEq (AsText (Invoke-TokenStatus)) "source=none`nok=no"

T 'the gh CLI is the last resort'
$script:FakeGhToken = 'gh-secret-value'
CheckEq (AsText (Invoke-TokenStatus)) "source=gh`nok=yes"
$script:FakeGhToken = $null

T 'login refuses an empty or missing token'
CheckEq (AsText (Invoke-Login -TokenFile (New-TokenFile ''))) 'error=トークンが空です。'
CheckEq (AsText (Invoke-Login -TokenFile (Join-Path $Work 'nope.txt'))) `
    'error=トークンのファイルが見つかりません。'

# ---------------------------------------------------------------------------
T 'find-pr resolves the open PR for a branch'
$handoff = New-TokenFile 'ghp_exampletoken'
Invoke-Login -TokenFile $handoff | Out-Null
$script:FakeApiFail = $false
$script:FakeApiResult = @( [pscustomobject]@{ number = 123; title = 'M23: feedback' } )
$out = AsText (Invoke-FindPr -Repo 'min-nano/vectorworks-plugin-import-ifc-homeskz' -Branch 'claude/feedback')
CheckEq $out "pr=123`ntitle=M23: feedback`nok"
CheckContains $script:LastUri 'head=min-nano:claude/feedback' 'queries head=<owner>:<branch>'

T 'find-pr reports when the branch has no open PR'
$script:FakeApiResult = @()
CheckEq (AsText (Invoke-FindPr -Repo 'o/r' -Branch 'no-such-branch')) `
    'error=ブランチ no-such-branch に open な PR がありません。'

T 'find-pr refuses a missing branch'
CheckEq (AsText (Invoke-FindPr -Repo 'o/r' -Branch '')) 'error=ブランチが指定されていません。'

T 'find-pr survives an unreachable API'
$script:FakeApiFail = $true
CheckEq (AsText (Invoke-FindPr -Repo 'o/r' -Branch 'b')) `
    'error=PR を検索できませんでした（ネットワークか権限）。'
$script:FakeApiFail = $false

# ---------------------------------------------------------------------------
T 'post sends the body as JSON and reports the comment URL'
$bodyFile = Join-Path $Work 'body.md'
$bodyText = "## 実機フィードバック `"round 1`"`nバックスラッシュ \ と `"引用符`"`n"
Set-Content -LiteralPath $bodyFile -Value $bodyText -NoNewline
$script:FakeApiResult = [pscustomobject]@{ html_url = 'https://github.com/o/r/pull/123#issuecomment-1' }
$out = AsText (Invoke-Post -Repo 'o/r' -Number '123' -BodyFile $bodyFile)
CheckEq $out "url=https://github.com/o/r/pull/123#issuecomment-1`nok"
CheckContains $script:LastUri 'https://api.github.com/repos/o/r/issues/123/comments' `
    'targets the issue-comments endpoint'

T 'the payload round-trips the body exactly'
# 送るのは UTF-8 のバイト列。読み戻して JSON として解けること、そして本文が
# 1 文字も変わっていないことを確かめる（引用符・バックスラッシュ・日本語）。
$sent = [Text.Encoding]::UTF8.GetString($script:LastBody)
$decoded = ($sent | ConvertFrom-Json).body
CheckEq $decoded $bodyText 'the JSON payload must round-trip the body'

T 'post relays the failure reason instead of throwing'
$script:FakeApiFail = $true
CheckContains (AsText (Invoke-Post -Repo 'o/r' -Number '123' -BodyFile $bodyFile)) `
    'error=コメントを投稿できませんでした' 'a failed post is reported, not fatal'
$script:FakeApiFail = $false

T 'post without a token says so'
Clear-Store
$script:FakeGhToken = $null
CheckEq (AsText (Invoke-Post -Repo 'o/r' -Number '123' -BodyFile $bodyFile)) `
    'error=GitHub のトークンがありません（先に login してください）。'

T 'post refuses a missing body file'
$handoff = New-TokenFile 'ghp_exampletoken'
Invoke-Login -TokenFile $handoff | Out-Null
CheckEq (AsText (Invoke-Post -Repo 'o/r' -Number '123' -BodyFile (Join-Path $Work 'none.md'))) `
    'error=引数が不足しています。'

# ---------------------------------------------------------------------------
# ask-note — 所見を別プロセスで訊く（M23）。肝は「すぐ返る」こと。
# ---------------------------------------------------------------------------
$script:Spawned = $null
function Invoke-SelfDetached {
    param([string[]] $ScriptArguments)
    $script:Spawned = ($ScriptArguments -join ' ')
}

$script:NoteAnswer = $null
$script:NotePrompt = ''
function Get-Note {
    param([string] $Title, [string] $Prompt)
    $script:NotePrompt = $Prompt
    return $script:NoteAnswer
}

$script:Alerted = $null
function Show-NoteAlert {
    param([string] $Title, [string] $Message)
    $script:Alerted = $Message
}

$script:Opened = $null
function Open-Url {
    param([string] $Url)
    $script:Opened = $Url
}

T 'ask-note returns immediately and hands the worker everything it needs'
$script:Spawned = $null
CheckEq (AsText (Invoke-AskNote -Repo 'o/r' -Number '123' -Round '2' -Build 'abc1234' `
            -Url 'https://example.test/c1')) 'ok'
CheckEq $script:Spawned 'ask-note-worker o/r 123 2 abc1234 https://example.test/c1 yes'

T 'ask-note passes on that the round was NOT posted'
$script:Spawned = $null
CheckEq (AsText (Invoke-AskNote -Repo 'o/r' -Number '123' -Round '5' -Build 'abc1234' `
            -Url '' -Posted 'no')) 'ok'
CheckEq $script:Spawned 'ask-note-worker o/r 123 5 abc1234  no'

T 'ask-note refuses a missing PR'
CheckEq (AsText (Invoke-AskNote -Repo 'o/r' -Number '' -Round '1' -Build 'abc' -Url '')) `
    'error=引数が不足しています。'

T 'the worker posts the note as its own comment'
$handoff = New-TokenFile 'ghp_exampletoken'
Invoke-Login -TokenFile $handoff | Out-Null
$script:FakeApiFail = $false
$script:FakeApiResult = [pscustomobject]@{ html_url = 'https://github.com/o/r/pull/123#issuecomment-2' }
$script:NoteAnswer = "3 階の梁が浮いている"
$script:Opened = $null
Invoke-AskNoteWorker -Repo 'o/r' -Number '123' -Round '2' -Build 'abc1234' `
    -Url 'https://example.test/c1'
$sent = [Text.Encoding]::UTF8.GetString($script:LastBody)
$decoded = ($sent | ConvertFrom-Json).body
CheckContains $decoded '<!-- homeskz-ifc-feedback-note v1 round=2 build=abc1234 posted=yes -->' `
    'marks the note as its own kind of comment'
CheckContains $decoded '> 3 階の梁が浮いている' 'quotes what the person wrote'
CheckContains $decoded '### 実機を見ての所見（round 2）' 'says which round it belongs to'
CheckContains $script:NotePrompt 'round 2 を投稿しました' 'tells the person the round was posted'
CheckEq $script:Opened 'https://example.test/c1' 'opens the posted comment'

T 'the worker quotes every line of a multi-line note'
$script:NoteAnswer = "1 行目`n2 行目"
Invoke-AskNoteWorker -Repo 'o/r' -Number '123' -Round '3' -Build 'abc1234' -Url ''
$decoded = ([Text.Encoding]::UTF8.GetString($script:LastBody) | ConvertFrom-Json).body
CheckContains $decoded "> 1 行目`n> 2 行目" 'every line is quoted'

T 'the worker posts nothing when there is no note'
$script:LastBody = $null
$script:NoteAnswer = $null
$script:Opened = $null
Invoke-AskNoteWorker -Repo 'o/r' -Number '123' -Round '2' -Build 'abc1234' `
    -Url 'https://example.test/c1'
CheckEq ($null -eq $script:LastBody) $true 'nothing was posted'
CheckEq $script:Opened 'https://example.test/c1' 'the posted comment is still opened'

T 'the worker treats a round that was NOT posted differently'
# **その周について PR に載るのはこの所見だけ**になるので、そう伝えたうえで訊く。黙って
# 終わると、読む側からはその周が丸ごと消える（実機 round 4 の所見）。
$script:NoteAnswer = '今回は絵を見たかっただけ'
$script:Opened = 'まだ開いていない'
Invoke-AskNoteWorker -Repo 'o/r' -Number '123' -Round '5' -Build 'abc1234' -Url '' -Posted 'no'
$decoded = ([Text.Encoding]::UTF8.GetString($script:LastBody) | ConvertFrom-Json).body
CheckContains $decoded '<!-- homeskz-ifc-feedback-note v1 round=5 build=abc1234 posted=no -->' `
    'records that the round was not posted'
CheckContains $decoded '### 実機を見ての所見（round 5・結果は未投稿）' 'says so in the heading'
CheckContains $script:NotePrompt '唯一の記録' 'tells the person it is the only record'

T 'an empty note on an unposted round leaves nothing behind'
$script:LastBody = $null
$script:NoteAnswer = $null
Invoke-AskNoteWorker -Repo 'o/r' -Number '123' -Round '6' -Build 'abc1234' -Url '' -Posted 'no'
CheckEq ($null -eq $script:LastBody) $true 'nothing was posted'

T 'the worker says so when the note could not be posted'
# プラグインはもう戻っているので、ここで黙ると書いた所見がどこにも残らないまま消える。
$script:Alerted = $null
$script:NoteAnswer = 'なにか'
$script:FakeApiFail = $true
Invoke-AskNoteWorker -Repo 'o/r' -Number '123' -Round '2' -Build 'abc1234' -Url ''
CheckContains $script:Alerted '所見を投稿できませんでした' 'a failed note is reported'
$script:FakeApiFail = $false

# ---------------------------------------------------------------------------
T 'an unknown mode is reported, not silently ignored'
CheckContains (AsText (Invoke-Main -Arguments @('nonsense'))) 'error=不明なモード' `
    'unknown modes must report'

T 'a mode with missing arguments does not crash'
CheckContains (AsText (Invoke-Main -Arguments @('find-pr'))) 'error=' `
    'missing arguments must be reported'

# ===========================================================================
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
Write-Output '---------------------------------------------------------------'
if ($script:TestsFailed -eq 0) {
    Write-Output ("PASS: all {0} checks passed." -f $script:TestsRun)
    exit 0
}
Write-Output ("FAIL: {0} of {1} checks failed." -f $script:TestsFailed, $script:TestsRun)
exit 1
