# shellcheck shell=bash
#
# ci-common.sh — 「CI の完了を待って **必ず** exit する」ための共通土台。
# 実行するものではなく `source` して使う（`ci-debug.sh` / `ci-wait.sh` が利用者）。
#
# なぜこれがあるか
# ----------------
# リモートセッション（クラウド上のコンテナ）から CI の完了を知る手段は
# **「完了したら exit するプロセスをバックグラウンドで走らせる」** ことだけである。
#
#   * PR 購読で配信されるのは CI の**失敗**とコメントだけで、**成功は通知されない**。
#   * `sleep` で時間を決め打ちして見に行くのは、実行時間の予測が要るうえ無駄な待機が出る。
#   * Claude Code のバックグラウンド実行は**プロセスの終了**をハーネスが検知して通知する。
#     つまり exit そのものが通知であり、exit しない待機は「通知されない待機」に等しい。
#
# したがってこの土台の最大の要件は **どんな異常でも必ず有限時間で exit すること**。
# 待機がぶら下がると「CI は終わっているのにセッションが気付かない」という最悪の
# 壊れ方をする（実際に 2 度起きた: #35 と、その後の手書き待機ループ）。歯止めは三重。
#
#   1. **すべての HTTP 呼び出しに時間上限**（--connect-timeout / --max-time）。
#      エージェントプロキシ経由の接続が無応答のまま固まっても curl 側で必ず戻る。
#      これが無いと、ポーリングループの中で無限にブロックし、下記 2 の締切判定に
#      到達できない（＝永久に exit しない）。
#   2. **締切（TIMEOUT）を毎周回で判定**し、超えたら結果不明として打ち切る。
#   3. **ウォッチドッグ**が締切＋余裕を過ぎてもプロセスが生きていたら自分を殺す。
#      将来 1・2 の外側に新しいブロック箇所が増えても、ぶら下がりだけは起こらない。
#
# 併せて、API が失敗し続けたときは黙って回り続けずに理由を出して打ち切る（認証エラー
# のように回復しないものは即座に）。「無言で 45 分回り続ける」のは、ぶら下がりと
# 見分けが付かないので同じ害がある。
#
# **待機ループを手書きしないこと。** 手書きの `while : ; do ... sleep 30; done` は
# 上記の歯止めをどれも持たないので、いつか必ず同じ壊れ方をする。新しい「何かの完了を
# 待つ」道具が要るときは、この土台の `poll_until` の上に probe を 1 つ書く。
#
# 利用者が source 前に設定するもの:
#   CI_TOOL   メッセージの接頭辞（例: ci-debug / ci-wait）。既定は "ci"
# 利用者が poll_until を呼ぶ前に設定するもの:
#   POLL      ポーリング間隔・秒（require_positive_int で検証すること）
#   TIMEOUT   待機の上限・秒（同上）
#
# 共通の環境変数（CI_DEBUG_* は従来名。互換のため引き続き効く）:
#   GITHUB_TOKEN / GH_TOKEN   API 呼び出しに使う
#   VW_REPO                   owner/repo
#   CI_HTTP_TIMEOUT           1 回の API 呼び出しの上限・秒（既定 45）
#   CI_CONNECT_TIMEOUT        接続確立の上限・秒（既定 15）
#   CI_MAX_API_ERRORS         API 連続失敗をどこまで許すか（既定 10）
#   CI_HEARTBEAT              状態が変わらないときの生存出力の間隔・秒（既定 300）
#

CI_TOOL="${CI_TOOL:-ci}"

VW_REPO="${VW_REPO:-min-nano/vectorworks-plugin-import-ifc-homeskz}"
# shellcheck disable=SC2034 # source した側（ci-wait.sh / ci-debug.sh）が使う。
VW_API="https://api.github.com/repos/${VW_REPO}"
TOKEN="${GH_TOKEN:-${GITHUB_TOKEN:-}}"

# 1 回の HTTP 呼び出しの上限。**これが待機のぶら下がりを防ぐ一番の要**（ヘッダの
# 「必ず有限時間で exit すること」参照）。接続の確立とデータ転送で別々に上限を持たせる。
HTTP_TIMEOUT="${CI_HTTP_TIMEOUT:-${CI_DEBUG_HTTP_TIMEOUT:-45}}"
HTTP_CONNECT_TIMEOUT="${CI_CONNECT_TIMEOUT:-${CI_DEBUG_CONNECT_TIMEOUT:-15}}"
# API が連続して失敗したときに諦める回数。一過性のエラーで待機を殺したくはないが、
# 無言で回り続けるのはぶら下がりと同じ害なので、どこかで打ち切って理由を出す。
MAX_API_ERRORS="${CI_MAX_API_ERRORS:-${CI_DEBUG_MAX_API_ERRORS:-10}}"
# 進捗が無くても生存を示す間隔・秒。状態が変わらないまま黙り込むと「固まっている」
# のか「CI が長いだけ」なのかを呼び出し側が区別できない。
HEARTBEAT="${CI_HEARTBEAT:-${CI_DEBUG_HEARTBEAT:-300}}"

die() {
	echo "$CI_TOOL: error: $1" >&2
	exit 2
}

command -v jq >/dev/null 2>&1 || die "jq が必要です"
command -v curl >/dev/null 2>&1 || die "curl が必要です"

# require_positive_int <値> <表示名>: 締切とポーリング間隔は必ず数値であること。
# 空文字や誤字がそのまま通ると比較が常に偽になり、締切が効かない＝ぶら下がる。
require_positive_int() {
	case "$1" in
		'' | *[!0-9]*) die "$2 は秒数（整数）で指定してください: '$1'" ;;
	esac
	[ "$1" -ge 1 ] || die "$2 は 1 以上にしてください: '$1'"
	return 0
}

# ---------------------------------------------------------------------------
# 一時ファイル
# ---------------------------------------------------------------------------
#
# 作業用ファイルは 1 つのディレクトリにまとめ、EXIT で丸ごと消す。個別に rm する
# だけだと、途中で殺された（ウォッチドッグ発火・Ctrl-C）ときに /tmp に残る。
#
# ディレクトリは**ここで先に**作る。workfile() は `f="$(workfile)"` の形で——つまり
# コマンド置換のサブシェルで——呼ばれるので、関数の中で遅延生成すると変数の代入が
# 親に返らず、呼ぶたびに別のディレクトリができて後始末できなくなる。
WORKDIR="$(mktemp -d)"

workfile() {
	mktemp "$WORKDIR/f.XXXXXX"
}

# ---------------------------------------------------------------------------
# ウォッチドッグ（最後の歯止め）
# ---------------------------------------------------------------------------
#
# 待機経路のブロックは curl の時間上限と締切判定で塞いであるが、それでも「絶対に
# exit する」ことをスクリプト構造に依存させたくない。締切＋余裕を過ぎても生きて
# いたら、対象（このスクリプト）とその子を殺し、呼び出し側にプロセス終了を届ける。
#
# **発火時のシグナル順は「親 → 子」でなければならない**（実測。逆にすると効かない）。
# bash はフォアグラウンドの子を待っている間、受け取ったシグナルの trap を保留し、その
# 子が終わってから実行する。つまり
#
#   * 親へ TERM → 子を殺す … 親に保留された trap が、子の終了直後に走る（正しい）。
#   * 子を殺す → 親へ TERM … 子の終了と TERM が競合し、**TERM が握り潰されて**
#     親はそのまま走り続ける（＝ウォッチドッグが効かない）。
#
# 子（curl / sleep）を殺すのは、親を待ち状態から解放して保留中の trap を即座に走らせる
# ため。これが無いと、親は今の子が終わるまで trap を実行できない。
WATCHDOG_PID=""
WATCHDOG_FLAG=""

# kill_descendants <signal> <pid> [除外する pid]: pid の子孫へ再帰的にシグナルを送る。
#
# 子孫まで落とすのは 2 つの理由から。(1) 親をフォアグラウンドの待ちから解放しないと、
# 保留されている trap が実行されない。(2) 親の stdout/stderr を引き継いだプロセスが
# 残ると、親が死んでもパイプが閉じず、呼び出し側の完了検知が遅れる。
#
# 段数を決め打ちにせず再帰するのは、実際の待機が
# 「親 → $(wait_run) → $(api_json) → $(api) → curl」と何段にもなるため。
#
# **除外 pid が必須の場面がある**: ウォッチドッグ自身も対象プロセスの子なので、
# 除外しないと列挙の途中で自分を殺してしまい、本命の子（待機中のサブシェル）へ
# シグナルが届かないまま終わる。
kill_descendants() {
	local sig="$1" pid="$2" skip="${3:-}" child
	while read -r child; do
		[ -n "$child" ] || continue
		[ -n "$skip" ] && [ "$child" = "$skip" ] && continue
		kill_descendants "$sig" "$child" "$skip"
		kill "-$sig" "$child" 2>/dev/null
	done < <(pgrep -P "$pid" 2>/dev/null)
	return 0
}

start_watchdog() {
	local limit="$1" target=$$ flag
	flag="$(workfile)"
	WATCHDOG_FLAG="$flag"
	(
		# **親の stdout/stderr を持たない。** 引き継いだまま生き残ると、親が終わっても
		# パイプが閉じず、呼び出し側の完了検知（EOF 待ち）を遅らせうる。発火したことは
		# フラグファイルで伝え、文言は親の TERM ハンドラに出させる。
		exec >/dev/null 2>&1
		sleep "$limit"
		printf 'fired\n' >"$flag"
		# 順序厳守（上の説明を参照）: まず親、次に親を待ち状態から解放するための子孫。
		kill -TERM "$target"
		sleep 1
		kill_descendants TERM "$target" "$BASHPID"
		# ここまでで終わらなければ問答無用で落とす。
		sleep 10
		kill -KILL "$target"
		kill_descendants KILL "$target" "$BASHPID"
	) &
	WATCHDOG_PID=$!
	# ジョブテーブルから外す。外さないと、正常終了時に kill したときシェルが
	# "Terminated" と stderr に書き、結果を読む側に無関係なノイズが混ざる。
	disown "$WATCHDOG_PID" 2>/dev/null || true
}

# stop_watchdog: 正常終了時に確実に片付ける。
#
# **順序が重要**: 先に子（sleep）を落とすと、sleep が終わったものとして本体が次の行へ
# 進み、その場で親を殺してしまう。まず本体を殺し、取り残された sleep を後から落とす。
stop_watchdog() {
	local pid="$WATCHDOG_PID" kids
	[ -n "$pid" ] || return 0
	WATCHDOG_PID=""
	# 子（sleep）の PID は**本体を殺す前に**控える。本体が死ぬと sleep は init へ
	# 里子に出され、親子関係から辿れなくなって上限ぶん居座る。
	kids="$(pgrep -P "$pid" 2>/dev/null | tr '\n' ' ')"
	kill "$pid" 2>/dev/null
	# shellcheck disable=SC2086 # kids は空白区切りの PID 列。分割させたい。
	[ -n "$kids" ] && kill $kids 2>/dev/null
	return 0
}

# ci_cleanup: EXIT で必ず動く後始末。ウォッチドッグを止め、作業ファイルを消す。
ci_cleanup() {
	stop_watchdog
	rm -rf "$WORKDIR"
	return 0
}

ci_on_term() {
	local fired=""
	[ -n "$WATCHDOG_FLAG" ] && [ -s "$WATCHDOG_FLAG" ] && fired=1
	stop_watchdog
	if [ -n "$fired" ]; then
		echo "$CI_TOOL: watchdog: 上限を超えても終わらないので打ち切りました（CI はまだ動いているかもしれません）" >&2
	else
		echo "$CI_TOOL: 中断されました（CI はまだ動いているかもしれません）" >&2
	fi
	exit 1
}

trap ci_cleanup EXIT
trap ci_on_term TERM INT

# ---------------------------------------------------------------------------
# GitHub API
# ---------------------------------------------------------------------------

# api <curl args...>: 認証済みの GitHub API 呼び出し。
#
# **時間上限を必ず付ける。** 素の curl は接続が無応答になると永久に待つ。ポーリング
# ループの中でそれが起きると、締切判定にすら到達できないままプロセスがぶら下がり、
# 「CI は完了しているのに待機コマンドが exit しない」という壊れ方をする（実際に
# 起きた不具合）。--retry は一過性の失敗を curl 側で吸収するためで、上限は 1 回の
# 試行ごとに適用される。
api() {
	curl -sS \
		--connect-timeout "$HTTP_CONNECT_TIMEOUT" \
		--max-time "$HTTP_TIMEOUT" \
		--retry 2 --retry-delay 2 --retry-connrefused \
		-H "Authorization: Bearer $TOKEN" \
		-H "Accept: application/vnd.github+json" \
		-H "X-GitHub-Api-Version: 2022-11-28" \
		"$@"
}

# api_json <url> <outfile>: GET してボディを outfile へ、HTTP ステータスを stdout へ。
# curl 自体が失敗したときは 000 を返す（呼び出し側は「一過性」として扱える）。
api_json() {
	local url="$1" out="$2" code
	code="$(api -o "$out" -w '%{http_code}' "$url" 2>/dev/null)"
	printf '%s\n' "${code:-000}"
}

# fatal_http <code>: そのステータスは待ち続けても回復しないか。認証・権限・不在は
# 回り続けるだけ無駄なので即座に諦める（45 分黙って回った末に「timeout」と言われる
# のが一番たちが悪い）。
fatal_http() {
	case "$1" in
		401 | 403 | 404 | 410) return 0 ;;
		*) return 1 ;;
	esac
}

# api_message <body-file>: エラーボディから message を取り出す（無ければ空）。
api_message() {
	jq -r '.message // empty' "$1" 2>/dev/null
}

# ---------------------------------------------------------------------------
# ポーリング（唯一の待機ループ）
# ---------------------------------------------------------------------------
#
# poll_until <probe 関数名>: probe を POLL 秒ごとに呼び、完了するまで待つ。
# 締切・生存出力・API 連続失敗の打ち切りは**すべてここが持つ**ので、probe は
# 「今どうなっているか」を 1 回調べるだけでよい（待機の作法を各所に散らさない）。
#
# probe の戻り値:
#   0  完了（結果は probe が自分の変数へ書く）
#   1  未完了。次の周回へ
#   2  回復しない異常。即打ち切り（理由は probe が出す）
#   3  一過性の失敗。MAX_API_ERRORS 回続いたら打ち切り
#
# probe は現在の状態を POLL_STATUS に入れる。**変化したときだけ** 1 行出力し、
# 変化が無いまま HEARTBEAT 秒たったら生存行を出す（黙り込むとぶら下がりと区別が
# 付かない）。経過秒は POLL_ELAPSED で参照できる。
#
# 戻り値: 0=完了 / 1=締切超過 / 2=打ち切り。**どれも有限時間で必ず返る。**
POLL_STATUS=""
POLL_ELAPSED=0

poll_until() {
	local probe="$1" started now beat errors=0 rc last=""
	started="$(date +%s)"
	beat="$started"
	while true; do
		POLL_STATUS=""
		"$probe"
		rc="$?"
		now="$(date +%s)"
		POLL_ELAPSED="$((now - started))"

		case "$rc" in
			0) return 0 ;;
			2) return 2 ;;
			3)
				errors="$((errors + 1))"
				if [ "$errors" -ge "$MAX_API_ERRORS" ]; then
					echo "$CI_TOOL: API が ${MAX_API_ERRORS} 回続けて失敗したので待機を打ち切ります" >&2
					return 2
				fi
				;;
			*) errors=0 ;;
		esac

		if [ -n "$POLL_STATUS" ] && [ "$POLL_STATUS" != "$last" ]; then
			echo "$POLL_STATUS (${POLL_ELAPSED}s)" >&2
			last="$POLL_STATUS"
			beat="$now"
		fi
		if [ "$((now - beat))" -ge "$HEARTBEAT" ]; then
			echo "$CI_TOOL: 待機中… ${last:-status=unknown} (${POLL_ELAPSED}s / 上限 ${TIMEOUT}s)" >&2
			beat="$now"
		fi
		if [ "$POLL_ELAPSED" -ge "$TIMEOUT" ]; then
			echo "$CI_TOOL: timeout: ${TIMEOUT}s 待っても完了しませんでした（CI はまだ動いているかもしれません）" >&2
			return 1
		fi
		sleep "$POLL"
	done
}
