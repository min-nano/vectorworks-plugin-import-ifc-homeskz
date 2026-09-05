<#
    vw-uninstall.ps1 — remove ONE installed build of the HomeskzIfcImport Vectorworks
    plug-in from a Vectorworks 2026 Plug-Ins folder (Windows).

    This is the Windows counterpart of scripts/vw-uninstall.sh; see that file's
    header for the full rationale. In short:

    **これは vw-install.ps1 の裏返しで、同じ理由で同じところに配られる。** 配布 zip の
    直下に同梱され、インストール時に**プラグインのフォルダの中へ一緒に置かれる**
    （インストーラ自身は置かれないが、これは置かれる）。リリースのアセットとしても
    単独で公開される。

    なぜ「インストール先へ一緒に置く」のか: **その版が置いたものを正しく取り除けるのは、
    その版自身のアンインストーラだけ**だから。アップデートは「前の版を取り除いてから
    新しい版を入れる」順で走る（vw-install.ps1 の Remove-Installed）。

    取り除くものの規則もひとつだけ: **そのプラグインのフォルダをまるごと。**
    ファイル名を列挙しない。

    **消してよいフォルダかどうかは必ず確かめる。** 名前が一致し、かつ中に殻
    （<name>.vlb）があるときだけ消す。Plug-Ins そのものや、無関係なディレクトリを
    巻き込まないための歯止めで、これが唯一の削除の安全弁である。

    **読み込み中の .vlb は削除できない。** Windows では実行中の DLL を消せないので、
    中身は「退かしてから消す」（消せなければ退かしたまま残す）。退いた ".old-*" は
    拡張子が .vlb ではないので Vectorworks は読み込まないし、次のインストールが掃く。

    Usage:
      powershell -ExecutionPolicy Bypass -File vw-uninstall.ps1
      powershell -ExecutionPolicy Bypass -File vw-uninstall.ps1 -Name HomeskzIfcImportDev
      powershell -ExecutionPolicy Bypass -File vw-uninstall.ps1 -PluginsDir <dir> -Machine

    Options:
      -Name <plugin>      HomeskzIfcImport / HomeskzIfcImportDev（既定は自動判定）
      -PluginsDir <dir>   Plug-Ins またはプラグインのフォルダ（既定: VW_PLUGINS_DIR、
                          無ければ VW2026 のユーザフォルダ）
      -Machine            機械可読な出力（removed= / ok / error=）
      -Help               使い方

    **入っていなければ成功とみなす**（アップデートの入口で無条件に叩けるように）。
    **知らないオプションは黙って読み飛ばす**（そのために param() ブロックを使わず
    $args を自分で読む）。

    Requirements: Windows PowerShell 5.1+ or PowerShell 7.

    Overridable via environment:
      VW_PLUGINS_DIR  Vectorworks Plug-Ins   (default: user folder for VW 2026)
#>

#requires -version 5
$ErrorActionPreference = 'Stop'

try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}

# 殻の拡張子。**「そのフォルダは本当にこのプラグインのものか」を確かめる鍵**で、これが
# 中に無いフォルダは消さない。
$VW_SHELL_EXT = '.vlb'

$script:LastError = ''
$script:Removed = ''
$script:Machine = $false
$script:ExitCode = 0
$script:PluginsDir = if ($env:VW_PLUGINS_DIR) { $env:VW_PLUGINS_DIR } else { Join-Path $env:APPDATA 'Nemetschek\Vectorworks\2026\Plug-Ins' }

function Write-Note([string] $message) {
    if (-not $script:Machine) { Write-Host $message }
}

function Show-Usage {
    Write-Host 'vw-uninstall.ps1 — インストール済みの HomeskzIfcImport を取り除く'
    Write-Host '  -Name <plugin>     HomeskzIfcImport / HomeskzIfcImportDev（既定は自動判定）'
    Write-Host '  -PluginsDir <dir>  Plug-Ins またはプラグインのフォルダ'
    Write-Host '  -Machine           機械可読な出力（removed= / ok / error=）'
}

# ---------------------------------------------------------------------------
# 置き場所の決め方。**vw-install.ps1 の Get-PluginDir と同じ規則**でなければならない
# （片方だけ変えると、入れた場所と消す場所が食い違う）。
# ---------------------------------------------------------------------------
function Get-PluginDir([string] $root, [string] $name) {
    if ((Split-Path -Leaf $root) -eq $name) { return $root }
    return (Join-Path $root $name)
}

# -Name が無いときにプラグイン名を割り出す。渡された先がプラグインのフォルダそのもの
# ならその名前、Plug-Ins ならその中の <名前>/<名前>.vlb を探す。
function Get-InstalledName([string] $root) {
    if (-not (Test-Path -LiteralPath $root)) { return '' }
    $leaf = Split-Path -Leaf $root
    if (Test-Path -LiteralPath (Join-Path $root "$leaf$VW_SHELL_EXT")) { return $leaf }
    foreach ($d in Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue) {
        if (Test-Path -LiteralPath (Join-Path $d.FullName "$($d.Name)$VW_SHELL_EXT")) { return $d.Name }
    }
    return ''
}

# ---------------------------------------------------------------------------
# 取り除く。**削除はここ 1 か所だけ**で、その手前に必ず安全弁を通す。
# ---------------------------------------------------------------------------
function Remove-PluginDir([string] $dir, [string] $name) {
    if (-not (Test-Path -LiteralPath $dir)) {
        Write-Note "取り除くものはありません（$dir は存在しません）。"
        return $true
    }

    # --- 安全弁 -------------------------------------------------------------
    if ((Split-Path -Leaf $dir) -ne $name) {
        $script:LastError = "取り除ける形になっていません（$dir はプラグインのフォルダではありません）。"
        return $false
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dir "$name$VW_SHELL_EXT"))) {
        $script:LastError = "取り除ける形になっていません（$dir に $name$VW_SHELL_EXT がありません）。"
        return $false
    }

    # 中身を 1 つずつ「退かしてから消す」。**読み込み中の .vlb は消せないが退かせる**
    # ので、Vectorworks が走っていても殻は確実にどかせる（拡張子が変わるため、次の
    # 起動でもう読み込まれない）。消せなかったものは退いたまま残し、次のインストールが
    # 掃く（vw-install.ps1 の Install-Tree）。ロック中のファイルが消せないのは想定内
    # なので catch は空でよい。
    foreach ($item in Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue) {
        $bak = "$($item.Name).old-$([System.IO.Path]::GetRandomFileName())"
        try { Rename-Item -LiteralPath $item.FullName -NewName $bak -ErrorAction Stop }
        catch {}
        $current = Join-Path $dir $bak
        if (-not (Test-Path -LiteralPath $current)) { $current = $item.FullName }
        try { Remove-Item -LiteralPath $current -Recurse -Force -ErrorAction Stop } catch {}
    }

    # 空になっていればフォルダごと消す。ロックされた残骸があれば残る（想定内）。
    try { Remove-Item -LiteralPath $dir -Recurse -ErrorAction Stop } catch {}

    $script:Removed = $dir
    Write-Note "取り除きました: $dir"
    return $true
}

# 引数を読む。**知らないオプションは黙って読み飛ばす。**
function Read-Option([string[]] $argv) {
    $opt = @{ Name = ''; Help = $false }
    if (-not $argv) { return $opt }
    $i = 0
    while ($i -lt $argv.Count) {
        $a = [string] $argv[$i]
        $next = if ($i + 1 -lt $argv.Count) { [string] $argv[$i + 1] } else { '' }
        # switch -regex は一致した節をすべて実行するので、各節を break で閉じる。
        switch -regex ($a) {
            '^-(?i:machine)$' { $script:Machine = $true; break }
            '^-(?i:name)$' { $opt.Name = $next; $i++; break }
            '^-(?i:pluginsdir)$' { $script:PluginsDir = $next; $i++; break }
            '^-(?i:h|help|\?)$' { $opt.Help = $true; break }
            '^-' {
                if ($next -and -not $next.StartsWith('-')) { $i++ }
                break
            }
            default { break }
        }
        $i++
    }
    return $opt
}

# ---------------------------------------------------------------------------
# 結末は $script:ExitCode で返す（return した値は出力ストリームに混ざるため。
# vw-install.ps1 の Invoke-Main と同じ理由）。
function Invoke-Main([string[]] $argv) {
    $script:ExitCode = 0
    $opt = Read-Option $argv
    if ($opt.Help) { Show-Usage; return }

    $name = $opt.Name
    if (-not $name) { $name = Get-InstalledName $script:PluginsDir }

    $ok = $false
    if (-not $name) {
        # 何も入っていない。**これも成功**（アップデートの入口で無条件に叩ける）。
        Write-Note "取り除くものはありません（$script:PluginsDir にプラグインが見つかりません）。"
        $ok = $true
    }
    else {
        $ok = Remove-PluginDir (Get-PluginDir $script:PluginsDir $name) $name
    }

    if ($script:Machine) {
        if ($ok) {
            if ($script:Removed) { Write-Output "removed=$script:Removed" }
            Write-Output 'ok'
        }
        else {
            $e = if ($script:LastError) { $script:LastError } else { '取り除けませんでした。' }
            Write-Output "error=$e"
        }
        # machine モードは常に 0 で終わる（結末は stdout の行が持つ）。
        return
    }

    if ($ok) { return }
    $e = if ($script:LastError) { $script:LastError } else { '取り除けませんでした。' }
    Write-Host "エラー: $e" -ForegroundColor Red
    $script:ExitCode = 1
}

# 直接実行したときだけ走らせる（テストはドットソースして個々の関数を叩く）。
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Main $args
    exit $script:ExitCode
}
