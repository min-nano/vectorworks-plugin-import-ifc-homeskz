<#
    vw-token.ps1 — GitHub のトークンの在り処（Windows）。**同梱スクリプトが共有する
    唯一の実装**で、保存先・探索順・DPAPI の出入り口はここ 1 か所にある
    （CLAUDE.md「重複を作らない置き場所」。macOS の対は scripts/vw-token.sh）。

    使う側は vw-update.ps1（リリース一覧・stable リリースを読む）。**トークンは要らないが、
    認証なしの GitHub API は IP ごとに 1 時間 60 回**で、M24〜M37 の往復のパレットが
    1 分ごとに確認しに行ってちょうど上限に達した（M27）。M37 まではもう 1 つの使い手
    （vw-feedback.ps1。PR への投稿）が login で保存していたので、保存先を読む口は残してある。

    **このファイルは dot-source される**（実行しない）。トークンを標準出力へ出すモードは
    持たない——呼び出し元が内部で Resolve-Token を呼び、要求のヘッダへ直接渡すこと。

    トークンの探索順:
      1. 環境変数 HOMESKZ_IFC_FEEDBACK_TOKEN
      2. %LOCALAPPDATA%\HomeskzIfcImport\feedback-token.dat（M37 までの login で保存したもの）
      3. gh CLI の認証（入っていれば）

    Overridable via environment:
      HOMESKZ_IFC_FEEDBACK_TOKEN   トークン（探索順 1）
      VW_FEEDBACK_TOKEN_FILE       保存先の差し替え（試験用）
#>

# 保存先のフォルダ名は**識別子なので据え置く**。プラグインの表示名やファイル名が変わっても
# 付け替えない——付け替えた瞬間、既に入っているトークンが参照できなくなり、利用者にもう一度
# 貼り付けさせることになる（コマンドの UUID を据え置くのと同じ理由）。
function Get-TokenFilePath {
    if ($env:VW_FEEDBACK_TOKEN_FILE) { return $env:VW_FEEDBACK_TOKEN_FILE }
    $dir = Join-Path $env:LOCALAPPDATA 'HomeskzIfcImport'
    return (Join-Path $dir 'feedback-token.dat')
}

# 保存の復号。保存は **DPAPI（ConvertFrom-SecureString の既定）** で暗号化されているので、
# 復号できるのは保存した Windows ユーザー本人だけ。
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

function Resolve-Token {
    if ($env:HOMESKZ_IFC_FEEDBACK_TOKEN) { return $env:HOMESKZ_IFC_FEEDBACK_TOKEN }
    $stored = Get-StoredToken
    if ($stored) { return $stored }
    return (Get-GhToken)
}
