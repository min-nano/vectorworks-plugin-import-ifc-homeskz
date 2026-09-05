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

function Get-TokenFilePath {
    if ($env:VW_FEEDBACK_TOKEN_FILE) { return $env:VW_FEEDBACK_TOKEN_FILE }
    $dir = Join-Path $env:LOCALAPPDATA 'HomeskzIfcImport'
    return (Join-Path $dir 'feedback-token.dat')
}

# 保存の暗号化。**DPAPI（ConvertFrom-SecureString の既定）を使う**ので、復号できるのは
# 保存した Windows ユーザー本人だけ——他人のプロファイルへファイルを持ち出しても読めない。
#
# この 2 つを関数に切ってあるのは、**単体テストが Linux の pwsh でも走るようにするため**
# （DPAPI は Windows にしか無い）。テストはここだけを差し替え、login / logout / 探索順
# といった本当のロジックは実物のまま走らせる（tests/vw-feedback.Tests.ps1）。
function Protect-TokenText {
    param([string] $PlainText)
    $secure = ConvertTo-SecureString -String $PlainText -AsPlainText -Force
    return (ConvertFrom-SecureString -SecureString $secure)
}

function Unprotect-TokenText {
    param([string] $Protected)
    $secure = ConvertTo-SecureString -String $Protected
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

# 保存したトークン（無ければ $null）。
function Get-StoredToken {
    $path = Get-TokenFilePath
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    try {
        $encrypted = Get-Content -LiteralPath $path -Raw
        if (-not $encrypted) { return $null }
        return (Unprotect-TokenText -Protected $encrypted.Trim())
    } catch {
        return $null
    }
}

function Get-GhToken {
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $gh) { return $null }
    try {
        $token = & $gh.Source auth token 2>$null
        if ($LASTEXITCODE -ne 0) { return $null }
        if ($token) { return ([string]$token).Trim() }
    } catch {
        return $null
    }
    return $null
}

# どこから取れるか（取れなければ 'none'）。**トークン自体は返さない。**
function Get-TokenSource {
    if ($env:HOMESKZ_IFC_FEEDBACK_TOKEN) { return 'env' }
    if (Get-StoredToken) { return 'stored' }
    if (Get-GhToken) { return 'gh' }
    return 'none'
}

function Resolve-Token {
    if ($env:HOMESKZ_IFC_FEEDBACK_TOKEN) { return $env:HOMESKZ_IFC_FEEDBACK_TOKEN }
    $stored = Get-StoredToken
    if ($stored) { return $stored }
    return (Get-GhToken)
}

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
        default        { Write-Output "error=不明なモード: '$mode'（token-status / login / logout / find-pr / post）。" }
    }
}

# Run only when EXECUTED, not when dot-sourced — the Pester tests
# (tests/vw-feedback.Tests.ps1) dot-source this file to drive the modes with the
# network and the token store stubbed out, exactly as tests/vw-update.Tests.ps1 does.
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Main -Arguments $args
}
