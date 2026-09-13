<#
    vw-feedback.ps1 — post the plug-in's run report back to the pull request it
    was built from (Windows). This is the counterpart of scripts/vw-feedback.sh;
    see that file's header for what the whole mechanism is for
    (docs/DEVELOPMENT.md「実機フィードバックの往復」).

    NON-INTERACTIVE and machine-readable, exactly like vw-update.ps1: every mode
    prints "key=value" lines (plus a bare "ok" on success) and never shows a
    dialog — the plug-in parses the output and shows its own native Vectorworks
    dialogs. Transient failures are reported as "error=<message>" with exit 0.

      token-status                  ソースと使えるかどうか
      login <token-file>            ファイルのトークンを保存し、ファイルを消す
      logout                        保存したトークンを消す
      find-pr <repo> <branch>       そのブランチの open な PR 番号を引く
      post <repo> <n> <body-file>   PR へコメントを 1 通投稿する
      loop-control <repo> <n> [since]  PR が open か・以後に「止めろ」の合図が付いたか
                                       （モードレスの往復が周期的に呼ぶ。M24）

    **ダイアログはここには無い。** 尋ねるのは全部プラグイン側で、しかも**取り込みが
    始まる前**に済ませる（src/draw/Feedback.h）。ここでダイアログを出すと、プラグインは
    出力を読み終わるまでメインスレッドを止めるので、そのあいだ図面が固まる。

    **トークンをコマンドラインに乗せない。** `login` が受け取るのは*ファイルのパス*で、
    中身は読んだ直後に消す（引数はプロセス一覧から見えるため）。保存は DPAPI
    （ConvertFrom-SecureString）で、**同じ Windows ユーザーだけが復号できる**形にする。

    トークンの探索順:
      1. 環境変数 HOMESKZ_IFC_FEEDBACK_TOKEN
      2. %LOCALAPPDATA%\HomeskzIfcImport\feedback-token.dat（login で保存したもの）
      3. gh CLI の認証（入っていれば）

    Requirements: Windows PowerShell 5.1+ (ships with Windows) or PowerShell 7.

    Overridable via environment:
      VW_REPO                      owner/repo (default below)
      HOMESKZ_IFC_FEEDBACK_TOKEN   トークン（探索順 1）
      VW_FEEDBACK_TOKEN_FILE       保存先の差し替え（試験用）
#>

#requires -version 5
$ErrorActionPreference = 'Stop'

# Prefer TLS 1.2 and emit UTF-8 so the plug-in reads Japanese messages without
# mojibake (same best-effort setup as vw-update.ps1; older hosts may reject it).
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}

$VW_REPO = if ($env:VW_REPO) { $env:VW_REPO } else { 'min-nano/vectorworks-plugin-import-ifc-homeskz' }
$VW_API = 'https://api.github.com'

# **トークンの在り処は同梱の vw-token.ps1 ただ 1 つ**（vw-update.ps1 と共有する。
# CLAUDE.md「重複を作らない置き場所」）。保存先・探索順・DPAPI の出入り口はそちら。
# **隣に置かれる前提**——同じ zip で一緒に配られ、モジュールの隣に並ぶ。
. (Join-Path $PSScriptRoot 'vw-token.ps1')

# ---------------------------------------------------------------------------
# Modes.
# ---------------------------------------------------------------------------

function Invoke-TokenStatus {
    $source = Get-TokenSource
    Write-Output "source=$source"
    if ($source -eq 'none') { Write-Output 'ok=no' } else { Write-Output 'ok=yes' }
}

function Invoke-Login {
    param([string] $TokenFile)

    if (-not $TokenFile -or -not (Test-Path -LiteralPath $TokenFile)) {
        Write-Output 'error=トークンのファイルが見つかりません。'
        return
    }
    $token = (Get-Content -LiteralPath $TokenFile -Raw)
    Remove-Item -LiteralPath $TokenFile -Force -ErrorAction SilentlyContinue
    if ($token) { $token = $token.Trim() }
    if (-not $token) {
        Write-Output 'error=トークンが空です。'
        return
    }
    try {
        $path = Get-TokenFilePath
        $dir = Split-Path -Parent $path
        if ($dir -and -not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        Set-Content -LiteralPath $path -Value (Protect-TokenText -PlainText $token) -NoNewline
        Write-Output 'ok'
    } catch {
        Write-Output 'error=トークンを保存できませんでした。'
    }
}

function Invoke-Logout {
    $path = Get-TokenFilePath
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    Write-Output 'ok'
}

# find-pr <repo> <branch>: そのブランチの open な PR。**番号を人に打たせないため**の口。
function Invoke-FindPr {
    param([string] $Repo, [string] $Branch)

    if (-not $Repo) { $Repo = $VW_REPO }
    if (-not $Branch) {
        Write-Output 'error=ブランチが指定されていません。'
        return
    }
    $owner = $Repo.Split('/')[0]
    $headers = @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'HomeskzIfcImport' }
    $token = Resolve-Token
    if ($token) { $headers['Authorization'] = "Bearer $token" }
    try {
        $url = "$VW_API/repos/$Repo/pulls?state=open&head=$owner`:$Branch"
        $pulls = Invoke-RestMethod -Uri $url -Headers $headers -TimeoutSec 20
    } catch {
        Write-Output 'error=PR を検索できませんでした（ネットワークか権限）。'
        return
    }
    if (-not $pulls -or $pulls.Count -eq 0) {
        Write-Output "error=ブランチ $Branch に open な PR がありません。"
        return
    }
    Write-Output "pr=$($pulls[0].number)"
    if ($pulls[0].title) { Write-Output "title=$($pulls[0].title)" }
    Write-Output 'ok'
}

# post <repo> <n> <body-file>: PR へコメントを 1 通。本文は UTF-8 のまま送る
# （ConvertTo-Json が JSON のエスケープを引き受けるので、自前の文字列連結はしない）。
function Invoke-Post {
    param([string] $Repo, [string] $Number, [string] $BodyFile)

    if (-not $Repo) { $Repo = $VW_REPO }
    if (-not $Number -or -not $BodyFile -or -not (Test-Path -LiteralPath $BodyFile)) {
        Write-Output 'error=引数が不足しています。'
        return
    }
    $token = Resolve-Token
    if (-not $token) {
        Write-Output 'error=GitHub のトークンがありません（先に login してください）。'
        return
    }

    $body = Get-Content -LiteralPath $BodyFile -Raw
    $payload = @{ body = $body } | ConvertTo-Json -Depth 3 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
    $headers = @{
        Accept          = 'application/vnd.github+json'
        Authorization   = "Bearer $token"
        'User-Agent'    = 'HomeskzIfcImport'
    }
    try {
        $result = Invoke-RestMethod -Uri "$VW_API/repos/$Repo/issues/$Number/comments" `
            -Method Post -Headers $headers -ContentType 'application/json; charset=utf-8' `
            -Body $bytes -TimeoutSec 60
    } catch {
        # GitHub の言い分をそのまま渡す（権限不足か PR 違いかが、これで切り分けられる）。
        $reason = $_.Exception.Message
        Write-Output "error=コメントを投稿できませんでした（$reason）。"
        return
    }
    if ($result.html_url) { Write-Output "url=$($result.html_url)" }
    # **投稿した時刻（ISO 8601, UTC）も返す。** 往復の駆動はこれを次の loop-control の
    # since に使い、「自分の投稿より後に付いた合図」だけを読む。
    if ($result.created_at) { Write-Output "created=$(Get-IsoTime $result.created_at)" }
    Write-Output 'ok'
}

# created_at は Invoke-RestMethod が DateTime へ変換してしまうことがあるので、文字列でも
# DateTime でも GitHub の since が受け取る形（UTC の ISO 8601）へそろえる。
function Get-IsoTime {
    param($Value)
    if ($Value -is [DateTime]) { return $Value.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
    return [string] $Value
}

# コメント本文から**往復への合図**を読む。合図は
#   <!-- homeskz-ifc-feedback v1 control=stop -->
# **ただ 1 行**で、その行に他の文字があってはならない（scripts/vw-feedback.sh の
# control_of_comment と同じ規則。理由もそちらに書いてある——「書いてある」と「合図である」は
# 違い、本文のどこかで拾う作りでは**目印を説明した文章が合図になる**。実機 round 1 で
# 実際に起きた。docs/DEV-NOTES.md M24「合図は行、文中の引用ではない」）。
function Get-CommentControl {
    param([string] $Body)
    if (-not $Body) { return 'none' }

    $fence = $false
    $self = $false
    $stop = $false
    foreach ($raw in ($Body -split "`r?`n")) {
        if ($raw -match '^\s*```') { $fence = -not $fence; continue }
        if ($fence) { continue }
        $line = $raw.Trim()
        if ($line -match '^<!--\s*homeskz-ifc-feedback\s+v1\s+round=') { $self = $true }
        if ($line -match '^<!--\s*homeskz-ifc-feedback\s+v1\s+control=ended') { $self = $true }
        if ($line -match '^<!--\s*homeskz-ifc-feedback\s+v1\s+control=stop\s*-->$') { $stop = $true }
    }
    if ($stop -and -not $self) { return 'stop' }
    return 'none'
}

# loop-control <repo> <n> [since]: **往復を続けてよいか**（M24。モードレスの往復が周期的に
# 呼ぶ）。答えは 2 つ:
#   state=<open|closed|merged>   PR の状態（閉じたら続ける相手がいない）
#   control=<stop|none>          since 以降のコメントに「止めろ」の合図があるか
#   ok
# since は ISO 8601（post が返した created=）。無ければ最近のコメント（上限 5 ページ）から読む。
function Invoke-LoopControl {
    param([string] $Repo, [string] $Number, [string] $Since)

    if (-not $Repo) { $Repo = $VW_REPO }
    if (-not $Number) {
        Write-Output 'error=PR 番号が指定されていません。'
        return
    }
    $headers = @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'HomeskzIfcImport' }
    $token = Resolve-Token
    if ($token) { $headers['Authorization'] = "Bearer $token" }

    try {
        $pull = Invoke-RestMethod -Uri "$VW_API/repos/$Repo/pulls/$Number" -Headers $headers -TimeoutSec 20
    } catch {
        Write-Output 'error=PR の状態を取得できませんでした（ネットワークか権限）。'
        return
    }
    if (-not $pull -or -not $pull.state) {
        Write-Output 'error=PR の状態を読めませんでした。'
        return
    }
    $state = [string] $pull.state
    if ($pull.merged -eq $true) { $state = 'merged' }

    $control = 'none'
    $maxPage = if ($Since) { 2 } else { 5 }
    for ($page = 1; $page -le $maxPage; $page++) {
        $url = "$VW_API/repos/$Repo/issues/$Number/comments?per_page=100&page=$page"
        if ($Since) { $url += "&since=$Since" }
        try {
            $comments = @(Invoke-RestMethod -Uri $url -Headers $headers -TimeoutSec 20)
        } catch {
            Write-Output 'error=PR のコメントを取得できませんでした（ネットワークか権限）。'
            return
        }
        foreach ($comment in $comments) {
            if ((Get-CommentControl -Body ([string] $comment.body)) -eq 'stop') { $control = 'stop' }
        }
        if ($comments.Count -lt 100) { break }
    }

    Write-Output "state=$state"
    Write-Output "control=$control"
    Write-Output 'ok'
}

# ---------------------------------------------------------------------------
# 引数を 1 つ取り出す（無ければ空文字）。範囲外の添字で落ちないようにするだけの道具。
function Get-Argument {
    param([string[]] $Arguments, [int] $Index)
    if ($null -eq $Arguments -or $Index -ge $Arguments.Count) { return '' }
    return [string] $Arguments[$Index]
}

function Invoke-Main {
    param([string[]] $Arguments)

    # 足りない引数は空文字にする（各モードが「引数が不足しています」を返せるように)。
    $mode = Get-Argument $Arguments 0
    switch ($mode) {
        'token-status' { Invoke-TokenStatus }
        'login'        { Invoke-Login -TokenFile (Get-Argument $Arguments 1) }
        'logout'       { Invoke-Logout }
        'find-pr'      { Invoke-FindPr -Repo (Get-Argument $Arguments 1) -Branch (Get-Argument $Arguments 2) }
        'post'         {
            Invoke-Post -Repo (Get-Argument $Arguments 1) -Number (Get-Argument $Arguments 2) `
                -BodyFile (Get-Argument $Arguments 3)
        }
        'loop-control' {
            Invoke-LoopControl -Repo (Get-Argument $Arguments 1) -Number (Get-Argument $Arguments 2) `
                -Since (Get-Argument $Arguments 3)
        }
        default        { Write-Output "error=不明なモード: '$mode'（token-status / login / logout / find-pr / post / loop-control）。" }
    }
}

# Run only when EXECUTED, not when dot-sourced — the Pester tests
# (tests/vw-feedback.Tests.ps1) dot-source this file to drive the modes with the
# network and the token store stubbed out, exactly as tests/vw-update.Tests.ps1 does.
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Main -Arguments $args
}
