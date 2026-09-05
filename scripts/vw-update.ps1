<#
    vw-update.ps1 — download the latest CI build of the HomeskzIfcImport Vectorworks
    plug-in and install it into your Vectorworks 2026 Plug-Ins folder (Windows).

    This is the Windows counterpart of scripts/vw-update.sh. On Windows a
    Vectorworks plug-in module is a "<name>.vlb" file (a DLL) with a sibling
    "<name>.vwr" resource, so this script installs those flat files rather than a
    macOS ".vwlibrary" bundle.

    Two channels, two separately-named plug-ins that can be installed at once:

      stable  -> "HomeskzIfcImport.vlb"     from the rolling "stable" release (main).
      dev     -> "HomeskzIfcImportDev.vlb"  from a per-branch "dev-<branch>" prerelease;
                 you pick which branch's build to install.

    The plug-in itself drives its own updates by invoking this same script (it is
    installed next to the .vlb, see src/Updater.cpp). The plug-in shows all of its
    own NATIVE Vectorworks dialogs, so this script exposes a small NON-INTERACTIVE,
    machine-readable back end for it — no dialogs of its own in those modes:

      q-stable            Print the stable channel status as key=value lines:
                          installed=<commit|none> / latest=<commit> / url=<zip url>
                          (or error=<message>).
      q-dev               Print installed=<commit|none> then one TSV line per dev
                          build: "build<TAB>commit<TAB>name<TAB>url"
                          (or error=<message>).
      do-install <url> <name>   Download+install "<name>.vlb"; print "ok" or
                                error=<message>. No dialogs.

    The interactive stable/dev modes are the manual, run-from-a-terminal fallback
    and prompt on the console.

    **ファイルの配置はこのスクリプトが決めない。** 走るのは常に**インストール済みの
    （＝古い）**この 1 本なので、ここに配置手順を持たせると「新しいビルドがどんな
    ファイルでできているか」を永遠に知らないままになる。実際 M21 で本体
    （.vwpayload）が増えたとき、古いアップデータはそれを写さず、利用者は zip を手で
    落として置き直す羽目になった。

    そこで配置は**落とした zip の中の vw-install.ps1**（リリースのアセットとしても公開
    されている）へ委ねる。委ね先はそのビルドと同じ版なので、ファイル構成や手順が変わって
    も自動アップデートだけで追随できる。zip にインストーラが無い——この仕組みより前の
    リリース——ときだけ、下の自前の配置へ落ちる。

    Usage:
      powershell -ExecutionPolicy Bypass -File vw-update.ps1            # ask which channel
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 stable
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 dev
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 q-stable                 # (used by the plug-in)
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 q-dev                    # (used by the plug-in)
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 do-install <url> <name>  # (used by the plug-in)

    Requirements: Windows PowerShell 5.1+ (ships with Windows) or PowerShell 7.
    Uses only built-in cmdlets (Invoke-RestMethod / Invoke-WebRequest /
    Expand-Archive) — no extra tools, and because the repository is public, no
    authentication.

    Overridable via environment:
      VW_REPO         owner/repo             (default below)
      VW_PLUGINS_DIR  Vectorworks Plug-Ins   (default: user folder for VW 2026;
                      the plug-in always passes the folder it actually loaded from)
#>

#requires -version 5
$ErrorActionPreference = 'Stop'

# Prefer TLS 1.2 (older Windows PowerShell defaults can be lower) and emit UTF-8
# so the plug-in reads our Japanese error messages without mojibake.
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}

$VW_REPO = if ($env:VW_REPO) { $env:VW_REPO } else { 'min-nano/vectorworks-plugin-import-ifc-homeskz' }
$VW_API  = "https://api.github.com/repos/$VW_REPO"
$VW_PLUGINS_DIR = if ($env:VW_PLUGINS_DIR) { $env:VW_PLUGINS_DIR } else { Join-Path $env:APPDATA 'Nemetschek\Vectorworks\2026\Plug-Ins' }

# 配布 zip のアセット名の末尾。アセット名を丸ごと決め打ちにせず末尾でも拾えるように
# してあるのは、**将来アセット名（プラグイン名）が変わっても、インストール済みの古い
# このスクリプトが「見つかりません」で止まらない**ようにするため（Get-PluginZipUrl）。
$VW_ZIP_SUFFIX = '.vlb.zip'
# 配置を委ねる先（落とした zip の直下にある）。冒頭のコメント参照。
$VW_INSTALLER = 'vw-install.ps1'

$script:LastError = ''
# 委ねたインストーラの機械可読な出力。委ねられなかったときは空のまま。
$script:InstallerOutput = ''

# ---------------------------------------------------------------------------
# GitHub REST + plug-in helpers.
# ---------------------------------------------------------------------------

# GET a GitHub API sub-path and return the parsed JSON. Throws on failure; the
# --max-time equivalent (-TimeoutSec) bounds it so the plug-in's start-up check
# can never hang Vectorworks on a slow/unreachable network.
function Invoke-GH([string] $subpath) {
    return Invoke-RestMethod -Uri "$VW_API/$subpath" `
        -Headers @{ 'Accept' = 'application/vnd.github+json' } `
        -UserAgent 'vw-update' -TimeoutSec 20 -Method Get
}

# browser_download_url of an asset by name, or $null.
function Get-AssetUrl($release, [string] $want) {
    foreach ($a in $release.assets) {
        if ($a.name -eq $want) { return $a.browser_download_url }
    }
    return $null
}

# 配布 zip の URL。**名前が完全一致しなければ、末尾が $VW_ZIP_SUFFIX のアセットで拾い
# 直す。** このスクリプトはインストール済みの（＝古い）ものが走るので、アセット名を
# 決め打ちにすると名前が変わった瞬間にアップデートの経路そのものが途切れる（利用者は
# 手で落とすしかなくなる）。1 つのリリースが持つ配布 zip はそのチャンネルの 1 つだけ
# なので、末尾での照合で取り違えは起きない。
function Get-PluginZipUrl($release, [string] $name) {
    $url = Get-AssetUrl $release ($name + $VW_ZIP_SUFFIX)
    if ($url) { return $url }
    foreach ($a in $release.assets) {
        if ($a.name -and $a.name.EndsWith($VW_ZIP_SUFFIX)) { return $a.browser_download_url }
    }
    return $null
}

# First 7 chars of a commit-ish, or '' if empty/null.
function Get-Short($commitish) {
    if (-not $commitish) { return '' }
    $s = [string] $commitish
    return $s.Substring(0, [Math]::Min(7, $s.Length))
}

# Stamped commit of the installed build, read from the "<name>.commit" sidecar
# the build ships next to its .vlb, or "none". (The mac side reads VWBuildCommit
# from the bundle's Info.plist; Windows has no plist, hence the sidecar.)
function Get-InstalledCommit([string] $name) {
    $f = Join-Path $VW_PLUGINS_DIR "$name.commit"
    if (Test-Path -LiteralPath $f) {
        $c = Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue
        if ($c) { return $c.Trim() }
    }
    return 'none'
}

# 殻（.vlb）の ID。ビルドが .vlb の隣へ置く "<name>.shell-id" から読む。**「アップデートに
# Vectorworks の再起動が要るか」を決める鍵**で、プラグイン側は自分にコンパイルされた
# VW_SHELL_ID と突き合わせる——一致するなら本体（.vwpayload）を読み直すだけで反映される
# （src/PayloadAbi.h / src/UpdaterParse.h）。読めなければ空文字（＝判断できないので、
# プラグイン側は安全側＝「再起動が要る」へ倒す）。
function Get-InstalledShellId([string] $name) {
    $f = Join-Path $VW_PLUGINS_DIR "$name.shell-id"
    if (Test-Path -LiteralPath $f) {
        $c = Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue
        if ($c) { return $c.Trim() }
    }
    return ''
}

# Install $src at $dst, replacing whatever is there. A currently-loaded .vlb
# cannot be deleted on Windows, but it CAN be renamed out of the way — so move any
# existing file aside (best effort) before copying the new one in. Leftover
# ".old-*" files are swept on the next install, once Vectorworks has unmapped
# them. (Named with the approved "Install" verb, like Install-Build, so
# PSScriptAnalyzer's PSUseApprovedVerbs rule stays satisfied.)
function Install-File([string] $src, [string] $dst) {
    if (Test-Path -LiteralPath $dst) {
        $bak = "$([System.IO.Path]::GetFileName($dst)).old-$([System.IO.Path]::GetRandomFileName())"
        try { Rename-Item -LiteralPath $dst -NewName $bak -ErrorAction Stop }
        catch { try { Remove-Item -LiteralPath $dst -Force -ErrorAction Stop } catch {} }
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

# ---------------------------------------------------------------------------
# 配置は zip の中のインストーラへ委ねる（冒頭のコメント参照）。ここから下の自前の配置は
# **この仕組みより前のリリースへ当たったときだけ**使う予備。
# ---------------------------------------------------------------------------

# Invoke-ZipInstaller: 展開済みの zip に入っている vw-install.ps1 へ配置を委ねる。委ね
# られたら、その機械可読な出力（installed-shell= / ok / error=）を 1 つの文字列で返す。
# 委ねられなければ $null（呼び出し側は自前の配置へ落ちる）。
#
# 「委ねられた」の判定は**結末の行が返ってきたか**で行う——インストーラが古い／壊れて
# いて何も言わないときに、成功したと取り違えないため。
function Invoke-ZipInstaller([string] $work, [string] $name) {
    $inst = Join-Path $work $VW_INSTALLER
    if (-not (Test-Path -LiteralPath $inst)) { return $null }
    # ダウンロード由来の「ブロック」印（Zone.Identifier）を外す。-ExecutionPolicy
    # Bypass でも走るが、外しておくほうが確実で副作用が無い。
    try { Unblock-File -LiteralPath $inst -ErrorAction SilentlyContinue } catch {}
    try {
        # 子スクリプトとして呼ぶ（& なので exit しても此方は死なない）。
        $lines = & $inst -Machine -From $work -Name $name -PluginsDir $VW_PLUGINS_DIR
    }
    catch { return $null }
    if (-not $lines) { return $null }
    $out = @($lines | ForEach-Object { [string] $_ })
    if (-not ($out | Where-Object { $_ -eq 'ok' -or $_ -like 'error=*' })) { return $null }
    return ($out -join "`n")
}

# Download <url> and install "<name>.vlb" (plus its .vwr, .commit and the updater
# script) into $VW_PLUGINS_DIR. Returns $true on success; sets $script:LastError
# on failure. Shows no UI (callers decide what, if anything, to display).
#
# 委ねられたときは、その出力を $script:InstallerOutput にそのまま入れて返す
# （Invoke-DoInstall がプラグインへ素通しする。増えたキーを途中で落とさないため）。
function Install-Build([string] $url, [string] $name) {
    $script:LastError = ''
    $script:InstallerOutput = ''
    if (-not $url -or -not $name) { $script:LastError = '引数が不足しています。'; return $false }

    $tmp = New-Item -ItemType Directory -Force -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("vwup-" + [System.IO.Path]::GetRandomFileName()))
    try {
        $zip = Join-Path $tmp.FullName "$name.vlb.zip"
        try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 120 }
        catch { $script:LastError = 'ダウンロードに失敗しました。'; return $false }

        $work = Join-Path $tmp.FullName 'x'
        try { Expand-Archive -LiteralPath $zip -DestinationPath $work -Force }
        catch { $script:LastError = 'アーカイブの展開に失敗しました。'; return $false }

        # **配置は落とした zip の中のインストーラへ委ねる**（上の Invoke-ZipInstaller）。
        $delegated = Invoke-ZipInstaller $work $name
        if ($delegated) {
            $script:InstallerOutput = $delegated
            if ($delegated -split "`n" | Where-Object { $_ -eq 'ok' }) { return $true }
            $err = ($delegated -split "`n" | Where-Object { $_ -like 'error=*' } | Select-Object -First 1)
            $script:LastError = if ($err) { $err.Substring(6) } else { 'インストールに失敗しました。' }
            return $false
        }

        # ここから下は予備——zip にインストーラが無い（この仕組みより前のリリース）
        # ときだけ通る。
        if (-not (Test-Path -LiteralPath (Join-Path $work "$name.vlb"))) {
            $script:LastError = "$name.vlb が zip 内に見つかりません。"; return $false
        }

        if (-not (Test-Path -LiteralPath $VW_PLUGINS_DIR)) {
            New-Item -ItemType Directory -Force -Path $VW_PLUGINS_DIR | Out-Null
        }

        # Sweep backups left by a previous update (now that VW has released them).
        Get-ChildItem -LiteralPath $VW_PLUGINS_DIR -Filter '*.old-*' -ErrorAction SilentlyContinue |
            ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction Stop } catch {} }

        # "$name.vwpayload" が**本体**（殻が自分で読み込むモジュール）。これが入れ替われば
        # Vectorworks を再起動しなくても次の操作から新しいコードが動く（src/PayloadAbi.h）。
        # "$name.shell-id" は「殻まで変わったか」を次回の判断に使う控え。
        foreach ($f in @("$name.vlb", "$name.vwpayload", "$name.vwr", "$name.commit",
                         "$name.shell-id", 'vw-update.ps1')) {
            $s = Join-Path $work $f
            if (Test-Path -LiteralPath $s) {
                try { Install-File $s (Join-Path $VW_PLUGINS_DIR $f) }
                catch { $script:LastError = 'インストール先へのコピーに失敗しました。'; return $false }
            }
        }
        return $true
    }
    finally {
        try { Remove-Item -LiteralPath $tmp.FullName -Recurse -Force -ErrorAction SilentlyContinue } catch {}
    }
}

# ---------------------------------------------------------------------------
# Non-interactive, machine-readable back end for the plug-in. These print simple
# key=value / TSV lines to stdout and NEVER prompt — the plug-in parses them and
# shows its own native Vectorworks dialogs. Transient failures are reported as an
# "error=<message>" line so the plug-in stays in control of what the user sees.
# ---------------------------------------------------------------------------

function Invoke-QStable {
    try { $rel = Invoke-GH 'releases/tags/stable' }
    catch { Write-Output 'error=stable リリースを取得できませんでした。'; return }

    $latestFull = $rel.target_commitish
    $url = Get-PluginZipUrl $rel 'HomeskzIfcImport'
    if (-not $latestFull -or -not $url) { Write-Output 'error=stable リリースの情報が不完全です。'; return }

    Write-Output ("installed=" + (Get-InstalledCommit 'HomeskzIfcImport'))
    Write-Output ("latest=" + (Get-Short $latestFull))
    Write-Output ("url=" + $url)
}

function Invoke-QDev {
    try { $rels = Invoke-GH 'releases?per_page=100' }
    catch { Write-Output 'error=リリース一覧を取得できませんでした。'; return }

    Write-Output ("installed=" + (Get-InstalledCommit 'HomeskzIfcImportDev'))

    foreach ($rel in $rels) {
        if ($rel.tag_name -like 'dev-*') {
            $url = Get-PluginZipUrl $rel 'HomeskzIfcImportDev'
            if ($url) {
                $name = if ($rel.name) { $rel.name } else { $rel.tag_name }
                # Only list builds that actually have a downloadable asset.
                Write-Output ("build`t" + (Get-Short $rel.target_commitish) + "`t" + $name + "`t" + $url)
            }
        }
    }
}

function Invoke-DoInstall([string] $url, [string] $name) {
    $installed = Install-Build $url $name
    # 委ねたときは**その出力をそのまま流す**。プラグインが読む契約
    # （installed-shell= / ok / error=）はインストーラ側が満たすので、ここで組み直すと
    # 将来キーが増えたときに落としてしまう。
    if ($script:InstallerOutput) { Write-Output $script:InstallerOutput; return }
    if ($installed) {
        # **いま入れた殻の ID を先に出す。** プラグインはこれを自分の VW_SHELL_ID と
        # 突き合わせて「再起動が要るか／本体の読み直しで済むか」を決める
        # （src/UpdaterParse.h の NeedsRestartAfterInstall）。読めなければ行を出さない
        # ＝プラグインは安全側へ倒す。
        $shell = Get-InstalledShellId $name
        if ($shell) { Write-Output "installed-shell=$shell" }
        Write-Output 'ok'
    }
    else {
        $e = if ($script:LastError) { $script:LastError } else { 'インストールに失敗しました。' }
        Write-Output "error=$e"
    }
}

# ---------------------------------------------------------------------------
# Interactive, console-based fallback (manual runs from a terminal).
# ---------------------------------------------------------------------------

function Invoke-Stable {
    try { $rel = Invoke-GH 'releases/tags/stable' }
    catch { Write-Host 'エラー: 安定版リリース (stable) が見つかりません。' -ForegroundColor Red; return }

    $url = Get-PluginZipUrl $rel 'HomeskzIfcImport'
    $latest = Get-Short $rel.target_commitish
    if (-not $latest -or -not $url) { Write-Host 'エラー: 安定版リリースの情報が不完全です。' -ForegroundColor Red; return }

    $installed = Get-InstalledCommit 'HomeskzIfcImport'
    if ($installed -eq $latest) { Write-Host "既に最新です（build $installed）。"; return }

    Write-Host '新しい安定版ビルドがあります。'
    Write-Host "  インストール済み: $installed"
    Write-Host "  最新: $latest"
    if ((Read-Host 'インストールしますか？ [y/N]') -notmatch '^[yY]') { Write-Host 'スキップしました。'; return }

    if (Install-Build $url 'HomeskzIfcImport') {
        Write-Host '更新しました。反映するには Vectorworks を再起動してください。' -ForegroundColor Green
    }
    else { Write-Host ("更新に失敗しました: " + $script:LastError) -ForegroundColor Red }
}

function Invoke-Dev {
    try { $rels = Invoke-GH 'releases?per_page=100' }
    catch { Write-Host 'エラー: リリース一覧を取得できませんでした。' -ForegroundColor Red; return }

    $builds = @()
    foreach ($rel in $rels) {
        if ($rel.tag_name -like 'dev-*') {
            $url = Get-PluginZipUrl $rel 'HomeskzIfcImportDev'
            if ($url) {
                $builds += [pscustomobject]@{
                    Name   = if ($rel.name) { $rel.name } else { $rel.tag_name }
                    Commit = Get-Short $rel.target_commitish
                    Url    = $url
                }
            }
        }
    }
    if ($builds.Count -eq 0) { Write-Host '開発版ビルド (dev-*) がまだありません。対象ブランチを push してビルドを走らせてください。'; return }

    Write-Host ("インストール済み: " + (Get-InstalledCommit 'HomeskzIfcImportDev'))
    Write-Host '利用可能な開発版ビルド:'
    for ($i = 0; $i -lt $builds.Count; $i++) {
        Write-Host ("  [{0}] {1} ({2})" -f ($i + 1), $builds[$i].Name, $builds[$i].Commit)
    }

    $sel = Read-Host 'インストールするビルド番号（Enter でキャンセル）'
    if (-not $sel) { Write-Host 'キャンセルしました。'; return }
    $n = 0
    if (-not [int]::TryParse($sel, [ref] $n) -or $n -lt 1 -or $n -gt $builds.Count) {
        Write-Host '無効な選択です。'; return
    }

    $b = $builds[$n - 1]
    if (Install-Build $b.Url 'HomeskzIfcImportDev') {
        Write-Host 'インストールしました。反映するには Vectorworks を再起動してください。' -ForegroundColor Green
    }
    else { Write-Host ("インストールに失敗しました: " + $script:LastError) -ForegroundColor Red }
}

# ---------------------------------------------------------------------------
# Dispatch only when this file is EXECUTED (the plug-in runs it with -File; a
# manual run is the same), NOT when it is dot-sourced. The unit tests
# (tests/vw-update.Tests.ps1) dot-source the script to call its back-end
# functions (Get-AssetUrl / Invoke-QStable / Invoke-QDev / Invoke-DoInstall) with
# Invoke-GH / Invoke-WebRequest stubbed out — there $MyInvocation.InvocationName
# is '.', so the switch below does not run. This is the PowerShell analogue of the
# BASH_SOURCE guard in vw-update.sh, and of the IUpdaterHost seam that makes
# UpdaterFlow.cpp testable.
if ($MyInvocation.InvocationName -ne '.') {
    $mode = if ($args.Count -ge 1) { [string] $args[0] } else { '' }

    switch ($mode) {
        'q-stable'   { Invoke-QStable }
        'q-dev'      { Invoke-QDev }
        'do-install' { Invoke-DoInstall ([string] $args[1]) ([string] $args[2]) }
        'stable'     { Invoke-Stable }
        'dev'        { Invoke-Dev }
        '' {
            Write-Host 'どのビルドを確認しますか？'
            Write-Host '  [1] stable（安定版 / main）'
            Write-Host '  [2] dev（開発版 / ブランチ選択）'
            switch (Read-Host '番号') {
                '1' { Invoke-Stable }
                '2' { Invoke-Dev }
                default { Write-Host 'キャンセルしました。' }
            }
        }
        default { Write-Output "error=不明なチャンネル: '$mode'（stable / dev / q-stable / q-dev / do-install）。" }
    }
}
