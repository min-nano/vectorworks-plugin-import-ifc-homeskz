<#
    vw-install.ps1 — install ONE build of the HomeskzIfcImport Vectorworks plug-in
    into a Vectorworks 2026 Plug-Ins folder (Windows).

    This is the Windows counterpart of scripts/vw-install.sh; see that file's
    header for the full rationale. In short:

    **このスクリプトは「配置の手順」そのもので、リリースと一緒に配られる。**
    配布 zip の直下に同梱され、リリースのアセットとしても単独で公開される。走るのは
    常に**インストール済みの（＝古い）** vw-update.ps1 なので、配置の知識をそちらに
    置くと「新しいビルドがどんなファイルでできているか」を永遠に知らないままになる
    ——実際 M21 で本体（.vwpayload）が増えたとき、古いアップデータはそれを写さず、
    利用者は zip を手で落として置き直す羽目になった。そこで vw-update.ps1 は**落とした
    zip の中のこのスクリプトへ配置を委ねる**。

    配置の規則はひとつ: **zip の直下にあるものを、そのまま Plug-Ins へ置く。**
    ファイル名を列挙しない——列挙した瞬間に「増えたファイルを取りこぼす」という、いま
    直している事故がそっくり戻ってくる。除くのはこのスクリプト自身だけ。

    Usage:
      powershell -ExecutionPolicy Bypass -File vw-install.ps1                     # stable
      powershell -ExecutionPolicy Bypass -File vw-install.ps1 -Tag dev-feature-x
      powershell -ExecutionPolicy Bypass -File vw-install.ps1 -Zip <file>
      powershell -ExecutionPolicy Bypass -File vw-install.ps1 -From <dir> -Machine

    Options:
      -Name <plugin>      HomeskzIfcImport / HomeskzIfcImportDev（既定は自動判定）
      -PluginsDir <dir>   インストール先（既定は VW_PLUGINS_DIR、無ければ VW2026 の
                          ユーザフォルダ）
      -From <dir>         展開済みのディレクトリから入れる（ダウンロードしない）
      -Zip <file>         手元の zip から入れる
      -Url <url>          この zip を落として入れる
      -Tag <tag>          このリリース（既定 "stable"）から落として入れる
      -Machine            機械可読な出力にする（installed-shell= / ok / error=）。
                          プラグイン側の vw-update.ps1 do-install が使う
      -Help               使い方

    **知らないオプションは黙って読み飛ばす。** 新しい vw-update.ps1 が古いリリースの zip
    に入ったこのスクリプトを呼ぶ、という向きが起こりうるので、増えた引数で落ちないように
    しておく（そのために param() ブロックを使わず $args を自分で読む——param() だと未知の
    名前付き引数が束縛エラーになる）。

    Requirements: Windows PowerShell 5.1+ (ships with Windows) or PowerShell 7.
    Uses only built-in cmdlets — no extra tools, and because the repository is
    public, no authentication.

    Overridable via environment:
      VW_REPO         owner/repo             (default below)
      VW_PLUGINS_DIR  Vectorworks Plug-Ins   (default: user folder for VW 2026)
#>

#requires -version 5
$ErrorActionPreference = 'Stop'

# Prefer TLS 1.2 (older Windows PowerShell defaults can be lower) and emit UTF-8
# so the plug-in reads our Japanese messages without mojibake. Both are optional
# niceties older hosts may reject, hence the empty catches.
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}

$VW_REPO = if ($env:VW_REPO) { $env:VW_REPO } else { 'min-nano/vectorworks-plugin-import-ifc-homeskz' }
$VW_API = "https://api.github.com/repos/$VW_REPO"

# 殻の拡張子（Windows のプラグインモジュールは "<name>.vlb" という DLL）。zip の
# 取り違えを弾くのと、アーカイブからプラグイン名を読み取るのに使う。
$VW_SHELL_EXT = '.vlb'
# 配布 zip のアセット名の末尾。**プラグイン名が変わっても効く**ようにアセット名そのもの
# ではなく末尾で照合する。
$VW_ZIP_SUFFIX = '.vlb.zip'
# Plug-Ins へは置かないもの（インストーラ自身。プラグインの一部ではない）。
$VW_NOT_INSTALLED = @('vw-install.ps1', 'vw-install.sh', '__MACOSX')

$script:LastError = ''
$script:Machine = $false
$script:ExitCode = 0
$script:PluginsDir = if ($env:VW_PLUGINS_DIR) { $env:VW_PLUGINS_DIR } else { Join-Path $env:APPDATA 'Nemetschek\Vectorworks\2026\Plug-Ins' }

# 人が実行したときだけ進捗を出す（machine モードは機械可読な行しか出さない）。
function Write-Note([string] $message) {
    if (-not $script:Machine) { Write-Host $message }
}

function Show-Usage {
    Write-Host 'vw-install.ps1 — HomeskzIfcImport を Vectorworks 2026 の Plug-Ins へ入れる'
    Write-Host '  -Name <plugin>     HomeskzIfcImport / HomeskzIfcImportDev（既定は自動判定）'
    Write-Host '  -PluginsDir <dir>  インストール先（既定: VW2026 のユーザフォルダ）'
    Write-Host '  -From <dir>        展開済みのディレクトリから入れる'
    Write-Host '  -Zip <file>        手元の zip から入れる'
    Write-Host '  -Url <url>         この zip を落として入れる'
    Write-Host '  -Tag <tag>         このリリースから落として入れる（既定: stable）'
    Write-Host '  -Machine           機械可読な出力（installed-shell= / ok / error=）'
}

# ---------------------------------------------------------------------------
# GitHub REST helpers. **vw-update.ps1 と同じものが写っているのは意図的**で、この
# ファイルは**単独で配られて単独で走る**（リリースから落としてきた 1 枚だけが手元に
# ある）から、他のスクリプトをドットソースできない。
# ---------------------------------------------------------------------------
function Invoke-GH([string] $subpath) {
    return Invoke-RestMethod -Uri "$VW_API/$subpath" `
        -Headers @{ 'Accept' = 'application/vnd.github+json' } `
        -UserAgent 'vw-install' -TimeoutSec 20 -Method Get
}

# リリースの JSON から配布 zip を 1 つ選ぶ。`want` が与えられていればその名前で厳密に、
# そうでなければ**末尾が $VW_ZIP_SUFFIX のアセット**を拾う（アセット名が将来変わっても
# 追随できる）。戻りは @{ Url; Name } か $null。
function Get-ReleaseZip($release, [string] $want) {
    foreach ($a in $release.assets) {
        if ($want) {
            if ($a.name -eq ($want + $VW_ZIP_SUFFIX)) {
                return [pscustomobject]@{ Url = $a.browser_download_url; Name = $want }
            }
        }
        elseif ($a.name -and $a.name.EndsWith($VW_ZIP_SUFFIX)) {
            $n = $a.name.Substring(0, $a.name.Length - $VW_ZIP_SUFFIX.Length)
            return [pscustomobject]@{ Url = $a.browser_download_url; Name = $n }
        }
    }
    return $null
}

# 展開済みディレクトリから殻を探し、その名前をプラグイン名として返す（-Name 省略時）。
# -Filter ではなく Where-Object で絞るのは、Windows の -Filter が 8.3 名まで見て
# "*.vlb" が ".vlb 以外で始まる長い拡張子" にも当たることがあるため。
function Get-PluginName([string] $dir) {
    $hit = Get-ChildItem -LiteralPath $dir -ErrorAction SilentlyContinue |
        Where-Object { $_.Name.EndsWith($VW_SHELL_EXT) } | Select-Object -First 1
    if ($hit) { return $hit.BaseName }
    return ''
}

# 殻の ID（ビルドが .vlb の隣へ置く "<name>.shell-id"）。**「アップデートに Vectorworks の
# 再起動が要るか」を決める鍵**で、プラグインは自分にコンパイルされた VW_SHELL_ID と
# 突き合わせる——一致するなら本体（.vwpayload）を読み直すだけで反映される
# （src/PayloadAbi.h / src/UpdaterParse.h）。読めなければ空＝プラグインは安全側
# （再起動が要る）へ倒す。
function Get-InstalledShellId([string] $name) {
    $f = Join-Path $script:PluginsDir "$name.shell-id"
    if (Test-Path -LiteralPath $f) {
        $c = Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue
        if ($c) { return $c.Trim() }
    }
    return ''
}

# ---------------------------------------------------------------------------
# 配置。**このスクリプトの本体で、ここだけが「プラグインがどんなファイルでできているか」を
# 知っている。**
# ---------------------------------------------------------------------------

# Install-File: $src を $dst へ置く。**読み込み中の .vlb は削除できないが、退かすことは
# できる**ので、既にあるものは best-effort で改名して退かしてから写す。残った ".old-*" は
# 次のインストールで（Vectorworks が手を離したあとに）掃く。ロック中のファイルが消せない
# のは想定内なので、catch は空でよい。
function Install-File([string] $src, [string] $dst) {
    if (Test-Path -LiteralPath $dst) {
        $bak = "$([System.IO.Path]::GetFileName($dst)).old-$([System.IO.Path]::GetRandomFileName())"
        try { Rename-Item -LiteralPath $dst -NewName $bak -ErrorAction Stop }
        catch { try { Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction Stop } catch {} }
    }
    Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
}

# Install-Tree: 展開済みディレクトリの直下にあるものを、そのまま Plug-Ins へ置く。
# **列挙しない**のが肝（冒頭のコメント参照）。
function Install-Tree([string] $work, [string] $name) {
    if (-not (Test-Path -LiteralPath (Join-Path $work "$name$VW_SHELL_EXT"))) {
        $script:LastError = "$name$VW_SHELL_EXT がアーカイブ内に見つかりません。"
        return $false
    }
    if (-not (Test-Path -LiteralPath $script:PluginsDir)) {
        New-Item -ItemType Directory -Force -Path $script:PluginsDir | Out-Null
    }

    # 前回のインストールが残した退避（Vectorworks が手を離した今なら消せる）を掃く。
    Get-ChildItem -LiteralPath $script:PluginsDir -Filter '*.old-*' -ErrorAction SilentlyContinue |
        ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction Stop } catch {} }

    foreach ($item in Get-ChildItem -LiteralPath $work -Force -ErrorAction SilentlyContinue) {
        if ($VW_NOT_INSTALLED -contains $item.Name) { continue }
        try { Install-File $item.FullName (Join-Path $script:PluginsDir $item.Name) }
        catch {
            $script:LastError = "インストール先へのコピーに失敗しました（$($item.Name)）。"
            return $false
        }
        Write-Note "  置きました: $($item.Name)"
    }
    return $true
}

# ---------------------------------------------------------------------------
# 取ってくる → 展開する。戻りは @{ Dir; Name } か $null（理由は $script:LastError）。
# ---------------------------------------------------------------------------
function Resolve-Source([hashtable] $opt, [string] $tmp) {
    $name = $opt.Name

    # 1) 展開済みを渡された（アップデータからの呼び出し）。
    if ($opt.From) {
        if (-not (Test-Path -LiteralPath $opt.From)) {
            $script:LastError = "指定されたディレクトリがありません: $($opt.From)"
            return $null
        }
        if (-not $name) { $name = Get-PluginName $opt.From }
        if (-not $name) { $script:LastError = 'アーカイブ内にプラグインが見つかりません。'; return $null }
        return [pscustomobject]@{ Dir = $opt.From; Name = $name }
    }

    # 2) zip / URL / リリースのいずれかから zip を得る。
    $zip = $opt.Zip
    if (-not $zip) {
        $url = $opt.Url
        if (-not $url) {
            $tag = if ($opt.Tag) { $opt.Tag } else { 'stable' }
            try { $rel = Invoke-GH "releases/tags/$tag" }
            catch { $script:LastError = "リリース '$tag' を取得できませんでした。"; return $null }
            $found = Get-ReleaseZip $rel $name
            if (-not $found) { $script:LastError = "リリース '$tag' に配布 zip が見つかりません。"; return $null }
            $url = $found.Url
            if (-not $name) { $name = $found.Name }
            Write-Note "リリース '$tag' から $name を取得します。"
        }
        $zip = Join-Path $tmp 'download.zip'
        try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 300 }
        catch { $script:LastError = 'ダウンロードに失敗しました。'; return $null }
    }

    $work = Join-Path $tmp 'unpacked'
    try { Expand-Archive -LiteralPath $zip -DestinationPath $work -Force }
    catch { $script:LastError = 'アーカイブの展開に失敗しました。'; return $null }

    if (-not $name) { $name = Get-PluginName $work }
    if (-not $name) { $script:LastError = 'アーカイブ内にプラグインが見つかりません。'; return $null }
    return [pscustomobject]@{ Dir = $work; Name = $name }
}

# 引数を読む。**知らないオプションは読み飛ばす**（冒頭のコメント参照）。値らしき次の
# 引数も一緒に捨てる。
function Read-Option([string[]] $argv) {
    $opt = @{ Name = ''; From = ''; Zip = ''; Url = ''; Tag = ''; Help = $false }
    if (-not $argv) { return $opt }
    $i = 0
    while ($i -lt $argv.Count) {
        $a = [string] $argv[$i]
        $next = if ($i + 1 -lt $argv.Count) { [string] $argv[$i + 1] } else { '' }
        # switch -regex は**一致した節をすべて**実行するので、各節を break で閉じる
        # （閉じないと "-machine" が最後の "^-" にも掛かって二重に走る）。
        switch -regex ($a) {
            '^-(?i:machine)$' { $script:Machine = $true; break }
            '^-(?i:name)$' { $opt.Name = $next; $i++; break }
            '^-(?i:pluginsdir)$' { $script:PluginsDir = $next; $i++; break }
            '^-(?i:from)$' { $opt.From = $next; $i++; break }
            '^-(?i:zip)$' { $opt.Zip = $next; $i++; break }
            '^-(?i:url)$' { $opt.Url = $next; $i++; break }
            '^-(?i:tag)$' { $opt.Tag = $next; $i++; break }
            '^-(?i:h|help|\?)$' { $opt.Help = $true; break }
            '^-' {
                # 知らないオプション。次が値らしければそれも捨てる。
                if ($next -and -not $next.StartsWith('-')) { $i++ }
                break
            }
            default { break } # 位置引数は使わない
        }
        $i++
    }
    return $opt
}

# ---------------------------------------------------------------------------
# 結末は **$script:ExitCode** で返す。戻り値（return）にすると、機械可読な行を出す
# Write-Output と同じ「出力ストリーム」に混ざり、呼び出し側が受け取る／飲み込むという
# 事故になる（PowerShell の関数は return した値も出力の一部）。
function Invoke-Main([string[]] $argv) {
    $script:ExitCode = 0
    $opt = Read-Option $argv
    if ($opt.Help) { Show-Usage; return }

    $tmpPath = Join-Path ([System.IO.Path]::GetTempPath()) ('vwinst-' + [System.IO.Path]::GetRandomFileName())
    $tmp = New-Item -ItemType Directory -Force -Path $tmpPath
    $ok = $false
    $name = ''
    try {
        $src = Resolve-Source $opt $tmp.FullName
        if ($src) {
            $name = $src.Name
            $ok = Install-Tree $src.Dir $name
        }
    }
    finally {
        try { Remove-Item -LiteralPath $tmp.FullName -Recurse -Force -ErrorAction SilentlyContinue } catch {}
    }

    if ($script:Machine) {
        # 機械可読な結末。**プラグインはこの行しか読まない**（src/UpdaterParse.h）。
        # 判断できない情報は行ごと出さない。
        if ($ok) {
            $shell = Get-InstalledShellId $name
            if ($shell) { Write-Output "installed-shell=$shell" }
            Write-Output 'ok'
        }
        else {
            $e = if ($script:LastError) { $script:LastError } else { 'インストールに失敗しました。' }
            Write-Output "error=$e"
        }
        # machine モードは常に 0 で終わる（結末は stdout の行が持つ）。
        return
    }

    if ($ok) {
        Write-Host "インストールしました: $script:PluginsDir" -ForegroundColor Green
        Write-Host 'Vectorworks を起動してください（起動中なら再起動してください）。'
        return
    }
    $e = if ($script:LastError) { $script:LastError } else { 'インストールに失敗しました。' }
    Write-Host "エラー: $e" -ForegroundColor Red
    $script:ExitCode = 1
}

# 直接実行したときだけ走らせる（テストはドットソースして個々の関数を叩く。
# scripts/vw-update.ps1 の末尾と同じ作法）。
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Main $args
    exit $script:ExitCode
}
