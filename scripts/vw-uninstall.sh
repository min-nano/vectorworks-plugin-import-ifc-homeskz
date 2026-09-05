#!/usr/bin/env bash
#
# vw-uninstall.sh — remove ONE installed build of the HomeskzIfcImport Vectorworks
# plug-in from a Vectorworks 2026 Plug-Ins folder (macOS).
#
# **これは `vw-install.sh` の裏返しで、同じ理由で同じところに配られる。**
#
#   * 配布 zip の直下に同梱され、インストール時に**プラグインのフォルダの中へ一緒に
#     置かれる**（インストーラ自身は置かれないが、これは置かれる。理由は下記）。
#   * リリースのアセットとしても単独で公開される（手動アンインストールの入口）。
#
# なぜ「インストール先へ一緒に置く」のか: **その版が置いたものを正しく取り除けるのは、
# その版自身のアンインストーラだけ**だから。アップデートは「前の版を取り除いてから新しい
# 版を入れる」順で走るので（`vw-install.sh` の `remove_installed`）、取り除く側は
# **いま入っている版に同梱されていたもの**でなければならない。インストーラが「新しい側の
# 知識で置く」のと、ちょうど対になっている。
#
# 取り除くものの規則もひとつだけ: **そのプラグインのフォルダをまるごと。**
# ファイル名を列挙しない——列挙すると、その版が増やしたファイルを取りこぼして
# Plug-Ins に残骸が残る（`vw-install.sh` の冒頭と同じ理由）。
#
# **消してよいフォルダかどうかは必ず確かめる。** 名前が一致し、かつ中に殻
# （`<name>.vwlibrary`）があるときだけ消す。`Plug-Ins` そのものや、無関係な
# ディレクトリを巻き込まないための歯止めで、これが唯一の削除の安全弁である。
#
# Usage:
#   ./vw-uninstall.sh                          # 既定の場所から HomeskzIfcImport を消す
#   ./vw-uninstall.sh --name HomeskzIfcImportDev
#   ./vw-uninstall.sh --plugins-dir <dir> --machine   # (vw-install.sh が使う)
#
# Options:
#   --name <plugin>      HomeskzIfcImport / HomeskzIfcImportDev（既定は置かれている
#                        ものから判定する）
#   --plugins-dir <dir>  Plug-Ins（またはプラグインのフォルダそのもの）。既定は
#                        VW_PLUGINS_DIR、無ければ VW2026 のユーザフォルダ
#   --machine            機械可読な出力にする（removed= / ok / error=）
#   -h | --help          使い方
#
# **入っていなければ成功とみなす。** アップデートの入口で無条件に叩ける（初回インストール
# でも、この仕組みより前の版からでも止まらない）ようにするため。
#
# **知らないオプションは黙って読み飛ばす**（`vw-install.sh` と同じ理由——新しい側が古い
# 側を呼ぶ向きが起こりうる）。
#
# Requirements: macOS only（実際に使うのは coreutils と bash だけだが、対象のパスが
# macOS のものである）。
#
# Overridable via environment:
#   VW_PLUGINS_DIR  Vectorworks Plug-Ins   (default: user folder for VW 2026)
#
set -euo pipefail

# 殻の入れ物の拡張子。**「そのフォルダは本当にこのプラグインのものか」を確かめる鍵**で、
# これが中に無いフォルダは消さない。
VW_SHELL_EXT=".vwlibrary"

MACHINE=0
OPT_NAME=""
PLUGINS_DIR="${VW_PLUGINS_DIR:-$HOME/Library/Application Support/Vectorworks/2026/Plug-Ins}"

# 失敗の理由。machine モードでは "error=<理由>"、人が実行したときは stderr へ。
LAST_ERROR=""
# 実際に取り除いた場所（machine モードで報告する。何もしなかったときは空）。
REMOVED=""

fail() { # message -> always 1
	LAST_ERROR="$1"
	return 1
}

say() {
	[ "$MACHINE" -eq 1 ] || printf '%s\n' "$1"
}

usage() {
	cat <<'USAGE'
vw-uninstall.sh — インストール済みの HomeskzIfcImport を取り除く（macOS）

  ./vw-uninstall.sh                              既定の場所から取り除く
  ./vw-uninstall.sh --name HomeskzIfcImportDev   dev 版を取り除く

  --name <plugin>      HomeskzIfcImport / HomeskzIfcImportDev（既定は自動判定）
  --plugins-dir <dir>  Plug-Ins またはプラグインのフォルダ（既定: VW2026 のユーザフォルダ）
  --machine            機械可読な出力（removed= / ok / error=）
USAGE
}

# ---------------------------------------------------------------------------
# 置き場所の決め方。**`vw-install.sh` の plugin_dir と同じ規則**でなければならない
# （片方だけ変えると、入れた場所と消す場所が食い違う）。プラグインは自分のフォルダを
# 1 つ持つので、渡された先が既にそのフォルダなら足さない。
# ---------------------------------------------------------------------------
plugin_dir() { # plugins-dir, name -> the plug-in's own folder
	local root="$1" name="$2"
	if [ "$(basename "$root")" = "$name" ]; then
		printf '%s' "$root"
	else
		printf '%s' "$root/$name"
	fi
}

# guess_name: `--name` が無いときにプラグイン名を割り出す。渡された先がプラグインの
# フォルダそのものなら、その名前。Plug-Ins なら、その中の <名前>/<名前>.vwlibrary を探す。
guess_name() { # plugins-dir -> name or ""
	local root="$1" base entry
	base="$(basename "$root")"
	if [ -d "$root/${base}${VW_SHELL_EXT}" ]; then
		printf '%s' "$base"
		return 0
	fi
	for entry in "$root"/*/; do
		[ -d "$entry" ] || continue
		base="$(basename "$entry")"
		if [ -d "$entry/${base}${VW_SHELL_EXT}" ]; then
			printf '%s' "$base"
			return 0
		fi
	done
	printf ''
}

# ---------------------------------------------------------------------------
# 取り除く。**削除はここ 1 か所だけ**で、その手前に必ず安全弁を通す。
# ---------------------------------------------------------------------------
remove_plugin_dir() { # dir, name -> 0 on success (or nothing to do)
	local dir="$1" name="$2"

	# 入っていない＝することが無い。**成功として返す**（冒頭のコメント参照）。
	if [ ! -d "$dir" ]; then
		say "取り除くものはありません（$dir は存在しません）。"
		REMOVED=""
		return 0
	fi

	# --- 安全弁 -------------------------------------------------------------
	# 名前が一致し、かつ中に殻があること。どちらも満たさないフォルダは、たとえ
	# 指定されても消さない——`Plug-Ins` そのものや、利用者の別のものを巻き込まない
	# ための唯一の歯止めである。
	if [ "$(basename "$dir")" != "$name" ]; then
		fail "取り除ける形になっていません（$dir はプラグインのフォルダではありません）。"
		return 1
	fi
	if [ ! -d "$dir/${name}${VW_SHELL_EXT}" ]; then
		fail "取り除ける形になっていません（$dir に ${name}${VW_SHELL_EXT} がありません）。"
		return 1
	fi

	if ! rm -rf "$dir"; then
		fail "取り除けませんでした: $dir"
		return 1
	fi
	REMOVED="$dir"
	say "取り除きました: $dir"
	return 0
}

# ---------------------------------------------------------------------------
parse_args() {
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--machine) MACHINE=1 ;;
			--name)
				OPT_NAME="${2:-}"
				shift
				;;
			--plugins-dir)
				PLUGINS_DIR="${2:-}"
				shift
				;;
			-h | --help)
				usage
				return 2 # 「使い方を出したので何もしない」の合図
				;;
			--*)
				# 知らないオプション。値らしき次の引数も一緒に捨てる。
				case "${2:-}" in
					'' | --*) : ;;
					*) shift ;;
				esac
				;;
			*) : ;; # 位置引数は使わない
		esac
		shift
	done
	return 0
}

main() {
	# **1 回ごとに結末の状態を初期化する**（PowerShell 版と同じ理由。あちらは同じ
	# セッションで何度も呼ばれて実際に持ち越した）。
	REMOVED=""
	LAST_ERROR=""
	local parsed=0
	parse_args "$@" || parsed=$?
	[ "$parsed" -eq 2 ] && return 0

	local name="$OPT_NAME"
	[ -n "$name" ] || name="$(guess_name "$PLUGINS_DIR")"

	local ok=1
	if [ -z "$name" ]; then
		# 名前が割り出せない＝そこには何も入っていない。**これも成功**（アップデートの
		# 入口で無条件に叩けることが要る）。
		say "取り除くものはありません（$PLUGINS_DIR にプラグインが見つかりません）。"
		ok=0
	else
		local dir
		dir="$(plugin_dir "$PLUGINS_DIR" "$name")"
		if remove_plugin_dir "$dir" "$name"; then
			ok=0
		fi
	fi

	if [ "$MACHINE" -eq 1 ]; then
		if [ "$ok" -eq 0 ]; then
			[ -n "$REMOVED" ] && echo "removed=$REMOVED"
			echo "ok"
		else
			echo "error=${LAST_ERROR:-取り除けませんでした。}"
		fi
		# machine モードは常に 0 で終わる（結末は stdout の行が持つ）。
		return 0
	fi

	[ "$ok" -eq 0 ] && return 0
	printf 'error: %s\n' "${LAST_ERROR:-取り除けませんでした。}" >&2
	return 1
}

# 直接実行したときだけ main を走らせる（テストは source して個々の関数を叩く。
# scripts/vw-install.sh の末尾と同じ作法）。
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
