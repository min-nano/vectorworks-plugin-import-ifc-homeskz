#!/usr/bin/env bash
#
# ci-debug-job.sh — `.github/workflows/ci-debug.yml`（CI debug）のランナー側本体。
#
# ワークフローは「チェックアウト → SDK 用意 → このスクリプトを 1 回実行」だけを行い、
# 実際の調査コマンドはすべてここに集約する。こうしている理由は 2 つ:
#
#   1. workflow_dispatch は「デフォルトブランチに存在するワークフロー」しか起動でき
#      ないため、ワークフロー本体を頻繁に触ると毎回 main へマージする必要が出る。
#      モードの追加・修正をこのスクリプト側に閉じ込めれば、作業ブランチに push する
#      だけで（dispatch の ref がそのブランチなので）すぐ試せる。
#   2. インライン YAML の run: と違い、独立したシェルスクリプトなので shellcheck に
#      そのままかけられる（scripts/lint.sh / lint.yml の shellcheck ジョブ）。
#
# 入力はすべて環境変数（ワークフローが inputs から詰める）:
#
#   MODE        sdk-grep | sdk-ls | build | compile-one | shell
#   PLATFORM    mac | windows | linux
#   ARGS        モードごとの引数（grep パターン / パス / ビルドターゲット）
#   SCRIPT      MODE=shell のときに実行する bash スクリプト本文
#   VW_SDK_DIR  トリミング済み SDK の場所（SDK を使うモードのみ）
#
# 出力は「ペイロードマーカー」で挟んだ 1 ブロックとして stdout に出す:
#
#   ===== BEGIN PAYLOAD (mode=... platform=...) =====
#   ...
#   ===== END PAYLOAD (exit=N lines_total=N truncated=yes|no) =====
#
# 呼び出し側（scripts/ci-debug.sh）はジョブログからこのマーカー間だけを抜き出すので、
# セットアップ手順のノイズを読まずに済む。生の全出力は debug-out/ に残し、ワーク
# フローがアーティファクトとしてアップロードする（人間用の保険。AI は GitHub MCP で
# アーティファクトを取得できないため、必要な情報は必ずログ側に出すこと）。
#
# 終了ステータスは調査コマンドのものをそのまま返す（＝run の conclusion になる）。
#
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

MODE="${MODE:-}"
PLATFORM="${PLATFORM:-mac}"
ARGS="${ARGS:-}"
SCRIPT="${SCRIPT:-}"
SDK="${VW_SDK_DIR:-}"
OUT_DIR="${OUT_DIR:-debug-out}"

# ペイロードに載せる最大行数。これを超えたぶんは切り捨て、END マーカーの
# truncated=yes で「全部は見ていない」ことを呼び出し側に明示する（AI が「該当なし」
# と誤読しないための最重要ポイント）。全文は debug-out/raw.txt に残る。
MAX_LINES="${PAYLOAD_MAX_LINES:-400}"

mkdir -p "$OUT_DIR"
RAW="$OUT_DIR/raw.txt"
PAYLOAD="$OUT_DIR/payload.txt"

# ビルドログのように「全文は長いが、欲しいのは診断行と末尾」という出力は、単純な
# head ではなくダイジェスト（診断行の抜粋＋末尾）にする。モード実装はサブシェルで
# 動く（後述）ので変数を書き戻せない。ここでモードから静的に決める。
case "$MODE" in
	build | compile-one | shell) DIGEST="log" ;;
	*) DIGEST="head" ;;
esac

# ヘッダ全文を読む sdk-ls だけは上限を上げる（SDK のヘッダは 1000 行級のものがある）。
if [ "$MODE" = "sdk-ls" ] && [ -z "${PAYLOAD_MAX_LINES:-}" ]; then
	MAX_LINES=1200
fi

# ---------------------------------------------------------------------------
# 小さなヘルパー
# ---------------------------------------------------------------------------

# die <message>: 使い方の誤り。モード実装はサブシェル内で走るので、この exit は
# スクリプト全体ではなくサブシェルだけを終わらせる。メッセージは（stderr ごと）
# RAW に入り、通常どおりペイロードとして出力される — つまり失敗しても呼び出し側は
# 必ずマーカー付きの理由を受け取れる。
die() {
	echo "ci-debug-job: error: $1" >&2
	exit 2
}

# ncpu: 並列ビルド数。macOS / Linux / Windows(git-bash) のどれでも動くようフォール
# バックを重ねる。
ncpu() {
	if command -v nproc >/dev/null 2>&1; then
		nproc
	elif command -v sysctl >/dev/null 2>&1; then
		sysctl -n hw.ncpu 2>/dev/null || echo 2
	else
		echo "${NUMBER_OF_PROCESSORS:-2}"
	fi
}

# need_sdk: SDK が用意されていることを確かめる（用意はワークフロー側の仕事）。
need_sdk() {
	[ -n "$SDK" ] || die "このモードには SDK が必要です（platform=linux では使えません）"
	[ -d "$SDK/SDKLib/Include" ] || die "SDK が見つかりません: $SDK/SDKLib/Include"
}

# need_args <説明>: ARGS 必須のモード用。
need_args() {
	[ -n "$ARGS" ] || die "このモードには args が必要です（$1）"
}

# cmake_configure <builddir>: プラットフォームに応じた configure。
# linux は SDK が無いので、SDK 非依存のテストビルド（core/ parse/ をカバー）を構成する。
cmake_configure() {
	local dir="$1"
	shift
	case "$PLATFORM" in
		linux)
			cmake -S . -B "$dir" \
				-DCMAKE_BUILD_TYPE=Debug \
				-DVW_BUILD_PLUGIN=OFF \
				-DVW_BUILD_TESTS=ON \
				-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
				"$@"
			;;
		windows)
			cmake -S . -B "$dir" -A x64 \
				-DVW_SDK_DIR="$SDK" \
				"$@"
			;;
		*)
			cmake -S . -B "$dir" \
				-DCMAKE_BUILD_TYPE=Release \
				-DVW_SDK_DIR="$SDK" \
				-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
				"$@"
			;;
	esac
}

# ---------------------------------------------------------------------------
# モード実装。すべて stdout/stderr に出し、呼び出し元が RAW へリダイレクトする。
# ---------------------------------------------------------------------------

# sdk-grep: SDK ヘッダを拡張正規表現で検索する。「この API は SDK にあるか」を
# 確かめる設計調査用で、ローカル（リモートセッションのコンテナ）に SDK が無い以上
# CI 経由でしか答えられない問いに答えるための最重要モード。
mode_sdk_grep() {
	need_sdk
	need_args "検索する拡張正規表現"
	echo "# grep -rnIE '$ARGS' in SDKLib/Include (paths are relative to it)"
	echo
	local status=0
	( cd "$SDK/SDKLib/Include" && grep -rnIE -- "$ARGS" . ) | sed 's#^\./##' || status=$?
	# grep はヒット 0 件で 1 を返す。「見つからなかった」は調査結果であって失敗では
	# ないので run を赤くしない（本当のエラーは 2 以上なのでそれだけ伝播させる）。
	if [ "$status" -eq 1 ]; then
		echo "(no matches)"
		return 0
	fi
	return "$status"
}

# sdk-ls: ARGS がヘッダの実ファイルを指していればその全文、そうでなければパスの
# 部分一致で一覧を出す。grep で当たりを付けてから宣言の前後を読む、という流れ。
mode_sdk_ls() {
	need_sdk
	need_args "ヘッダのパス、またはパスの部分一致文字列"
	local root="$SDK/SDKLib/Include"
	if [ -f "$root/$ARGS" ]; then
		echo "# cat SDKLib/Include/$ARGS"
		echo
		cat "$root/$ARGS"
	else
		echo "# find SDKLib/Include -ipath '*$ARGS*' (paths are relative to it)"
		echo
		( cd "$root" && find . -type f -ipath "*$ARGS*" ) | sed 's#^\./##' | sort
	fi
}

# build: configure してビルドする。ARGS があれば単一ターゲットだけを作る。
# ここで作るのはコンパイル可否の確認だけで、署名・パッケージ・リリース公開は一切
# 行わない（それは build.yml の仕事）。
mode_build() {
	[ "$PLATFORM" = "linux" ] || need_sdk
	local dir="build-dbg"
	echo "# cmake configure ($PLATFORM)"
	cmake_configure "$dir" || return $?
	echo
	echo "# cmake --build ${ARGS:+--target $ARGS}"
	echo
	if [ -n "$ARGS" ]; then
		cmake --build "$dir" --config Release --parallel "$(ncpu)" --target "$ARGS"
	else
		cmake --build "$dir" --config Release --parallel "$(ncpu)"
	fi
}

# compile-one: compile_commands.json から 1 翻訳単位ぶんのコンパイル行を取り出して
# それだけを実行する。フルビルドを待たずに 1 ファイルのエラーを回せる（数十秒）。
# Visual Studio ジェネレータは compile_commands.json を出さないため Windows では
# 使えない（そちらは mode=build を使う）。
mode_compile_one() {
	[ "$PLATFORM" = "windows" ] &&
		die "compile-one は Windows では使えません（VS ジェネレータが compile_commands.json を出さないため）。mode=build を使ってください"
	[ "$PLATFORM" = "linux" ] || need_sdk
	need_args "コンパイルするソースのパス（末尾一致。例 src/parse/Grid.cpp）"
	local dir="build-dbg"
	echo "# cmake configure ($PLATFORM, PCH off)"
	cmake_configure "$dir" -DVW_ENABLE_PCH=OFF || return $?

	local db="$dir/compile_commands.json"
	[ -f "$db" ] || die "compile_commands.json が生成されませんでした: $db"

	local entry
	entry="$(jq -c --arg f "$ARGS" 'map(select(.file | endswith($f))) | .[0] // empty' "$db")"
	[ -n "$entry" ] || die "compile_commands.json に '$ARGS' に一致するエントリがありません"

	local wd cmd
	wd="$(printf '%s' "$entry" | jq -r '.directory')"
	cmd="$(printf '%s' "$entry" | jq -r '.command // (.arguments | map(@sh) | join(" "))')"
	echo
	echo "# compiling: $(printf '%s' "$entry" | jq -r '.file')"
	echo
	( cd "$wd" && bash -c "$cmd" )
}

# shell: 逃げ道。固定モードで表現できない一発調査を bash でそのまま流す。
# SCRIPT は環境変数で渡ってくる（YAML へ展開しないのでクォート事故が起きない）。
mode_shell() {
	[ -n "$SCRIPT" ] || die "mode=shell には script が必要です"
	local f="$OUT_DIR/script.sh"
	printf '%s\n' "$SCRIPT" >"$f"
	echo "# bash $f"
	echo
	bash "$f"
}

# ---------------------------------------------------------------------------
# ペイロード出力
# ---------------------------------------------------------------------------

# digest_log: ビルドログ向けの抜粋。診断行（error/warning/FAILED …）を先に、その後
# 末尾の数十行を出す。並列ビルドではエラーが末尾に来るとは限らないので、単純な
# tail ではなく両方を出している。
digest_log() {
	local hits
	hits="$(grep -nE -- '(^|[^A-Za-z])([Ee]rror|ERROR|FAILED|fatal|undefined (reference|symbols)|error C[0-9]{4}|warning C[0-9]{4})' "$RAW" | head -n 300)"
	if [ -n "$hits" ]; then
		echo "--- diagnostics (max 300 lines, prefixed with the line number in raw.txt) ---"
		printf '%s\n' "$hits"
		echo
	fi
	echo "--- tail of the log (last 80 lines) ---"
	tail -n 80 "$RAW"
}

# emit_payload <exit-status>: マーカーで挟んだ 1 ブロックを stdout と payload.txt へ。
emit_payload() {
	local status="$1" total truncated="no"
	total="$(wc -l <"$RAW" | tr -d ' ')"

	{
		echo "===== BEGIN PAYLOAD (mode=$MODE platform=$PLATFORM) ====="
		if [ "$DIGEST" = "log" ]; then
			digest_log
		else
			head -n "$MAX_LINES" "$RAW"
			if [ "$total" -gt "$MAX_LINES" ]; then
				truncated="yes"
			fi
		fi
		echo "===== END PAYLOAD (exit=$status lines_total=$total truncated=$truncated) ====="
	} >"$PAYLOAD"

	cat "$PAYLOAD"
	emit_annotation
}

# emit_annotation: ペイロードを **チェックラン注釈** としても出す。
#
# なぜ二重に出すか: 呼び出し側がペイロードを取る経路は本来ジョブログだが、ログ API は
# 署名付きの Azure Blob Storage へ 302 で飛ぶ。組織の egress ポリシーがそのホストを
# 拒否している環境（Claude Code のリモートセッションなど）では、コンテナからログ本文を
# 取得できない。一方、注釈は
#
#   GET /repos/{owner}/{repo}/check-runs/{check_run_id}/annotations
#
# つまり api.github.com だけで読めるうえ、ログのノイズ（セットアップ手順・アーティファクト
# アップロード・ポストジョブ後始末）が混ざらない。ワークフローコマンドの仕様で改行は
# %0A へエスケープする必要がある（% と CR も同様）。
#
# **GitHub は注釈のメッセージを 4096 文字ちょうどで切る**（実測）。しかも切り方は
# 単語の途中でも構わない乱暴なもので、そのままだと END マーカーごと消えて「これで
# 全部だ」と誤読される。そこで自前でバイト予算に収め、切り詰めた旨の 1 行と END
# マーカー行を**必ず**収まる形で残す。全文はジョブログとアーティファクトに残る。
#
# END 行には lines_total が入っているので、注釈側が切られていても「本当は何行あった
# のか」は読み手に伝わる。
emit_annotation() {
	local budget="${ANNOTATION_MAX_BYTES:-3800}" total kept body tail_line notice
	total="$(wc -l <"$PAYLOAD" | tr -d ' ')"
	tail_line="$(tail -n 1 "$PAYLOAD")"
	notice="... (annotation truncated by GitHub's 4096-char limit — the full payload is in the job log and the run artifact)"

	# 予算から「切り詰め通知＋END 行」ぶんを引いた範囲まで、行単位で詰める。
	# 文字数ではなくバイト数で数えるため LC_ALL=C（日本語のエラーメッセージ対策）。
	body="$(LC_ALL=C awk -v limit="$((budget - ${#notice} - ${#tail_line} - 4))" '
		{
			len += length($0) + 1
			if (len > limit) { exit }
			print
		}' "$PAYLOAD")"

	kept="$(printf '%s\n' "$body" | wc -l | tr -d ' ')"
	if [ "$kept" -lt "$total" ]; then
		body="$(printf '%s\n%s\n%s' "$body" "$notice" "$tail_line")"
	fi

	body="$(printf '%s\n' "$body" |
		sed -e 's/%/%25/g' -e 's/\r/%0D/g' |
		awk '{printf "%s%%0A", $0}')"
	echo "::notice title=ci-debug payload::${body}"
}

# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------

# モード実装はサブシェルで動かす。die の exit がここで止まるので、使い方の誤りでも
# 必ず emit_payload まで到達する（＝呼び出し側は理由をマーカー付きで受け取れる）。
(
	case "$MODE" in
		sdk-grep) mode_sdk_grep ;;
		sdk-ls) mode_sdk_ls ;;
		build) mode_build ;;
		compile-one) mode_compile_one ;;
		shell) mode_shell ;;
		*) die "未知の mode: '$MODE'（sdk-grep / sdk-ls / build / compile-one / shell）" ;;
	esac
) >"$RAW" 2>&1
STATUS=$?

emit_payload "$STATUS"
exit "$STATUS"
