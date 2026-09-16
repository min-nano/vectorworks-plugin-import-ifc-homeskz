<#
    vw-token.ps1 — GitHub のトークンの在り処（Windows）。**同梱スクリプトが共有する
    唯一の実装**で、保存先・探索順・DPAPI の出入り口はここ 1 か所にある
    （CLAUDE.md「重複を作らない置き場所」。macOS の対は scripts/vw-token.sh）。

    使う側が 2 つある:

      vw-feedback.ps1  PR を読む／コメントを投稿する（トークンが無ければ投稿できない）
      vw-update.ps1    リリース一覧・stable リリースを読む（**トークンは要らないが、
                       認証なしの GitHub API は IP ごとに 1 時間 60 回**。往復の
                       パレットは 1 分ごとに見に行く＝ちょうど上限なので、何か 1 回でも
                       挟まれば超える）

    **このファイルは dot-source される**（実行しない）。トークンを標準出力へ出すモードは
    持たない——呼び出し元が内部で Resolve-Token を呼び、要求のヘッダへ直接渡すこと。

    トークンの探索順:
      1. 環境変数 HOMESKZ_IFC_FEEDBACK_TOKEN
      2. %LOCALAPPDATA%\HomeskzIfcImport\feedback-token.dat（login で保存したもの）
      3. gh CLI の認証（入っていれば）

    Overridable via environment:
      HOMESKZ_IFC_FEEDBACK_TOKEN   トークン（探索順 1）
      VW_FEEDBACK_TOKEN_FILE       保存先の差し替え（試験用）
#>

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
