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
      ask-note <repo> <n> <round> <build> [<url>] [yes|no]
                                    **所見を尋ねて投稿する**。すぐ返り、ダイアログは
                                    別プロセスに残る

    **ask-note が「待たない」のが肝。** プラグインは同梱スクリプトの出力を読み終わるまで
    Vectorworks のメインスレッドを止めるので、ここでダイアログを出して待つと図面が固まって
    見られない——所見を書くために絵を見たい、という当の目的が果たせない。そこで ask-note は
    **自分自身を別プロセスで起こし直して即座に `ok` を返す**（scripts/vw-feedback.sh の
    同名モードと同じ作法）。

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

# 保存先のフォルダ名は**識別子なので据え置く**。プラグインの表示名やファイル名が変わっても
# 付け替えない——付け替えた瞬間、既に入っているトークンが行方不明になり、利用者にもう一度
# 貼り付けさせることになる（コマンドの UUID を据え置くのと同じ理由）。
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
    # PSAvoidUsingConvertToSecureStringWithPlainText は「平文から SecureString を作るな」
    # という規則だが、ここでの SecureString は**保管を暗号化するための通り道**であって、
    # 平文をメモリから隠すためのものではない（トークンは呼び出し元が平文で持っている）。
    # ConvertFrom-SecureString は DPAPI で暗号化した文字列を返すので、**保存したユーザー
    # 本人しか復号できないファイル**になる——それがここで欲しい唯一の性質である。
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute(
        'PSAvoidUsingConvertToSecureStringWithPlainText', '',
        Justification = 'SecureString is only the route to DPAPI-at-rest; the token is already plaintext here.')]
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
# 所見を尋ねる（ask-note）。**別プロセスのダイアログ**なので、開いている間も
# Vectorworks は動かせる（冒頭「ask-note が『待たない』のが肝」）。
# ---------------------------------------------------------------------------

# Get-Note: 所見を 1 つ尋ねる。取り消し／空欄はどちらも $null（Windows の InputBox は
# その 2 つを区別しないが、どちらも「所見なし」なので困らない）。
function Get-Note {
    param([string] $Title, [string] $Prompt)
    try {
        Add-Type -AssemblyName Microsoft.VisualBasic -ErrorAction Stop
        $text = [Microsoft.VisualBasic.Interaction]::InputBox($Prompt, $Title, '')
        if ([string]::IsNullOrWhiteSpace($text)) { return $null }
        return $text
    } catch {
        return $null
    }
}

# Show-NoteAlert: 伝えないと黙って消えてしまうことだけを出す（投稿の失敗）。
function Show-NoteAlert {
    param([string] $Title, [string] $Message)
    try {
        Add-Type -AssemblyName Microsoft.VisualBasic -ErrorAction Stop
        [Microsoft.VisualBasic.Interaction]::MsgBox($Message, 0, $Title) | Out-Null
    } catch {
        # 出せなくても続ける（付随の通知）。
    }
}

function Open-Url {
    param([string] $Url)
    if (-not $Url) { return }
    try { Start-Process $Url | Out-Null } catch {
        # ブラウザを開けなくても投稿は済んでいる。
    }
}

# Invoke-SelfDetached: 自分自身を別プロセスで起こす。**テストが差し替える唯一の口**で、
# scripts/vw-feedback.sh の spawn_self と対になる。
function Invoke-SelfDetached {
    param([string[]] $ScriptArguments)
    $self = $PSCommandPath
    Start-Process -FilePath 'powershell' -WindowStyle Hidden -ArgumentList (@(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $self) + $ScriptArguments) | Out-Null
}

# ask-note: **すぐ返る。** 自分自身を別プロセスで起こし直し、'ok' を出して終わる。
function Invoke-AskNote {
    param([string] $Repo, [string] $Number, [string] $Round, [string] $Build, [string] $Url,
        [string] $Posted = 'yes')

    if (-not $Number) {
        Write-Output 'error=引数が不足しています。'
        return
    }
    if (-not $Posted) { $Posted = 'yes' }
    try {
        Invoke-SelfDetached -ScriptArguments @(
            'ask-note-worker', $Repo, $Number, $Round, $Build, $Url, $Posted)
        Write-Output 'ok'
    } catch {
        Write-Output 'error=所見のダイアログを起動できませんでした。'
    }
}

# ask-note-worker: 別プロセス側の本体。**プラグインはもう戻っている**ので、尋ねてから
# 投稿するところまでここでやり切る。所見は独立した 1 通として投稿する（取り込み結果の
# 本文へ差し込まない——差し込むと組み立てが C++ 側とここの 2 か所に割れる）。
function Invoke-AskNoteWorker {
    param([string] $Repo, [string] $Number, [string] $Round, [string] $Build, [string] $Url,
        [string] $Posted = 'yes')

    if (-not $Posted) { $Posted = 'yes' }

    # **結果を投稿した周と、しなかった周で言うことが違う。** 投稿しなかった周は、この所見が
    # その周について PR に載る**唯一のもの**になる——だからそう書いて、書く気になってもらう。
    if ($Posted -eq 'no') {
        $prompt = "round $Round の結果は投稿しませんでした。`n" +
            "伝えたいことがあれば書いてください（これがこの周の唯一の記録になります）。`n" +
            "空のままなら、何も投稿せずに終わります。`n" +
            "（OK で所見を送り、キャンセルなら書かずに閉じます。）`n" +
            '※ 往復は続きます（終わるのは次の取り込みの確認で「往復を終える」を押したときです）。'
        $suffix = '・結果は未投稿'
    } else {
        $prompt = "round $Round を投稿しました。`n" +
            "図面を確かめて、気付いたことがあれば書いてください（空のままなら所見なしで終わります）。`n" +
            "（OK で所見を送り、キャンセルなら書かずに閉じます。）`n" +
            '※ 往復は続きます（終わるのは次の取り込みの確認で「往復を終える」を押したときです）。'
        $suffix = ''
    }

    $note = Get-Note -Title "実機フィードバック round $Round" -Prompt $prompt
    if (-not $note) {
        Open-Url $Url
        return
    }

    $quoted = ($note -split "`r?`n" | ForEach-Object { "> $_" }) -join "`n"
    $body = "<!-- homeskz-ifc-feedback-note v1 round=$Round build=$Build posted=$Posted -->`n" +
        "### 実機を見ての所見（round $Round$suffix）`n`n" + $quoted + "`n"

    $file = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
    Set-Content -LiteralPath $file -Value $body -NoNewline
    $out = (Invoke-Post -Repo $Repo -Number $Number -BodyFile $file) -join "`n"
    Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue

    $reason = ($out -split "`n" | Where-Object { $_ -like 'error=*' } | Select-Object -First 1)
    if ($reason) {
        Show-NoteAlert '実機フィードバック' ("所見を投稿できませんでした（" + $reason.Substring(6) + "）")
        return
    }
    Open-Url $Url
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
        'ask-note'     {
            Invoke-AskNote -Repo (Get-Argument $Arguments 1) -Number (Get-Argument $Arguments 2) `
                -Round (Get-Argument $Arguments 3) -Build (Get-Argument $Arguments 4) `
                -Url (Get-Argument $Arguments 5) -Posted (Get-Argument $Arguments 6)
        }
        # 内部用（ask-note が自分を起こし直すときの入口。人が直接呼ぶものではない）。
        'ask-note-worker' {
            Invoke-AskNoteWorker -Repo (Get-Argument $Arguments 1) `
                -Number (Get-Argument $Arguments 2) -Round (Get-Argument $Arguments 3) `
                -Build (Get-Argument $Arguments 4) -Url (Get-Argument $Arguments 5) `
                -Posted (Get-Argument $Arguments 6)
        }
        default        { Write-Output "error=不明なモード: '$mode'（token-status / login / logout / find-pr / post / ask-note）。" }
    }
}

# Run only when EXECUTED, not when dot-sourced — the Pester tests
# (tests/vw-feedback.Tests.ps1) dot-source this file to drive the modes with the
# network and the token store stubbed out, exactly as tests/vw-update.Tests.ps1 does.
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Main -Arguments $args
}
