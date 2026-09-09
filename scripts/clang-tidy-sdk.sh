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
#   3. **同じ入力を何度も解析し直していた。** PR は同じブランチへ何度も push されるが、
#      1 コミットが触るのはたいてい数ファイルである。それでも毎回 41 本すべてを解析して
#      いた。コンパイルの側は ccache が「入力が同じなら結果を使い回す」で解いているのに、
#      clang-tidy にはそれが無い——**出力がオブジェクトではなく診断だから**である。
#      そこで同じ原理を自前で持つ（-c）。翻訳単位の入力すべてを 1 つの鍵にまとめ
#      （scripts/tidy-cache-key.py）、「この入力では診断が 1 つも出なかった」という事実
#      だけを控える。次に同じ鍵が出た翻訳単位は解析ごと省ける。
#
#      **鍵に漏れがあると「直したのに緑」になる**ので、何を鍵に入れているか・なぜ多めに
#      拾う側へ倒すのかは tidy-cache-key.py の冒頭に書いてある。控えるのは
#      **きれいに通ったときだけ**（診断が 1 行でも出たら控えない）で、鍵を出せない
#      翻訳単位は必ず解析する。
#
# 並列には**2 段**ある。ランナー 1 台の中でコア数ぶん同時に回す（-j）のに加えて、
# 対象そのものを複数のジョブへ分けられる（-s）。前者はコア数で頭打ちになる
# （実測で 4 コアのランナーは 4 並列でも 2.5 倍程度しか出ない）ので、そこから先を
# 縮めたければランナーを増やすしかない。-s はそのためにある。
#
# 使い方:
#   scripts/clang-tidy-sdk.sh -p <compile-db-dir> [-t <clang-tidy>] [-j <jobs>]
#                             [-s <index>/<total>] [-c <cache-dir>]
#                             [-x <extra clang-tidy arg>]...
#
#     -p DIR   compile_commands.json のあるディレクトリ（必須）。PCH 無しで
#              configure したものを渡すこと（VW_ENABLE_PCH=OFF）。
#     -t PATH  clang-tidy の実体（既定: PATH 上の clang-tidy）
#     -j N     1 ランナー内の並列数（既定: ランナーのコア数）
#     -s I/N   翻訳単位を N 分割したうちの I 番目だけを解析する（1 始まり）。
#              build.yml が matrix から渡す。分け方はラウンドロビンなので、
#              全シャードを合わせるとちょうど全体になり、重複も漏れも無い。
#     -c DIR   結果キャッシュの置き場所。指定すると、前に同じ入力できれいに通った
#              翻訳単位を解析せずに飛ばす（上記 3）。python3 が要る——無ければ
#              キャッシュを使わずに全部解析する（黙って遅くなるだけで、結果は同じ）。
#              build.yml が actions/cache で実行をまたいで持ち回る。
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
CACHE_DIR=""
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
		-c)
			CACHE_DIR="${2:-}"
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
# SDK 依存のコードだけをここに置く。**ディレクトリ単位はグロブで拾う**——中のモジュールが
# 定義上すべて SDK 依存で（CLAUDE.md「依存の向きは厳守する」）、マイルストーンごとに増える
# ところだからである。
#
#   * src/draw/*.cpp       … 描画モジュール。要素を 1 つ足すたびに 1 本増える。
#   * src/Extensions/*.cpp … SDK 拡張（メニューと自作 PIO の本体）。PIO も要素ごとに増える。
#
# **名前で列挙すると、足したファイルが黙って解析されないまま残る。** 実際 M12 の
# ExtColumnMark.cpp と M19 の ExtShearWall.cpp は、ここが ExtMenu.cpp だけの列挙だったせいで
# 一度も clang-tidy にかかっていなかった（PIO 本体は draw/ と同じ SDK の API を叩くので、
# 同じ規則で見るべきコードである）。グロブなら次に PIO が増えても取りこぼさない。
#
#   * src/payload/*.cpp    … 本体（ペイロード）の入口。殻との境界（src/PayloadAbi.h）。
#
# 残る 5 本は増えない glue なので名前で置く（PayloadHost / PayloadSession は SDK の型を
# 使わないが、PluginPrefix.h を通しビルドの構成も殻と同じなので、ここで一緒に見る。
# FeedbackLoopHost は M24 の往復の駆動を殻の道具へ結ぶ側で、gSDK を触る）。
#
# src/UpdaterFlow.cpp と src/FeedbackLoop.cpp は入れない。Vectorworks のヘッダを 1 つも include せず GS_MAC /
# GS_WIN の分岐も無いので、SDK の要らない lint.yml が同じ規則で先に解析している。
# （高価な SDK ジョブ 2 つで解析し直しても、Linux ジョブが見逃すものは何も出なかった。）
FILES=(src/draw/*.cpp src/Extensions/*.cpp src/payload/*.cpp src/ModuleMain.cpp src/Updater.cpp
	   src/PayloadHost.cpp src/PayloadSession.cpp src/FeedbackLoopHost.cpp)

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

# **clang-tidy へ渡す引数はまるごと鍵に入れる。** -x で来たものだけでなく、この
# スクリプトが常に付ける --warnings-as-errors='*' も、将来ここへ足す引数（--header-filter
# など）も含める——実行ファイルと -p DB を除いた残り（CMD の 4 つ目から）がそれである。
# こうしておくと**引数を足したときに鍵が自動で変わる**ので、SCHEME を上げ忘れて古い控えが
# 生き残る、ということが起きない。CMD は必ず 4 要素以上あるので、空配列の心配は要らない。
TIDY_ARGS="${CMD[*]:3}"

# **ツールチェインの標準ヘッダの同一性。** Xcode の SDK・MSVC の STL・Windows SDK は
# -I ではなくコンパイラの既定の検索パスから来るので、鍵の include 走査には映らない。
# 数万ファイルをハッシュするわけにはいかないので、**どのランナーイメージで走っているか**
# で代表させる（GitHub が ImageOS / ImageVersion を渡す）。イメージが更新された実行は
# 全件が外れる——ヘッダが実際に入れ替わっているのだから、それが正しい。
# 手元では両方とも未設定なので空のままで、値は安定する。
IMAGE_KEY=""
if [ -n "${ImageOS:-}${ImageVersion:-}" ]; then
	IMAGE_KEY="${ImageOS:-}-${ImageVersion:-}"
fi

# clang-tidy の版。表示には出力をそのまま使うが、**鍵には版の番号だけを入れる**。
#
# `--version` の出力には `Host CPU: apple-m1` のような**実行機ごとに変わる行**があり、
# GitHub のランナープールは機種が混在している。出力を丸ごと鍵に入れると、**実行のたびに
# 別の鍵になって 1 件も再利用されない**——実機で実際にそうなった（キャッシュ自体は
# primary key で復元できているのに `0 reused / 21 analysed`）。速さのための仕組みが
# 静かに何もしなくなる、といういちばん気付きにくい壊れ方なので、抜き出す側で潰す。
#
# 番号を取り出せない相手（見慣れない綴りを出すツール）のときだけ出力をそのまま使う。
# 実行機ごとに鍵が変わるかもしれないが、**結果は変わらない**（使い回さないだけ）。
TIDY_VERSION_FULL="$("$TIDY" --version 2>&1)"
TIDY_VERSION="$(printf '%s\n' "$TIDY_VERSION_FULL" |
	sed -n 's/.*LLVM version \([0-9][0-9.]*\).*/\1/p' | head -n 1)"
if [ -z "$TIDY_VERSION" ]; then
	TIDY_VERSION="$TIDY_VERSION_FULL"
fi

# --- 結果キャッシュ（-c）の下ごしらえ ----------------------------------------
#
# **使えないと分かったら黙って諦める。** キャッシュはあくまで速さのための飾りで、
# 無くても結果は 1 ビットも変わらない——python3 が無い・ディレクトリを作れない、
# といった場面でジョブを落とす理由が無い。
PYTHON=""
if [ -n "$CACHE_DIR" ]; then
	# **見つかるだけでは足りない。** Windows の PATH には Microsoft Store を開くだけの
	# python3.exe（0 バイトのスタブ）が載っていることがあり、command -v はそれを見つけて
	# しまう。実際に走らせて確かめる——さもないと**いちばん効かせたい Windows のジョブで
	# だけ静かにキャッシュが死ぬ**。
	for candidate in python3 python; do
		if command -v "$candidate" >/dev/null 2>&1 &&
			"$candidate" -c 'import hashlib, json' >/dev/null 2>&1; then
			PYTHON="$candidate"
			break
		fi
	done
	if [ -z "$PYTHON" ]; then
		echo "clang-tidy-sdk.sh: python3 が無いので結果キャッシュを使いません（全部解析します）" >&2
		CACHE_DIR=""
	elif ! mkdir -p "$CACHE_DIR" 2>/dev/null; then
		echo "clang-tidy-sdk.sh: $CACHE_DIR を作れないので結果キャッシュを使いません" >&2
		CACHE_DIR=""
	fi
fi

# 走る前に、復元された控えが何件あるかを数えておく。**「控えは届いているのに 1 件も
# 使い回さない」を見つけるため**で、これは実際に起きた壊れ方である（鍵に実行機ごとに
# 変わるものが混ざっていた。上記 TIDY_VERSION）。結果は正しいままなので CI は緑になり、
# 誰も気付かない——だから数えて、下で言わせる。
CACHE_RESTORED=0
if [ -n "$CACHE_DIR" ]; then
	CACHE_RESTORED="$(find "$CACHE_DIR" -type f 2>/dev/null | awk 'END {print NR}')"
	[ -n "$CACHE_RESTORED" ] || CACHE_RESTORED=0
fi

echo "Tidying ${#FILES[@]} SDK-dependent translation units with $JOBS parallel jobs$SHARD_LABEL:"
printf '  %s\n' "${FILES[@]}"
printf '%s\n' "$TIDY_VERSION_FULL" | sed 's/^/  /'
if [ -n "$CACHE_DIR" ]; then
	echo "  result cache: $CACHE_DIR"
fi

LOGDIR="$(mktemp -d)"
trap 'rm -rf "$LOGDIR"' EXIT

# シャードは**ラウンドロビン**で割り当てる。1 翻訳単位あたりの時間は SDK ヘッダの
# 解析に支配されていてどれもほぼ同じなので、静的に配るだけで実質最適に詰まる。
# （bash 3.2 の macOS では `wait -n` が使えず、動的なワークキューは書けない。）
run_shard() {
	local shard="$1" k f start rc key
	k="$shard"
	while [ "$k" -lt "${#FILES[@]}" ]; do
		f="${FILES[$k]}"

		# この翻訳単位の入力すべてを表す鍵。出せなければ（キャッシュ不可・python3 の
		# 失敗）空になり、以降はキャッシュが無いのと同じ扱いになる＝必ず解析する。
		key=""
		if [ -n "$CACHE_DIR" ]; then
			# 値は必ず --opt=value の形で渡す。**"-" で始まる値**（-x で渡される
			# --extra-arg=… がまさにそれ）を空白区切りで渡すと、argparse がそれを
			# 次のオプションと読んで落ちる＝Windows のジョブだけ永久にキャッシュが
			# 効かない、という静かな事故になる。
			key="$("$PYTHON" scripts/tidy-cache-key.py -p "$DB" \
				"--tidy-version=$TIDY_VERSION" \
				"--sdk-key=${VW_SDK_CACHE_KEY:-}" \
				"--image-key=$IMAGE_KEY" \
				"--extra=$TIDY_ARGS" "$f" 2>"$LOGDIR/$k.key")" || key=""
			# 形（64 桁の 16 進）を確かめてから使う。python が何かの拍子に別のものを
			# 標準出力へ出しても、$CACHE_DIR/$key が妙なパスにならないようにする。
			case "$key" in
				*[!0-9a-f]*) key="" ;;
			esac
			if [ "${#key}" -ne 64 ]; then
				key=""
			fi
		fi

		if [ -n "$key" ] && [ -f "$CACHE_DIR/$key" ]; then
			# 前に同じ入力で解析して、診断が 1 つも出なかった翻訳単位。触って寿命を
			# 延ばす（下の掃除が「しばらく引かれていない控え」を落とすため）。
			: >"$LOGDIR/$k.log"
			echo 0 >"$LOGDIR/$k.rc"
			echo cached >"$LOGDIR/$k.sec"
			touch "$CACHE_DIR/$key" 2>/dev/null
		else
			start="$(date +%s)"
			"${CMD[@]}" "$f" >"$LOGDIR/$k.log" 2>&1
			rc="$?"
			echo "$rc" >"$LOGDIR/$k.rc"
			echo "$(($(date +%s) - start))" >"$LOGDIR/$k.sec"
			# **きれいに通ったときだけ控える。判定は終了コードで行う。**
			#
			# ここを「出力が空なら」にしてはいけない——clang-tidy は**成功時にも**
			# 「72231 warnings generated.」「Suppressed 72277 warnings (…)」を必ず出す
			# ので、その条件は永久に成り立たず、**1 件も控えられないまま緑になる**
			# （実機でそうなった。復元は効いているのに `0 reused … (0 restored)`）。
			# 終了コードで足りるのは、上の CMD が常に --warnings-as-errors='*' を付けて
			# いるからである——診断が 1 つでも出れば非ゼロになる。つまり rc==0 は
			# 「言うべきことは何も無かった」と同じ意味で、残っている出力は抑制の数え上げ
			# だけ（次の実行で消えても失うものが無い）。
			if [ "$rc" -eq 0 ] && [ -n "$key" ]; then
				printf '' 2>/dev/null >"$CACHE_DIR/$key" || true
			fi
		fi
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
cached=0
analysed=0
k=0
while [ "$k" -lt "${#FILES[@]}" ]; do
	rc="$(cat "$LOGDIR/$k.rc" 2>/dev/null || echo 1)"
	sec="$(cat "$LOGDIR/$k.sec" 2>/dev/null || echo '?')"
	if [ "$sec" = cached ]; then
		echo "----- ${FILES[$k]} (cached) -----"
		cached=$((cached + 1))
	else
		echo "----- ${FILES[$k]} (${sec}s, exit=$rc) -----"
		analysed=$((analysed + 1))
	fi
	# 鍵を出せなかった理由は黙って飲み込まない——**永久にキャッシュが効かない状態**は
	# 「速いはずが遅いまま」として現れるだけで、それ自体はジョブを落とさないからである。
	if [ -s "$LOGDIR/$k.key" ]; then
		sed 's/^/  (cache) /' "$LOGDIR/$k.key"
	fi
	if [ -s "$LOGDIR/$k.log" ]; then
		cat "$LOGDIR/$k.log"
	fi
	if [ "$rc" -ne 0 ]; then
		status=1
		failed="$failed ${FILES[$k]}"
	fi
	k=$((k + 1))
done

if [ -n "$CACHE_DIR" ]; then
	echo "clang-tidy result cache: $cached reused / $analysed analysed" \
		"($CACHE_RESTORED restored)"
	# 控えが届いているのに 1 つも引けなかった。規則（.clang-tidy）・SDK・共有ヘッダを
	# 変えた実行なら当然だが、**そうでなければ鍵が実行機ごとに変わっている**——つまり
	# この仕組みは黙って何もしていない。緑のまま気付けない壊れ方なので、必ず言う。
	if [ "$CACHE_RESTORED" -gt 0 ] && [ "$cached" -eq 0 ]; then
		echo "::warning::clang-tidy の結果キャッシュが $CACHE_RESTORED 件復元されたのに 1 件も再利用されませんでした。規則（.clang-tidy）・SDK・共有ヘッダ・ランナーイメージ・clang-tidy の版や引数を変えていないなら、鍵に実行機ごとに変わるものが混ざっています（scripts/tidy-cache-key.py）。"
	fi
	# 古い控えを落とす。鍵は入力が変わるたびに変わるので、放っておくと溜まる一方に
	# なる（毎コミットぶんが残る）。生きている鍵は引くたびに touch しているので、
	# しばらく触られていないものはもう誰も引かない。
	find "$CACHE_DIR" -type f -mtime +7 -delete 2>/dev/null || true
fi

if [ "$status" -ne 0 ]; then
	echo "::error::clang-tidy failed on:$failed"
fi
exit "$status"
