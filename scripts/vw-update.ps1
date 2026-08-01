<#
    vw-update.ps1 — download the latest CI build of the HomeskzIfcImport Vectorworks
    plug-in and install it into your Vectorworks 2026 Plug-Ins folder (Windows).

    This is the Windows counterpart of scripts/vw-update.sh. On Windows a
    Vectorworks plug-in module is a "<name>.vlb" file (a DLL) with a sibling
    "<name>.vwr" resource, so this script installs those flat files rather than a
    macOS ".vwlibrary" bundle.

    Builds are distributed from a Cloudflare R2 bucket, NOT from GitHub Releases:
    reading (pre)release assets through the GitHub API proved unreliable from the
    client side, so the bucket serves the zips plus small JSON manifests over a
    public base URL, and this script reads only those. The bucket layout and the
    manifest schema are documented in scripts/r2-publish.sh (the CI-side writer).

    Two channels, two separately-named plug-ins that can be installed at once:

      stable  -> "HomeskzIfcImport.vlb"     from "stable/manifest.json" (main).
      dev     -> "HomeskzIfcImportDev.vlb"  from "dev/index.json", which lists one
                 entry per branch; you pick which branch's build to install.

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

    Usage:
      powershell -ExecutionPolicy Bypass -File vw-update.ps1            # ask which channel
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 stable
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 dev
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 q-stable                 # (used by the plug-in)
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 q-dev                    # (used by the plug-in)
      powershell -ExecutionPolicy Bypass -File vw-update.ps1 do-install <url> <name>  # (used by the plug-in)

    Requirements: Windows PowerShell 5.1+ (ships with Windows) or PowerShell 7.
    Uses only built-in cmdlets (Invoke-RestMethod / Invoke-WebRequest /
    Expand-Archive) — no extra tools, and because the bucket is served publicly,
    no authentication.

    Overridable via environment:
      VW_BASE_URL     distribution base URL  (baked in at build time, see below)
      VW_PLUGINS_DIR  Vectorworks Plug-Ins   (default: user folder for VW 2026;
                      the plug-in always passes the folder it actually loaded from)
#>

#requires -version 5
$ErrorActionPreference = 'Stop'

# Prefer TLS 1.2 (older Windows PowerShell defaults can be lower) and emit UTF-8
# so the plug-in reads our Japanese error messages without mojibake.
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}

# The distribution base URL is CONFIGURATION, not source: CMake substitutes it
# into the copy of this script that ships next to the .vlb (see
# VW_UPDATE_BASE_URL in CMakeLists.txt). The copy in the repository still holds
# the unexpanded placeholder — detect that and treat it as "not configured", so
# a manual run can supply the URL through VW_BASE_URL instead.
$VW_BASE_DEFAULT = '@VW_UPDATE_BASE_URL@'
if ($VW_BASE_DEFAULT.StartsWith('@')) { $VW_BASE_DEFAULT = '' }
$VW_BASE_URL = if ($env:VW_BASE_URL) { $env:VW_BASE_URL } else { $VW_BASE_DEFAULT }
$VW_BASE_URL = $VW_BASE_URL.TrimEnd('/')

$VW_PLUGINS_DIR = if ($env:VW_PLUGINS_DIR) { $env:VW_PLUGINS_DIR } else { Join-Path $env:APPDATA 'Nemetschek\Vectorworks\2026\Plug-Ins' }

# The two manifests this script reads (relative to $VW_BASE_URL).
$VW_STABLE_MANIFEST = 'stable/manifest.json'
$VW_DEV_INDEX = 'dev/index.json'

# Shown when $VW_BASE_URL is not configured; used by both the console and the
# machine-readable paths.
$VW_NO_BASE_MSG = '配布先 URL が設定されていません（環境変数 VW_BASE_URL）。'

$script:LastError = ''

# ---------------------------------------------------------------------------
# Distribution + plug-in helpers.
# ---------------------------------------------------------------------------

# True when a distribution base URL is available at all. Callers check this
# BEFORE Get-Manifest so a missing URL is reported as such, rather than as the
# generic "could not fetch" message their catch block would otherwise produce
# (the bash side draws the same distinction with have_base_url).
function Test-BaseUrl { return [bool] $VW_BASE_URL }

# GET one of the bucket's JSON objects and return the parsed result. Throws on
# failure (including "no base URL configured", as a backstop for callers that
# forgot Test-BaseUrl); -TimeoutSec bounds it so the plug-in's start-up check
# can never hang Vectorworks on a slow/unreachable network.
#
# The manifests are uploaded with "Cache-Control: no-cache", but a stale copy
# from an intermediate cache is exactly the flakiness this move away from the
# GitHub API was meant to end — so ask for a fresh copy AND append a
# cache-busting query. (The zips are immutable per commit and need neither.)
function Get-Manifest([string] $key) {
    if (-not $VW_BASE_URL) { throw $VW_NO_BASE_MSG }
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $uri = "$VW_BASE_URL/$key" + "?ts=$ts"
    return Invoke-RestMethod -Uri $uri `
        -Headers @{ 'Cache-Control' = 'no-cache' } `
        -UserAgent 'vw-update' -TimeoutSec 20 -Method Get
}

# Read one property off a parsed JSON object, or $null when it is absent. Going
# through PSObject.Properties keeps this safe under Set-StrictMode (which the
# test harness enables), where touching a missing property would throw.
function Get-Field($obj, [string] $name) {
    if ($null -eq $obj) { return $null }
    $p = $obj.PSObject.Properties[$name]
    if ($p) { return $p.Value }
    return $null
}

# First 7 chars of a commit-ish, or '' if empty/null.
function Get-Short($commitish) {
    if (-not $commitish) { return '' }
    $s = [string] $commitish
    return $s.Substring(0, [Math]::Min(7, $s.Length))
}

# The 7-char build id a manifest entry identifies itself by: its own "short"
# field, falling back to a truncated "commit" so an older manifest still works.
function Get-BuildId($entry) {
    $short = Get-Field $entry 'short'
    if ($short) { return [string] $short }
    return Get-Short (Get-Field $entry 'commit')
}

# The branch name shown in the build picker; falls back to the bucket folder.
function Get-BuildName($entry) {
    $branch = Get-Field $entry 'branch'
    if ($branch) { return [string] $branch }
    $slug = Get-Field $entry 'slug'
    if ($slug) { return [string] $slug }
    return ''
}

# The dev builds listed in dev/index.json, as parsed objects (empty if none).
function Get-DevBuildList($index) {
    $builds = Get-Field $index 'builds'
    if ($null -eq $builds) { return @() }
    return @($builds)
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

# Download <url> and install "<name>.vlb" (plus its .vwr, .commit and the updater
# script) into $VW_PLUGINS_DIR. Returns $true on success; sets $script:LastError
# on failure. Shows no UI (callers decide what, if anything, to display).
function Install-Build([string] $url, [string] $name) {
    $script:LastError = ''
    if (-not $url -or -not $name) { $script:LastError = '引数が不足しています。'; return $false }

    $tmp = New-Item -ItemType Directory -Force -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("vwup-" + [System.IO.Path]::GetRandomFileName()))
    try {
        $zip = Join-Path $tmp.FullName "$name.vlb.zip"
        try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 120 }
        catch { $script:LastError = 'ダウンロードに失敗しました。'; return $false }

        $work = Join-Path $tmp.FullName 'x'
        try { Expand-Archive -LiteralPath $zip -DestinationPath $work -Force }
        catch { $script:LastError = 'アーカイブの展開に失敗しました。'; return $false }

        if (-not (Test-Path -LiteralPath (Join-Path $work "$name.vlb"))) {
            $script:LastError = "$name.vlb が zip 内に見つかりません。"; return $false
        }

        if (-not (Test-Path -LiteralPath $VW_PLUGINS_DIR)) {
            New-Item -ItemType Directory -Force -Path $VW_PLUGINS_DIR | Out-Null
        }

        # Sweep backups left by a previous update (now that VW has released them).
        Get-ChildItem -LiteralPath $VW_PLUGINS_DIR -Filter '*.old-*' -ErrorAction SilentlyContinue |
            ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction Stop } catch {} }

        foreach ($f in @("$name.vlb", "$name.vwr", "$name.commit", 'vw-update.ps1')) {
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
    if (-not (Test-BaseUrl)) { Write-Output "error=$VW_NO_BASE_MSG"; return }
    try { $m = Get-Manifest $VW_STABLE_MANIFEST }
    catch { Write-Output 'error=stable のマニフェストを取得できませんでした。'; return }

    $latest = Get-BuildId $m
    $url = Get-Field $m 'win'
    if (-not $latest -or -not $url) { Write-Output 'error=stable のマニフェストの内容が不完全です。'; return }

    Write-Output ("installed=" + (Get-InstalledCommit 'HomeskzIfcImport'))
    Write-Output ("latest=" + $latest)
    Write-Output ("url=" + $url)
}

function Invoke-QDev {
    if (-not (Test-BaseUrl)) { Write-Output "error=$VW_NO_BASE_MSG"; return }
    try { $index = Get-Manifest $VW_DEV_INDEX }
    catch { Write-Output 'error=開発版ビルドの一覧を取得できませんでした。'; return }

    Write-Output ("installed=" + (Get-InstalledCommit 'HomeskzIfcImportDev'))

    foreach ($b in (Get-DevBuildList $index)) {
        $url = Get-Field $b 'win'
        $short = Get-BuildId $b
        # Only list builds that actually have a downloadable asset.
        if ($url -and $short) {
            Write-Output ("build`t" + $short + "`t" + (Get-BuildName $b) + "`t" + $url)
        }
    }
}

function Invoke-DoInstall([string] $url, [string] $name) {
    if (Install-Build $url $name) {
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
    if (-not (Test-BaseUrl)) { Write-Host "エラー: $VW_NO_BASE_MSG" -ForegroundColor Red; return }
    try { $m = Get-Manifest $VW_STABLE_MANIFEST }
    catch { Write-Host 'エラー: 安定版のマニフェストを取得できませんでした。' -ForegroundColor Red; return }

    $url = Get-Field $m 'win'
    $latest = Get-BuildId $m
    if (-not $latest -or -not $url) { Write-Host 'エラー: 安定版マニフェストの内容が不完全です。' -ForegroundColor Red; return }

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
    if (-not (Test-BaseUrl)) { Write-Host "エラー: $VW_NO_BASE_MSG" -ForegroundColor Red; return }
    try { $index = Get-Manifest $VW_DEV_INDEX }
    catch { Write-Host 'エラー: 開発版ビルドの一覧を取得できませんでした。' -ForegroundColor Red; return }

    $builds = @()
    foreach ($b in (Get-DevBuildList $index)) {
        $url = Get-Field $b 'win'
        $short = Get-BuildId $b
        if ($url -and $short) {
            $builds += [pscustomobject]@{
                Name   = Get-BuildName $b
                Commit = $short
                Url    = $url
            }
        }
    }
    if ($builds.Count -eq 0) { Write-Host '開発版ビルドがまだありません。対象ブランチを push してビルドを走らせてください。'; return }

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
# functions (Get-Field / Invoke-QStable / Invoke-QDev / Invoke-DoInstall) with
# Get-Manifest / Invoke-WebRequest stubbed out — there $MyInvocation.InvocationName
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
