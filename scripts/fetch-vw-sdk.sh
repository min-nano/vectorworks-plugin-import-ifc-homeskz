#!/usr/bin/env bash
#
# fetch-vw-sdk.sh — Vectorworks SDK を CI ランナーへ用意する（唯一の定義）。
#
# なぜこれがあるか
# ----------------
# SDK を要するジョブは build.yml だけで 4 つある（mac / Windows のビルドと、
# mac / Windows の clang-tidy）。さらに ci-debug.yml も同じことをする。「大きな
# zip を取ってきて、必要な部分だけ残して、ちゃんと揃っているか確かめる」という
# 手順を各ジョブのインライン run: に書くと 5 か所に増え、SDK の構成が変わったときに
# 直し漏れる（CLAUDE.md「重複を作らない置き場所」）。手順はここに 1 つだけ置く。
#
# **キャッシュがヒットしていれば何もしない。** 呼び出し側は actions/cache で
# $VW_SDK_DIR を復元してからこれを呼ぶだけでよく、「ヒットしたか」で分岐する必要は
# ない（このスクリプトが中身を見て判断する）。ヒット時は検証だけ行って抜けるので、
# 壊れたキャッシュを引いた場合もその場で分かる。
#
# 環境変数:
#   VW_SDK_DIR   トリミング済み SDK の置き場所（この下に SDKLib/ を作る）
#   VW_SDK_URL   SDK zip の URL
#   RUNNER_OS    GitHub が設定する（Windows / macOS）。未設定なら uname から推測する。
#
# 残すのは、プラグインをビルドするのに要る部分だけ:
#   Include            ヘッダ
#   Source             Include/ の一部が ../../../Source/VWSDK/... を include する
#   LibMac / LibWin    リンクするライブラリ（libVWSDK.a / VWSDK.lib と VWMM）
#   ToolsMac / ToolsWin  .vwr リソースを固める BuildVWR（Windows は 7-Zip も同梱）

set -euo pipefail

SDK_DIR="${VW_SDK_DIR:?VW_SDK_DIR is not set}"
SDK_URL="${VW_SDK_URL:?VW_SDK_URL is not set}"

case "${RUNNER_OS:-$(uname -s)}" in
	Windows | MINGW* | MSYS* | CYGWIN*)
		OS=windows
		SUBDIRS="Include Source LibWin ToolsWin"
		;;
	macOS | Darwin)
		OS=mac
		SUBDIRS="Include Source LibMac ToolsMac"
		;;
	*)
		echo "::error::fetch-vw-sdk.sh: Vectorworks SDK は macOS / Windows 用しかありません（RUNNER_OS=${RUNNER_OS:-$(uname -s)}）" >&2
		exit 1
		;;
esac

# verify: プラグインのビルドに最低限要る 3 点が揃っているか。ダウンロード直後だけ
# でなくキャッシュヒット時にも走らせる（壊れた・古い形のキャッシュをここで弾く）。
verify() {
	local ok=0
	[ -f "$SDK_DIR/SDKLib/Include/VectorworksSDK.h" ] || ok=1
	if [ "$OS" = windows ]; then
		[ -f "$SDK_DIR/SDKLib/LibWin/Release/VWSDK.lib" ] || ok=1
		[ -f "$SDK_DIR/SDKLib/ToolsWin/BuildVWR/buildvwr.exe" ] || ok=1
	else
		[ -f "$SDK_DIR/SDKLib/LibMac/Release/libVWSDK.a" ] || ok=1
		[ -x "$SDK_DIR/SDKLib/ToolsMac/BuildVWR/BuildVWR" ] || ok=1
	fi
	return "$ok"
}

if verify; then
	echo "Vectorworks SDK is already present at $SDK_DIR (cache hit)."
	exit 0
fi

if [ -e "$SDK_DIR" ]; then
	# キャッシュはヒットしたが中身が期待と違う、というケース。中途半端に混ざると
	# 原因が分かりにくいので、作り直す。
	echo "$SDK_DIR exists but is incomplete — re-fetching from scratch."
	rm -rf "$SDK_DIR"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Downloading the Vectorworks SDK (a large zip: ~140 MB mac / ~90 MB win)..."
curl -fL --retry 4 --retry-delay 5 -o "$work/sdk.zip" "$SDK_URL"

# 展開。ランナーによって入っている道具が違うので順に試す（git-bash には unzip が
# 無いことがあり、そこでは 7z か PowerShell の Expand-Archive を使う）。
echo "Extracting..."
if command -v unzip >/dev/null 2>&1; then
	unzip -q "$work/sdk.zip" -d "$work/extracted"
elif command -v 7z >/dev/null 2>&1; then
	7z x -bso0 -bsp0 -o"$work/extracted" "$work/sdk.zip"
elif command -v pwsh >/dev/null 2>&1 || command -v powershell >/dev/null 2>&1; then
	ps="$(command -v pwsh || command -v powershell)"
	"$ps" -NoProfile -Command \
		"\$ProgressPreference='SilentlyContinue'; Expand-Archive -LiteralPath '$(cygpath -w "$work/sdk.zip" 2>/dev/null || echo "$work/sdk.zip")' -DestinationPath '$(cygpath -w "$work/extracted" 2>/dev/null || echo "$work/extracted")' -Force"
else
	echo "::error::fetch-vw-sdk.sh: 展開する道具がありません（unzip / 7z / PowerShell のいずれも無い）" >&2
	exit 1
fi

# zip は全体を 1 階層のフォルダで包んでいることがあるので SDKLib を探す。macOS の
# zip が混ぜる "__MACOSX" メタデータミラーは避ける。
sdklib="$(find "$work/extracted" -type d -name SDKLib -not -path '*/__MACOSX/*' -print -quit)"
if [ -z "$sdklib" ]; then
	echo "::error::fetch-vw-sdk.sh: ダウンロードした SDK の中に SDKLib が見つかりません" >&2
	exit 1
fi
echo "Found SDKLib at: $sdklib"

mkdir -p "$SDK_DIR/SDKLib"
for sub in $SUBDIRS; do
	if [ -d "$sdklib/$sub" ]; then
		cp -R "$sdklib/$sub" "$SDK_DIR/SDKLib/$sub"
	else
		echo "(note) SDKLib/$sub not present in this SDK; skipping."
	fi
done

if [ "$OS" = mac ]; then
	# BuildVWR が実行できる状態か確かめる（実行ビット・Gatekeeper の隔離属性）。
	chmod +x "$SDK_DIR/SDKLib/ToolsMac/BuildVWR/BuildVWR" || true
	xattr -dr com.apple.quarantine "$SDK_DIR/SDKLib/ToolsMac" 2>/dev/null || true
fi

echo "Trimmed SDK size:"
du -sh "$SDK_DIR"

if ! verify; then
	echo "::error::fetch-vw-sdk.sh: SDK を用意しましたが必要なファイルが揃っていません（$SDK_DIR）" >&2
	find "$SDK_DIR" -maxdepth 3 >&2 || true
	exit 1
fi
echo "SDK looks good."
