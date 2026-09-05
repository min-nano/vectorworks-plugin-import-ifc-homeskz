#!/usr/bin/env bash
#
# vw-install.sh — install ONE build of the HomeskzIfcImport Vectorworks plug-in
# into a Vectorworks 2026 Plug-Ins folder (macOS).
#
# **このスクリプトは「配置の手順」そのもので、リリースと一緒に配られる。**
#
#   * 配布 zip の直下（`<name>.vwlibrary` の隣）に同梱される。
#   * リリースのアセットとしても単独で公開される。
#
# なぜ 2 か所かというと、**アップデートの経路と手動インストールの経路の両方で、常に
# 「そのビルドと同じ版の配置手順」が使われるようにする**ため。
#
#   1. 自動アップデート … 走るのは**インストール済みの（＝古い）** `vw-update.sh` で、
#      その中の配置ロジックは当然「古い構成」しか知らない。実際 M21 で本体
#      （`.vwpayload`）が増えたとき、古いアップデータはそれを写さず、利用者は zip を
#      手で落として置き直す羽目になった。そこで `vw-update.sh` は**落とした zip の中の
#      このスクリプトへ配置を委ねる**——配置の知識が新しいビルドの側にあるので、
#      ファイル構成が変わっても利用者の手は要らない。
#   2. 手動インストール … リリースからこのファイルだけを落として実行すれば、その時点の
#      正しい構成で入る（隔離解除・アドホック署名込み）。
#
# したがって **ここに書いてよいのは「配置に関わること」だけ**で、UI もチャンネルの選択も
# 持たない（それは `vw-update.sh` の仕事）。
#
# 配置の規則はひとつ: **zip の直下にあるものを、そのまま置く。**
# ファイル名を列挙しない——列挙した瞬間に「増えたファイルを取りこぼす」という、いま
# 直している事故がそっくり戻ってくる。除くのはこのスクリプト自身だけ。
#
# 置き先は **`Plug-Ins` の直下ではなく、プラグインが自分で持つフォルダ**である:
#
#     <Plug-Ins>/HomeskzIfcImport/HomeskzIfcImport.vwlibrary
#     <Plug-Ins>/HomeskzIfcImport/HomeskzIfcImport.vwpayload
#     <Plug-Ins>/HomeskzIfcImport/vw-uninstall.sh
#
# こうしておくと**そのプラグインのものが 1 か所に閉じる**ので、取り除くのが「フォルダを
# 1 つ消す」で済む（`vw-uninstall.sh`）。Vectorworks が `Plug-Ins` のサブフォルダも
# 読みに行くことは実機で確認済み。
#
# 入れる前に、**いま入っている版を、その版自身のアンインストーラで取り除く**
# （下記 `remove_installed`）。取り除く側が「いま入っている版」の知識を持つのは、
# 置く側が「新しい版」の知識を持つのとちょうど対になっている。
#
# Usage:
#   ./vw-install.sh                          # 最新の stable を入れる
#   ./vw-install.sh --tag dev-feature-x      # そのプレリリースを入れる
#   ./vw-install.sh --zip <file>             # 手元の zip から入れる
#   ./vw-install.sh --from <dir> --machine   # 展開済みから入れる（vw-update.sh 用）
#
# Options:
#   --name <plugin>      HomeskzIfcImport / HomeskzIfcImportDev（既定はアーカイブか
#                        リリースのアセット名から判定する）
#   --plugins-dir <dir>  Plug-Ins（またはプラグインのフォルダそのもの）。既定は
#                        VW_PLUGINS_DIR、無ければ VW2026 のユーザフォルダ
#   --from <dir>         展開済みのディレクトリから入れる（ダウンロードしない）
#   --zip <file>         手元の zip から入れる
#   --url <url>          この zip を落として入れる
#   --tag <tag>          このリリース（既定 "stable"）から落として入れる
#   --machine            機械可読な出力にする（installed-shell= / ok / error=）。
#                        プラグイン側の `vw-update.sh do-install` が使う
#   -h | --help          使い方
#
# **知らないオプションは黙って読み飛ばす。** 新しい `vw-update.sh` が古いリリースの zip に
# 入ったこのスクリプトを呼ぶ、という向きが起こりうるので、増えた引数で落ちないように
# しておく（逆向き——古い呼び出し側と新しいインストーラ——は既定値が吸収する）。
#
# Requirements: macOS only. Uses tools that ship with macOS (curl, plutil, unzip,
# codesign, xattr) — no Homebrew, no `gh`, and because the repository is public,
# no authentication.
#
# Overridable via environment:
#   VW_REPO         owner/repo             (default below)
#   VW_PLUGINS_DIR  Vectorworks Plug-Ins   (default: user folder for VW 2026)
#
set -euo pipefail

VW_REPO="${VW_REPO:-min-nano/vectorworks-plugin-import-ifc-homeskz}"
VW_API="https://api.github.com/repos/${VW_REPO}"

# 殻の入れ物の拡張子。zip の取り違えを弾くのと、アーカイブからプラグイン名を読み取るのに
# 使う（macOS のプラグインは常にこのバンドルとして配られる）。
VW_SHELL_EXT=".vwlibrary"
# 配布 zip のアセット名の末尾。リリースから拾うときの手掛かりで、**プラグイン名が変わっても
# 効く**ようにアセット名そのものではなく末尾で照合する。
VW_ZIP_SUFFIX="${VW_SHELL_EXT}.zip"
# インストール先へ一緒に置くアンインストーラ。**これはインストール先へ置く**——次の
# アップデートで「いま入っている版を、その版自身の知識で取り除く」ために要る
# （下記 remove_installed、および scripts/vw-uninstall.sh の冒頭）。
VW_UNINSTALLER="vw-uninstall.sh"

MACHINE=0
OPT_NAME=""
OPT_FROM=""
OPT_ZIP=""
OPT_URL=""
OPT_TAG=""
PLUGINS_DIR="${VW_PLUGINS_DIR:-$HOME/Library/Application Support/Vectorworks/2026/Plug-Ins}"

# 作業用の一時ディレクトリ（main が作って exit で消す）。
TMP_ROOT=""

# 失敗の理由。machine モードでは "error=<理由>" として出し、人が実行したときは stderr へ
# 出して終了コードで知らせる。
LAST_ERROR=""

fail() { # message -> always 1
	LAST_ERROR="$1"
	return 1
}

# say: 人が実行したときだけ進捗を出す（machine モードは機械可読な行しか出さない）。
say() {
	[ "$MACHINE" -eq 1 ] || printf '%s\n' "$1"
}

usage() {
	cat <<'USAGE'
vw-install.sh — HomeskzIfcImport を Vectorworks 2026 の Plug-Ins へ入れる（macOS）

  ./vw-install.sh                       最新の stable を入れる
  ./vw-install.sh --tag dev-feature-x   そのプレリリースを入れる
  ./vw-install.sh --zip <file>          手元の zip から入れる

  --name <plugin>      HomeskzIfcImport / HomeskzIfcImportDev（既定は自動判定）
  --plugins-dir <dir>  インストール先（既定: VW2026 のユーザフォルダ）
  --from <dir>         展開済みのディレクトリから入れる
  --zip <file>         手元の zip から入れる
  --url <url>          この zip を落として入れる
  --tag <tag>          このリリースから落として入れる（既定: stable）
  --machine            機械可読な出力（installed-shell= / ok / error=）
USAGE
}

# ---------------------------------------------------------------------------
# GitHub REST helpers. **`vw-update.sh` と同じものが写っているのは意図的**で、この
# ファイルは**単独で配られて単独で走る**（リリースから落としてきた 1 枚だけが手元に
# ある）から、他のスクリプトを source できない。
# ---------------------------------------------------------------------------
api_get() { # api-subpath -> path to a temp file holding the JSON, or fail
	local f
	f="$(mktemp)"
	if curl -fsSL --max-time 20 --retry 2 -H "Accept: application/vnd.github+json" \
		"${VW_API}/$1" -o "$f"; then
		printf '%s' "$f"
	else
		rm -f "$f"
		return 1
	fi
}

jval() { # json-file, keypath -> raw scalar value (empty if missing)
	plutil -extract "$2" raw -o - "$1" 2>/dev/null || true
}

# release_zip: リリースの JSON から配布 zip を 1 つ選び、"<url><TAB><plugin-name>" を返す。
# 照合は 2 段構え——`want` が与えられていればその名前で厳密に、そうでなければ**末尾が
# "<VW_ZIP_SUFFIX>" のアセット**を拾う。後者があるおかげで、アセット名（プラグイン名）が
# 将来変わってもこのスクリプトはそのまま追随できる。
release_zip() { # json-file, wanted-name (may be empty) -> "url<TAB>plugin-name"
	local f="$1" want="$2" j=0 nm url
	while [ "$j" -lt 30 ]; do
		nm="$(jval "$f" "assets.${j}.name")"
		[ -n "$nm" ] || break
		if [ -n "$want" ]; then
			if [ "$nm" = "${want}${VW_ZIP_SUFFIX}" ]; then
				url="$(jval "$f" "assets.${j}.browser_download_url")"
				printf '%s\t%s' "$url" "$want"
				return 0
			fi
		else
			case "$nm" in
				*"${VW_ZIP_SUFFIX}")
					url="$(jval "$f" "assets.${j}.browser_download_url")"
					printf '%s\t%s' "$url" "${nm%"${VW_ZIP_SUFFIX}"}"
					return 0
					;;
			esac
		fi
		j=$((j + 1))
	done
	return 1
}

download() { # url, out-file
	curl -fL --retry 3 --max-time 300 "$1" -o "$2"
}

# ---------------------------------------------------------------------------
# 配置。**このスクリプトの本体で、ここだけが「プラグインがどんなファイルでできているか」を
# 知っている。**
# ---------------------------------------------------------------------------

# place_entry: 1 つ置く。書き込みは必ず「別名へ書いてから差し替える」——走っている
# Vectorworks が途中まで書かれたファイルを掴まないようにするため（殻は起動時にしか
# 読まないが、本体 <name>.vwpayload は動作中に読み直される。src/PayloadSession.cpp）。
place_entry() { # src, dst -> 0 on success
	local src="$1" dst="$2"

	# Gatekeeper: 隔離フラグを外し、アドホック署名を当て直す。unzip した直後のバイナリは
	# Apple Silicon では署名が無いと読み込めない。署名できない中身（テキストなど）で
	# 失敗しても構わないので握り潰す。
	xattr -dr com.apple.quarantine "$src" 2>/dev/null || true
	if [ -d "$src" ]; then
		codesign --force --deep --sign - "$src" >/dev/null 2>&1 || true
	else
		codesign --force --sign - "$src" >/dev/null 2>&1 || true
	fi

	rm -rf "$dst.new"
	if ! cp -R "$src" "$dst.new"; then
		fail "インストール先へのコピーに失敗しました（$(basename "$dst")）。"
		return 1
	fi
	if [ -d "$src" ]; then
		# ディレクトリは上書き rename ができないので、いったん外してから入れ替える。
		rm -rf "$dst"
		if ! mv "$dst.new" "$dst"; then
			fail "インストール先への差し替えに失敗しました（$(basename "$dst")）。"
			return 1
		fi
	else
		# ファイルは rename 1 発で差し替わる（＝途中の状態が見えない）。
		if ! mv -f "$dst.new" "$dst"; then
			fail "インストール先への差し替えに失敗しました（$(basename "$dst")）。"
			return 1
		fi
	fi
	return 0
}

# plugin_dir: 実際に置くフォルダ。**プラグインは自分のフォルダを 1 つ持つ**
# （`<Plug-Ins>/<name>/`。冒頭のコメント参照）。
#
# **渡された先が既にそのフォルダなら足さない。** 自動アップデートのとき、プラグインは
# 「いま自分が読み込まれたフォルダ」を渡してくる——サブフォルダ化のあとはそれ自身が
# `<Plug-Ins>/<name>` なので、無条件に足すと更新のたびに `<name>/<name>/…` と際限なく
# 深くなる。`vw-uninstall.sh` の同名関数と**同じ規則**でなければならない（片方だけ
# 変えると、入れた場所と消す場所が食い違う）。
plugin_dir() { # plugins-dir, name -> the plug-in's own folder
	local root="$1" name="$2"
	if [ "$(basename "$root")" = "$name" ]; then
		printf '%s' "$root"
	else
		printf '%s' "$root/$name"
	fi
}

# remove_installed: **いま入っている版を、その版自身が置いていったアンインストーラで
# 取り除く。** 置く側が「新しい版」の知識を持つのと対で、取り除く側は「いま入っている
# 版」の知識を持つ（scripts/vw-uninstall.sh の冒頭）。
#
# 見つからなければ何もしない（初回インストール、あるいはこの仕組みより前の版）。
# 失敗しても**続行する**——このあとどのみち上書きするので、取り除けなかったことを
# 理由にインストールごと失敗させるのは損。
#
# **一時ディレクトリへ写してから走らせる。** アンインストーラは自分が消すフォルダの中に
# 居るので、その場で走らせると「実行中のスクリプトを消す」ことになる。
#
# （なお、走っている `vw-update.sh` は殻のバンドルの中に居るので、これも一緒に消える。
# POSIX では開いたままの fd から読み続けられるので走り切れる——M21 以来、殻の入れ替えで
# 同じことが起きていて実機で問題は出ていない。）
remove_installed() { # dest-dir, name
	local dest="$1" name="$2"
	local src="$dest/$VW_UNINSTALLER"
	[ -f "$src" ] || return 0

	# 写し先は**この関数が自分で作る**。main の一時ディレクトリを当てにすると、直接
	# 呼ばれたとき（テスト、あるいは将来の呼び出し）に空文字を掴んで `/` へ書きに行く
	# ——root なら成功してしまい、テストが「通ったのに何もしていない」状態になる（実際に
	# それで CI と手元の結果が食い違った）。
	local tmp
	tmp="$(mktemp -d)" || return 0
	local copy="$tmp/$VW_UNINSTALLER"
	if cp "$src" "$copy" 2>/dev/null; then
		say "いま入っている版を取り除きます…"
		/bin/bash "$copy" --machine --name "$name" --plugins-dir "$dest" >/dev/null 2>&1 || true
	fi
	rm -rf "$tmp"
	return 0
}

# install_tree: 展開済みディレクトリの直下にあるものを、そのままプラグインのフォルダへ
# 置く。**列挙しない**のが肝（冒頭のコメント参照）。
install_tree() { # work-dir, plugin-name -> 0 on success
	local work="$1" name="$2"

	# 取り違えた zip を黙って撒かないための最低限の確認。殻はどのビルドにも必ず入って
	# いる（これが無ければそもそもプラグインではない）。**取り除くより先に確かめる**
	# ——ここで弾けなかったら、消しただけで入れられない状態になる。
	if [ ! -d "$work/${name}${VW_SHELL_EXT}" ]; then
		fail "${name}${VW_SHELL_EXT} がアーカイブ内に見つかりません。"
		return 1
	fi

	local dest
	dest="$(plugin_dir "$PLUGINS_DIR" "$name")"

	# 入れる前に前の版を取り除く（上記）。
	remove_installed "$dest" "$name"

	if ! mkdir -p "$dest"; then
		fail "インストール先を作成できませんでした: $dest"
		return 1
	fi

	local entry base
	for entry in "$work"/*; do
		[ -e "$entry" ] || continue
		base="$(basename "$entry")"
		case "$base" in
			# インストーラ自身は置かない（プラグインの一部ではない）。**アンインストーラは
			# 置く**——次のアップデートがこれを使う（remove_installed）。
			vw-install.sh | vw-install.ps1) continue ;;
			# unzip / macOS が残す残骸。
			__MACOSX | .DS_Store) continue ;;
		esac
		place_entry "$entry" "$dest/$base" || return 1
		say "  置きました: $base"
	done
	return 0
}

# installed_shell_id: いま置いた殻の ID（Info.plist の VWShellId）。**「アップデートに
# Vectorworks の再起動が要るか」を決める鍵**で、プラグインは自分にコンパイルされた
# VW_SHELL_ID と突き合わせる——一致するなら本体（.vwpayload）を読み直すだけで反映される
# （src/PayloadAbi.h / src/UpdaterParse.h）。読めなければ空＝プラグインは安全側
# （再起動が要る）へ倒す。
installed_shell_id() { # plugin-name -> id or ""
	local plist
	plist="$(plugin_dir "$PLUGINS_DIR" "$1")/$1${VW_SHELL_EXT}/Contents/Info.plist"
	if [ -f "$plist" ]; then
		/usr/libexec/PlistBuddy -c "Print :VWShellId" "$plist" 2>/dev/null || echo ""
	else
		echo ""
	fi
}

# ---------------------------------------------------------------------------
# 取ってくる → 展開する。
# ---------------------------------------------------------------------------

# guess_name: 展開済みディレクトリから殻を探し、その名前をプラグイン名として返す。
# `--name` が無いとき（手動インストール）に使う。
guess_name() { # dir -> plugin-name or ""
	local entry base
	for entry in "$1"/*"${VW_SHELL_EXT}"; do
		[ -e "$entry" ] || continue
		base="$(basename "$entry")"
		printf '%s' "${base%"${VW_SHELL_EXT}"}"
		return 0
	done
	printf ''
}

# prepare_source: 引数に応じて「展開済みディレクトリ」と「プラグイン名」を用意する。
# 結果は PREPARED_DIR / PREPARED_NAME へ入れて返す（bash の戻り値は 1 つしかない）。
PREPARED_DIR=""
PREPARED_NAME=""

prepare_source() { # -> 0 on success
	PREPARED_DIR=""
	PREPARED_NAME="$OPT_NAME"

	# 1) 展開済みを渡された（アップデータからの呼び出し）。
	if [ -n "$OPT_FROM" ]; then
		if [ ! -d "$OPT_FROM" ]; then
			fail "指定されたディレクトリがありません: $OPT_FROM"
			return 1
		fi
		PREPARED_DIR="$OPT_FROM"
		[ -n "$PREPARED_NAME" ] || PREPARED_NAME="$(guess_name "$OPT_FROM")"
		if [ -z "$PREPARED_NAME" ]; then
			fail "アーカイブ内にプラグインが見つかりません。"
			return 1
		fi
		return 0
	fi

	# 2) zip / URL / リリースのいずれかから zip を得る。
	local zip="$OPT_ZIP"
	if [ -z "$zip" ]; then
		local url="$OPT_URL"
		if [ -z "$url" ]; then
			local tag="${OPT_TAG:-stable}"
			local f
			if ! f="$(api_get "releases/tags/$tag")"; then
				fail "リリース '$tag' を取得できませんでした。"
				return 1
			fi
			local found
			found="$(release_zip "$f" "$OPT_NAME" || true)"
			rm -f "$f"
			if [ -z "$found" ]; then
				fail "リリース '$tag' に配布 zip が見つかりません。"
				return 1
			fi
			url="${found%%$'\t'*}"
			[ -n "$PREPARED_NAME" ] || PREPARED_NAME="${found#*$'\t'}"
			say "リリース '$tag' から ${PREPARED_NAME} を取得します。"
		fi
		zip="$TMP_ROOT/download.zip"
		if ! download "$url" "$zip"; then
			fail "ダウンロードに失敗しました。"
			return 1
		fi
	fi

	local work="$TMP_ROOT/unpacked"
	mkdir -p "$work"
	if ! unzip -q "$zip" -d "$work" >/dev/null 2>&1; then
		fail "アーカイブの展開に失敗しました。"
		return 1
	fi
	PREPARED_DIR="$work"
	[ -n "$PREPARED_NAME" ] || PREPARED_NAME="$(guess_name "$work")"
	if [ -z "$PREPARED_NAME" ]; then
		fail "アーカイブ内にプラグインが見つかりません。"
		return 1
	fi
	return 0
}

# parse_args: 引数を読む。**知らないオプションは読み飛ばす**（冒頭のコメント参照）。
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
			--from)
				OPT_FROM="${2:-}"
				shift
				;;
			--zip)
				OPT_ZIP="${2:-}"
				shift
				;;
			--url)
				OPT_URL="${2:-}"
				shift
				;;
			--tag)
				OPT_TAG="${2:-}"
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

# ---------------------------------------------------------------------------
main() {
	local parsed=0
	parse_args "$@" || parsed=$?
	[ "$parsed" -eq 2 ] && return 0

	TMP_ROOT="$(mktemp -d)"
	# shellcheck disable=SC2064  # 展開はいま行う（trap の実行時ではなく）。
	trap "rm -rf '$TMP_ROOT'" EXIT

	local ok=1
	if prepare_source && install_tree "$PREPARED_DIR" "$PREPARED_NAME"; then
		ok=0
	fi

	if [ "$MACHINE" -eq 1 ]; then
		# 機械可読な結末。**プラグインはこの行しか読まない**（src/UpdaterParse.h）。
		# 判断できない情報は行ごと出さない。
		if [ "$ok" -eq 0 ]; then
			local shell_id
			shell_id="$(installed_shell_id "$PREPARED_NAME")"
			[ -n "$shell_id" ] && echo "installed-shell=$shell_id"
			echo "ok"
		else
			echo "error=${LAST_ERROR:-インストールに失敗しました。}"
		fi
		# machine モードは常に 0 で終わる（結末は stdout の行が持つ）。
		return 0
	fi

	if [ "$ok" -eq 0 ]; then
		printf '%s\n' "インストールしました: $(plugin_dir "$PLUGINS_DIR" "$PREPARED_NAME")"
		printf '%s\n' "Vectorworks を起動してください（起動中なら再起動してください）。"
		return 0
	fi
	printf 'error: %s\n' "${LAST_ERROR:-インストールに失敗しました。}" >&2
	return 1
}

# 直接実行したときだけ main を走らせる（テストは source して個々の関数を叩く。
# scripts/vw-update.sh の末尾と同じ作法）。
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
