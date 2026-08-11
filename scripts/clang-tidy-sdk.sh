#!/usr/bin/env bash
#
# clang-tidy-sdk.sh — SDK 依存の翻訳単位に clang-tidy をかける（コア数ぶん並列）。
#
# なぜこれがあるか
# ----------------
# build.yml の mac / Windows ジョブは、どちらも「Vectorworks SDK が要るので
# lint.yml（SDK 非依存・Linux）では見られないコード」を同じ .clang-tidy の規則で
# 解析する。その 2 ステップは元々インラインの run: に直書きされていて、2 つの問題を
# 抱えていた。
#
#   1. **対象ファイルの一覧が 2 か所にあった。** 片方だけ直せばもう片方が黙って
#      置いていかれる（CLAUDE.md「重複を作らない置き場所」）。一覧はここに 1 つだけ置く。
#
#   2. **clang-tidy を 1 プロセスで直列に回していた。** clang-tidy はビルドを走らせずに
#      解析するため PCH を使えず（CMakeLists.txt の VW_ENABLE_PCH 参照）、翻訳単位ごとに
#      SDK のアンブレラヘッダを丸ごと解析し直す。1 本あたり数十秒かかるので、直列だと
#      これがそのまま積み上がる。実測（2026-08、15 TU）で
#      **Windows ジョブは 12 分 41 秒のうち 10 分 10 秒、mac ジョブは 7 分 44 秒のうち
#      6 分 40 秒**がこのステップだった。翻訳単位どうしは独立なので、ランナーのコア数
#      だけ並列に回せばそのぶん短くなる（ランナーは Windows 4 コア / mac 3 コア）。
#
# 出力は**ファイル一覧の順**にまとめて出す。並列実行の完了順に垂れ流すと実行ごとに
# 並びが変わって差分が読めなくなるため、いったんファイルごとのログへ落としてから
# 順に流す（CLAUDE.md「決定性を守る」）。
#
# 並列には**2 段**ある。ランナー 1 台の中でコア数ぶん同時に回す（-j）のに加えて、
# 対象そのものを複数のジョブへ分けられる（-s）。前者はコア数で頭打ちになる
# （実測で 4 コアのランナーは 4 並列でも 2.5 倍程度しか出ない）ので、そこから先を
# 縮めたければランナーを増やすしかない。-s はそのためにある。
#
# 使い方:
#   scripts/clang-tidy-sdk.sh -p <compile-db-dir> [-t <clang-tidy>] [-j <jobs>]
#                             [-s <index>/<total>] [-x <extra clang-tidy arg>]...
#
#     -p DIR   compile_commands.json のあるディレクトリ（必須）。PCH 無しで
#              configure したものを渡すこと（VW_ENABLE_PCH=OFF）。
#     -t PATH  clang-tidy の実体（既定: PATH 上の clang-tidy）
#     -j N     1 ランナー内の並列数（既定: ランナーのコア数）
#     -s I/N   翻訳単位を N 分割したうちの I 番目だけを解析する（1 始まり）。
#              build.yml が matrix から渡す。分け方はラウンドロビンなので、
#              全シャードを合わせるとちょうど全体になり、重複も漏れも無い。
#     -x ARG   clang-tidy へそのまま渡す追加引数。複数回指定できる。
#              Windows は SDK のテンプレートヘッダを通すために
#              -x --extra-arg=-fdelayed-template-parsing が要る（build.yml 参照）。
#
# 1 本でも診断が出れば非ゼロで終わる（--warnings-as-errors='*' は下で常に付ける）。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

TIDY="clang-tidy"
DB=""
JOBS=""
SHARD=""
EXTRA=()

usage() {
	sed -n '/^# 使い方:/,/^$/p' "$0" | sed 's/^#\{1,2\} \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		-p)
			DB="${2:-}"
			shift 2
			;;
		-t)
			TIDY="${2:-}"
			shift 2
			;;
		-j)
			JOBS="${2:-}"
			shift 2
			;;
		-s)
			SHARD="${2:-}"
			shift 2
			;;
		-x)
			EXTRA+=("${2:-}")
			shift 2
			;;
		-h | --help)
			usage
			exit 0
			;;
		*)
			echo "clang-tidy-sdk.sh: 不明な引数: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

if [ -z "$DB" ]; then
	echo "clang-tidy-sdk.sh: -p <compile-db-dir> は必須です" >&2
	exit 2
fi
if [ ! -f "$DB/compile_commands.json" ]; then
	echo "clang-tidy-sdk.sh: $DB/compile_commands.json がありません" >&2
	exit 2
fi

# ncpu: ランナーのコア数。macOS / Linux / Windows(git-bash) のどれでも動くよう
# フォールバックを重ねる。Linux にも sysctl はあるが hw.ncpu は無いので落ちて nproc へ。
ncpu() {
	if sysctl -n hw.ncpu 2>/dev/null; then
		:
	elif command -v nproc >/dev/null 2>&1; then
		nproc
	else
		echo "${NUMBER_OF_PROCESSORS:-2}"
	fi
}

if [ -z "$JOBS" ]; then
	JOBS="$(ncpu)"
fi
case "$JOBS" in
	'' | *[!0-9]* | 0)
		echo "clang-tidy-sdk.sh: -j には正の整数を指定してください（'$JOBS'）" >&2
		exit 2
		;;
esac

# --- 解析する翻訳単位（唯一の定義） ----------------------------------------
#
# SDK 依存のコードだけをここに置く。src/draw/ は**グロブで拾う**: あそこのモジュールは
# 定義上すべて SDK 依存なので（CLAUDE.md「依存の向きは厳守する」）、新しい draw モジュールが
# 増えた瞬間に解析対象へ入る。残りの 3 本は SDK にしか触れない glue で、マイルストーンごとに
# 増えたりはしない。
#
# src/UpdaterFlow.cpp は入れない。Vectorworks のヘッダを 1 つも include せず GS_MAC /
# GS_WIN の分岐も無いので、SDK の要らない lint.yml が同じ規則で先に解析している。
# （高価な SDK ジョブ 2 つで解析し直しても、Linux ジョブが見逃すものは何も出なかった。）
FILES=(src/draw/*.cpp src/ModuleMain.cpp src/Extensions/ExtMenu.cpp src/Updater.cpp)

TOTAL="${#FILES[@]}"
SHARD_LABEL=""

# --- -s I/N: 対象を N 台のランナーへ分ける ----------------------------------
#
# 割り当ては**ラウンドロビン**（下の -j と同じ理屈）。1 翻訳単位あたりの時間は SDK
# ヘッダの解析に支配されていてどれもほぼ同じなので、静的に配るだけで実質最適に詰まる。
# 全シャードの和はちょうど元の一覧になるので、重複も漏れも起きない。
if [ -n "$SHARD" ]; then
	case "$SHARD" in
		*/*) ;;
		*)
			echo "clang-tidy-sdk.sh: -s は I/N の形で指定してください（'$SHARD'）" >&2
			exit 2
			;;
	esac
	SHARD_INDEX="${SHARD%%/*}"
	SHARD_TOTAL="${SHARD##*/}"
	case "$SHARD_INDEX$SHARD_TOTAL" in
		'' | *[!0-9]*)
			echo "clang-tidy-sdk.sh: -s の I と N は正の整数で（'$SHARD'）" >&2
			exit 2
			;;
	esac
	if [ "$SHARD_INDEX" -lt 1 ] || [ "$SHARD_TOTAL" -lt 1 ] ||
		[ "$SHARD_INDEX" -gt "$SHARD_TOTAL" ]; then
		echo "clang-tidy-sdk.sh: -s は 1 <= I <= N を満たすこと（'$SHARD'）" >&2
		exit 2
	fi
	# 分割数が翻訳単位より多いと空のシャードができる。黙って「成功」にすると
	# 「何も解析していないのに緑」になるので、設定の誤りとして落とす。
	if [ "$SHARD_TOTAL" -gt "$TOTAL" ]; then
		echo "clang-tidy-sdk.sh: 分割数 $SHARD_TOTAL が翻訳単位の数 $TOTAL を超えています" >&2
		exit 2
	fi
	SHARDED=()
	k=$((SHARD_INDEX - 1))
	while [ "$k" -lt "$TOTAL" ]; do
		SHARDED+=("${FILES[$k]}")
		k=$((k + SHARD_TOTAL))
	done
	FILES=("${SHARDED[@]}")
	SHARD_LABEL=" (shard $SHARD_INDEX of $SHARD_TOTAL, out of $TOTAL total)"
fi

# 実際に叩くコマンド。--warnings-as-errors は呼び出し側に任せず必ず付ける（両ジョブで
# 同一にするため）。空配列の展開は bash 3.2 の `set -u` で落ちるので、要素があるときだけ
# 足して、以降は常に非空の配列として展開する。
CMD=("$TIDY" -p "$DB" --warnings-as-errors='*')
if [ "${#EXTRA[@]}" -gt 0 ]; then
	CMD+=("${EXTRA[@]}")
fi

echo "Tidying ${#FILES[@]} SDK-dependent translation units with $JOBS parallel jobs$SHARD_LABEL:"
printf '  %s\n' "${FILES[@]}"
"$TIDY" --version | sed 's/^/  /'

LOGDIR="$(mktemp -d)"
trap 'rm -rf "$LOGDIR"' EXIT

# シャードは**ラウンドロビン**で割り当てる。1 翻訳単位あたりの時間は SDK ヘッダの
# 解析に支配されていてどれもほぼ同じなので、静的に配るだけで実質最適に詰まる。
# （bash 3.2 の macOS では `wait -n` が使えず、動的なワークキューは書けない。）
run_shard() {
	local shard="$1" k f start rc
	k="$shard"
	while [ "$k" -lt "${#FILES[@]}" ]; do
		f="${FILES[$k]}"
		start="$(date +%s)"
		"${CMD[@]}" "$f" >"$LOGDIR/$k.log" 2>&1
		rc="$?"
		echo "$rc" >"$LOGDIR/$k.rc"
		echo "$(($(date +%s) - start))" >"$LOGDIR/$k.sec"
		k=$((k + JOBS))
	done
}

i=0
while [ "$i" -lt "$JOBS" ]; do
	run_shard "$i" &
	i=$((i + 1))
done
wait

# --- 結果をファイル一覧の順に出す -------------------------------------------
status=0
failed=""
k=0
while [ "$k" -lt "${#FILES[@]}" ]; do
	rc="$(cat "$LOGDIR/$k.rc" 2>/dev/null || echo 1)"
	sec="$(cat "$LOGDIR/$k.sec" 2>/dev/null || echo '?')"
	echo "----- ${FILES[$k]} (${sec}s, exit=$rc) -----"
	if [ -s "$LOGDIR/$k.log" ]; then
		cat "$LOGDIR/$k.log"
	fi
	if [ "$rc" -ne 0 ]; then
		status=1
		failed="$failed ${FILES[$k]}"
	fi
	k=$((k + 1))
done

if [ "$status" -ne 0 ]; then
	echo "::error::clang-tidy failed on:$failed"
fi
exit "$status"
