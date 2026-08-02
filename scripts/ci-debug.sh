#!/usr/bin/env bash
#
# ci-debug.sh — `.github/workflows/ci-debug.yml`（CI debug）を起動し、**完了まで待って**
# 結果ペイロードを取り出すクライアント。
#
# なぜこれがあるか
# ----------------
# リモートセッションのコンテナには Vectorworks SDK が無いため、SDK 依存のビルド
# エラーや「この API は SDK にあるか」という調査は CI 上でしか答えが出ない。ところが
#
#   * PR 購読で配信されるのは CI の**失敗**とコメントだけで、**成功は通知されない**。
#   * よってタイマーで見に行くことになるが、実行時間の予測が要るうえ無駄な待機が出る。
#
# 一方このコンテナからは GitHub REST API に直接到達できる（GITHUB_TOKEN が入っている）。
# そこで「完了したら exit するプロセス」をバックグラウンドで走らせれば、待機時間ゼロ・
# タイマー不要で完了を知れる。Claude Code なら:
#
#   Bash(run_in_background: true) で
#     scripts/ci-debug.sh run --mode sdk-grep --args 'GetLayerByName'
#   を投げて別作業を続け、終了通知が来たら出力ファイルを Read するだけ。
#
# ディスパッチ → run の特定 → 完了待ち → ジョブログからペイロード抽出、までを 1 本で
# やるので、GitHub MCP を 3 往復する必要もない。
#
# 使い方
# ------
#   scripts/ci-debug.sh run --mode <mode> [options]      起動して完了まで待つ
#   scripts/ci-debug.sh wait --label <label>             既存の実行に後から合流する
#   scripts/ci-debug.sh wait --run-id <id>
#   scripts/ci-debug.sh logs --run-id <id>               完了済み実行のペイロードだけ取る
#
#   モード（詳細は scripts/ci-debug-job.sh）:
#     sdk-grep     SDK ヘッダを拡張正規表現で検索（--args にパターン）
#     sdk-ls       ヘッダの全文表示 / パス部分一致の一覧（--args にパス）
#     build        configure してビルド（--args に単一ターゲット、省略可）
#     compile-one  1 翻訳単位だけコンパイル（--args にソースのパス。Windows 不可）
#     shell        --script の bash をそのまま実行（逃げ道）
#
#   オプション:
#     --mode M         必須。上記のいずれか
#     --platform P     mac（既定） | windows | linux（linux は SDK 非依存コード専用）
#     --args A         モードごとの引数
#     --script S       mode=shell のとき実行する bash
#     --ref R          調査対象のブランチ（既定: いま checkout しているブランチ）
#     --label L        実行の識別子（既定: 自動生成。run 名に埋め込まれる）
#     --notify-pr N    完了時に結果を PR へコメントする（待機プロセスを失ったときの保険）
#     --no-wait        ディスパッチだけして待たない
#     --poll S         ポーリング間隔・秒（既定 15）
#     --timeout S      待機の上限・秒（既定 2700 = 45 分）
#
# **必ず終わること**（ここが設計の要）
# ----------------------------------
# このスクリプトの唯一の存在意義は「CI が終わった瞬間に exit して呼び出し側へ知らせる」
# ことなので、**どんな異常でも必ず有限時間で exit する**必要がある。exit しないまま
# ぶら下がると、呼び出し側（Claude Code のバックグラウンド待機）は完了通知を受け取れず、
# 「CI は終わっているのにセッションが気付かない」という最悪の壊れ方をする。そのため
# 待機経路には三重の歯止めを入れてある。
#
#   1. **すべての HTTP 呼び出しに時間上限**（--connect-timeout / --max-time）。
#      エージェントプロキシ経由の接続が無応答のまま固まっても curl 側で必ず戻る。
#      これが無いと、ポーリングループの中で無限にブロックし、下記 2 の締切判定に
#      到達できない（＝永久に exit しない）。
#   2. **締切（--timeout）を毎周回で判定**し、超えたら結果不明として exit 1。
#   3. **ウォッチドッグ**が締切＋余裕を過ぎてもプロセスが生きていたら自分を殺す。
#      将来 1・2 の外側に新しいブロック箇所が増えても、ぶら下がりだけは起こらない。
#
# 併せて、API が失敗し続けたときは黙って回り続けずに理由を出して打ち切る（認証エラー
# のように回復しないものは即座に）。「無言で 45 分回り続ける」のは、ぶら下がりと
# 見分けが付かないので同じ害がある。
#
# 環境変数:
#   GITHUB_TOKEN / GH_TOKEN   必須（ディスパッチには write 権限が要る）
#   VW_REPO                   owner/repo（既定は下記）
#   CI_DEBUG_POLL             ポーリング間隔・秒（既定 15。--poll と同じ）
#   CI_DEBUG_TIMEOUT          待機の上限・秒（既定 2700 = 45 分。ジョブ側の
#                             timeout-minutes と同じ。--timeout と同じ）
#   CI_DEBUG_HTTP_TIMEOUT     1 回の API 呼び出しの上限・秒（既定 45）
#   CI_DEBUG_CONNECT_TIMEOUT  接続確立の上限・秒（既定 15）
#   CI_DEBUG_MAX_API_ERRORS   API 連続失敗をどこまで許すか（既定 10）
#   CI_DEBUG_HEARTBEAT        状態が変わらないときの生存出力の間隔・秒（既定 300）
#
# 終了ステータス: run の conclusion が success なら 0、それ以外（失敗・締切超過・
# API 断念）は 1。使い方の誤りや起動失敗は 2。
#
set -uo pipefail

VW_REPO="${VW_REPO:-min-nano/vectorworks-plugin-import-ifc-homeskz}"
VW_API="https://api.github.com/repos/${VW_REPO}"
WORKFLOW_FILE="ci-debug.yml"
POLL="${CI_DEBUG_POLL:-15}"
TIMEOUT="${CI_DEBUG_TIMEOUT:-2700}"
TOKEN="${GH_TOKEN:-${GITHUB_TOKEN:-}}"

# 1 回の HTTP 呼び出しの上限。**これが待機のぶら下がりを防ぐ一番の要**（ヘッダの
# 「必ず終わること」参照）。接続の確立とデータ転送で別々に上限を持たせる。
HTTP_TIMEOUT="${CI_DEBUG_HTTP_TIMEOUT:-45}"
HTTP_CONNECT_TIMEOUT="${CI_DEBUG_CONNECT_TIMEOUT:-15}"
# API が連続して失敗したときに諦める回数。一過性のエラーで待機を殺したくはないが、
# 無言で回り続けるのはぶら下がりと同じ害なので、どこかで打ち切って理由を出す。
MAX_API_ERRORS="${CI_DEBUG_MAX_API_ERRORS:-10}"
# 進捗が無くても生存を示す間隔・秒。状態が変わらないまま黙り込むと「固まっている」
# のか「CI が長いだけ」なのかを呼び出し側が区別できない。
HEARTBEAT="${CI_DEBUG_HEARTBEAT:-300}"

die() {
	echo "ci-debug: error: $1" >&2
	exit 2
}

command -v jq >/dev/null 2>&1 || die "jq が必要です"

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

# cleanup: EXIT で必ず動く後始末。ウォッチドッグを止め、作業ファイルを消す。
cleanup() {
	stop_watchdog
	rm -rf "$WORKDIR"
	return 0
}

on_term() {
	local fired=""
	[ -n "$WATCHDOG_FLAG" ] && [ -s "$WATCHDOG_FLAG" ] && fired=1
	stop_watchdog
	if [ -n "$fired" ]; then
		echo "ci-debug: watchdog: 上限を超えても終わらないので打ち切りました（run はまだ動いているかもしれません）" >&2
	else
		echo "ci-debug: 中断されました（run はまだ動いているかもしれません）" >&2
	fi
	exit 1
}

trap cleanup EXIT
trap on_term TERM INT

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

# ---------------------------------------------------------------------------
# 引数解析
# ---------------------------------------------------------------------------

# usage: ヘッダのコメントブロックをそのままヘルプとして出す（唯一の説明を二重に
# 持たないため）。行番号で切ると header を書き足すたびにずれるので、shebang の次から
# 最初の非コメント行の手前までを出す。
usage() {
	awk 'NR > 1 { if ($0 !~ /^#/) exit; print }' "$0"
}

CMD="${1:-}"
case "$CMD" in
	-h | --help | help) usage ; exit 0 ;;
esac
[ -n "$CMD" ] || die "サブコマンドを指定してください（run / wait / logs）"
shift

MODE=""
PLATFORM="mac"
ARGS=""
SCRIPT=""
REF=""
LABEL=""
NOTIFY_PR=""
RUN_ID=""
WAIT=1

while [ "$#" -gt 0 ]; do
	case "$1" in
		--mode) MODE="${2:-}" ; shift 2 ;;
		--platform) PLATFORM="${2:-}" ; shift 2 ;;
		--args) ARGS="${2:-}" ; shift 2 ;;
		--script) SCRIPT="${2:-}" ; shift 2 ;;
		--ref) REF="${2:-}" ; shift 2 ;;
		--label) LABEL="${2:-}" ; shift 2 ;;
		--notify-pr) NOTIFY_PR="${2:-}" ; shift 2 ;;
		--run-id) RUN_ID="${2:-}" ; shift 2 ;;
		--poll) POLL="${2:-}" ; shift 2 ;;
		--timeout) TIMEOUT="${2:-}" ; shift 2 ;;
		--no-wait) WAIT=0 ; shift ;;
		-h | --help) usage ; exit 0 ;;
		*) die "未知のオプション: $1" ;;
	esac
done

# ---------------------------------------------------------------------------
# run の特定・待機・ログ取得
# ---------------------------------------------------------------------------

# fatal_http <code>: そのステータスは待ち続けても回復しないか。認証・権限・不在は
# 回り続けるだけ無駄なので即座に諦める（45 分黙って回った末に「timeout」と言われる
# のが一番たちが悪い）。
fatal_http() {
	case "$1" in
		401 | 403 | 404 | 410) return 0 ;;
		*) return 1 ;;
	esac
}

# resolve_run <label>: label を run 名に埋め込んであるので、それで自分の run を探す。
# dispatch API は run の id を返さないため、この突き合わせが唯一の手掛かり。GitHub が
# run を作るまで数秒かかるので少し粘るが、粘る時間は有限（120 秒）で、諦めるときは
# 必ず理由を stderr に出す。
resolve_run() {
	local label="$1" tries=0 id code body
	body="$(workfile)"
	while [ "$tries" -lt 40 ]; do
		tries=$((tries + 1))
		code="$(api_json "$VW_API/actions/workflows/$WORKFLOW_FILE/runs?event=workflow_dispatch&per_page=50" "$body")"
		if [ "$code" = "200" ]; then
			id="$(jq -r --arg l "[$label]" \
				'first(.workflow_runs[] | select(.display_title | contains($l)) | .id) // empty' "$body" 2>/dev/null)"
			if [ -n "$id" ]; then
				rm -f "$body"
				printf '%s\n' "$id"
				return 0
			fi
		elif fatal_http "$code"; then
			echo "ci-debug: run 一覧を取得できません（HTTP $code）: $(jq -r '.message // empty' "$body" 2>/dev/null)" >&2
			rm -f "$body"
			return 1
		else
			echo "ci-debug: run 一覧の取得に失敗（HTTP $code）— 再試行します" >&2
		fi
		sleep 3
	done
	echo "ci-debug: label=[$label] の run が 120 秒たっても現れませんでした" >&2
	rm -f "$body"
	return 1
}

# wait_run <run-id>: completed になるまでポーリングし、conclusion を echo する。
#
# 終了経路は 3 つだけで、**どれも有限時間で必ず返る**:
#   * completed を観測 → conclusion（成功経路）
#   * 締切（TIMEOUT）超過 → timed-out-waiting
#   * API の連続失敗が上限に達した / 回復しないエラー → api-error
#
# 一過性の API エラーで待機を殺さないよう失敗しても次の周回へ進むが、黙って進まず
# 必ず 1 行出す。「何も出力せず生きているプロセス」はぶら下がりと区別が付かない。
wait_run() {
	local id="$1" started now elapsed code status conclusion last="" errors=0 beat body
	started="$(date +%s)"
	beat="$started"
	body="$(workfile)"
	while true; do
		code="$(api_json "$VW_API/actions/runs/$id" "$body")"
		now="$(date +%s)"
		elapsed="$((now - started))"

		if [ "$code" = "200" ]; then
			errors=0
			status="$(jq -r '.status // empty' "$body" 2>/dev/null)"
			conclusion="$(jq -r '.conclusion // empty' "$body" 2>/dev/null)"
			if [ -n "$status" ] && [ "$status" != "$last" ]; then
				echo "status=$status (${elapsed}s)" >&2
				last="$status"
				beat="$now"
			fi
			if [ "$status" = "completed" ]; then
				rm -f "$body"
				printf '%s\n' "${conclusion:-unknown}"
				return 0
			fi
		else
			errors=$((errors + 1))
			echo "ci-debug: run の状態取得に失敗（HTTP $code, ${errors}/${MAX_API_ERRORS} 連続, ${elapsed}s）" >&2
			if fatal_http "$code"; then
				echo "ci-debug: 回復しないエラーなので待機を打ち切ります: $(jq -r '.message // empty' "$body" 2>/dev/null)" >&2
				rm -f "$body"
				printf '%s\n' "api-error"
				return 1
			fi
			if [ "$errors" -ge "$MAX_API_ERRORS" ]; then
				echo "ci-debug: API が ${MAX_API_ERRORS} 回続けて失敗したので待機を打ち切ります" >&2
				rm -f "$body"
				printf '%s\n' "api-error"
				return 1
			fi
		fi

		if [ "$((now - beat))" -ge "$HEARTBEAT" ]; then
			echo "ci-debug: 待機中… status=${last:-unknown} (${elapsed}s / 上限 ${TIMEOUT}s)" >&2
			beat="$now"
		fi

		if [ "$elapsed" -ge "$TIMEOUT" ]; then
			echo "ci-debug: timeout: ${TIMEOUT}s 待っても完了しませんでした（run はまだ動いているかもしれません）" >&2
			rm -f "$body"
			printf '%s\n' "timed-out-waiting"
			return 1
		fi
		sleep "$POLL"
	done
}

# fetch_payload <run-id>: ペイロードを取り出す。経路は 2 つあり、順に試す。
#
#   1. チェックラン注釈（api.github.com のみ）… ci-debug-job.sh が ::notice:: で
#      ペイロードを注釈としても出しているので、ここから読めればログを触らずに済む。
#      ログのノイズが混ざらず、下記 2 が使えない環境でも動く。
#   2. ジョブログ … 従来の経路。ただしログ API は署名付きの Azure Blob Storage へ
#      302 で飛ぶため、そのホストを egress ポリシーで拒否している環境（Claude Code の
#      リモートセッション等）では取得できない。その場合は理由と代替手段を示す。
fetch_payload() {
	local id="$1" jobs job check url tmp ann tries
	jobs="$(workfile)"
	if [ "$(api_json "$VW_API/actions/runs/$id/jobs" "$jobs")" != "200" ]; then
		echo "ci-debug: ジョブ一覧を取得できませんでした（run=$id）" >&2
		rm -f "$jobs"
		return 1
	fi
	job="$(jq -r '.jobs[0].id // empty' "$jobs" 2>/dev/null)"
	[ -n "$job" ] || {
		echo "ci-debug: ジョブが見つかりませんでした（run=$id）" >&2
		rm -f "$jobs"
		return 1
	}
	check="$(jq -r '.jobs[0].check_run_url // empty' "$jobs" 2>/dev/null)"
	rm -f "$jobs"

	# 注釈は run が completed になった直後だとまだ見えないことがある（チェックランの
	# 更新が数秒遅れる）。ここで諦めると下記のログ経路——このコンテナからは到達できない
	# ——へ落ちてしまうので、短く粘る。
	if [ -n "$check" ]; then
		tries=0
		while [ "$tries" -lt 4 ]; do
			tries=$((tries + 1))
			ann="$(api "$check/annotations" 2>/dev/null |
				jq -r 'map(select(.title == "ci-debug payload")) | .[0].message // empty' 2>/dev/null)"
			if [ -n "$ann" ]; then
				printf '%s\n' "$ann"
				return 0
			fi
			[ "$tries" -lt 4 ] && sleep 5
		done
	fi

	tmp="$(workfile)"
	url="$(api -o /dev/null -w '%{redirect_url}' "$VW_API/actions/jobs/$job/logs")"
	if [ -n "$url" ]; then
		# ログ本体は GitHub 外（署名付きストレージ）なので api() は使えない。時間上限
		# だけは同じように必ず付ける — ここで固まると「完了したのに exit しない」に戻る。
		if ! curl -sS --connect-timeout "$HTTP_CONNECT_TIMEOUT" --max-time "$HTTP_TIMEOUT" \
			"$url" >"$tmp" 2>"$tmp.err"; then
			echo "(ペイロード注釈が無く、ジョブログも取得できませんでした)"
			echo "  理由: $(tr -d '\n' <"$tmp.err")"
			echo "  ログ API は署名付きストレージへリダイレクトします。そのホストが"
			echo "  egress ポリシーで拒否されている環境では、GitHub MCP の get_job_logs"
			echo "  （job_id=$job, return_content=true）で取得してください。"
			rm -f "$tmp" "$tmp.err"
			return 1
		fi
	else
		api "$VW_API/actions/jobs/$job/logs" >"$tmp"
	fi
	rm -f "$tmp.err"

	# GitHub のログは各行に ISO タイムスタンプが前置される。読みづらいので落とす。
	sed -E 's/^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z //' "$tmp" >"$tmp.clean"

	if grep -q '^===== BEGIN PAYLOAD' "$tmp.clean"; then
		awk '/^===== BEGIN PAYLOAD/,/^===== END PAYLOAD/' "$tmp.clean"
	else
		# 調査コマンドに到達する前に落ちた場合（SDK ダウンロード失敗など）。
		# 黙って空を返すと「結果なし」と誤読されるので、理由が分かる末尾を出す。
		echo "(ペイロードマーカーがありません — 調査コマンドに到達する前に失敗した可能性があります。ログ末尾:)"
		echo
		tail -n 100 "$tmp.clean"
	fi
	rm -f "$tmp" "$tmp.clean"
}

# report <run-id> <conclusion>: 結果の見出しとペイロードを出し、終了コードを決める。
#
# 待機が完了を見届けられなかったとき（締切超過・API 断念）は run がまだ動いている
# はずで、ペイロードを取りにいっても中身が無い。無駄に待たせず、合流コマンドを示す。
# 最後に必ず 1 行「終わった」と出すのは、呼び出し側が出力の末尾だけを見て
# 「exit したのか、まだ動いているのか」を判定できるようにするため。
report() {
	local id="$1" conclusion="$2" rc=0
	echo
	echo "conclusion=$conclusion"
	echo "run_url=https://github.com/$VW_REPO/actions/runs/$id"
	echo
	case "$conclusion" in
		timed-out-waiting | api-error)
			echo "(結果は取得していません — 待機が完了を見届けられませんでした)"
			echo "  合流するには: scripts/ci-debug.sh wait --run-id $id"
			rc=1
			;;
		*)
			fetch_payload "$id"
			[ "$conclusion" = "success" ] || rc=1
			;;
	esac
	echo
	echo "ci-debug: done (conclusion=$conclusion exit=$rc)"
	return "$rc"
}

# ---------------------------------------------------------------------------
# サブコマンド
# ---------------------------------------------------------------------------

# 締切とポーリング間隔は必ず数値であること。空文字や誤字がそのまま通ると比較が常に
# 偽になり、締切が効かない＝ぶら下がる。
case "$POLL" in '' | *[!0-9]*) die "--poll は秒数（整数）で指定してください: '$POLL'" ;; esac
case "$TIMEOUT" in '' | *[!0-9]*) die "--timeout は秒数（整数）で指定してください: '$TIMEOUT'" ;; esac
[ "$POLL" -ge 1 ] || die "--poll は 1 秒以上にしてください"

case "$CMD" in
	run)
		[ -n "$TOKEN" ] || die "GITHUB_TOKEN / GH_TOKEN が未設定です（ディスパッチには write 権限が要ります）"
		[ -n "$MODE" ] || die "--mode が必要です"
		# 締切＋余裕（ペイロード取得ぶん）を過ぎたら自分を殺す。締切判定より外側で
		# 何かが固まっても、プロセスは必ず終わる。
		start_watchdog "$((TIMEOUT + 300))"
		if [ -z "$REF" ]; then
			REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
			if [ -z "$REF" ] || [ "$REF" = "HEAD" ]; then
				die "--ref を指定してください（現在のブランチを判定できません）"
			fi
		fi
		if [ -z "$LABEL" ]; then
			LABEL="dbg-$(date +%s)-$$"
		fi

		body="$(jq -n \
			--arg ref "$REF" \
			--arg mode "$MODE" \
			--arg platform "$PLATFORM" \
			--arg label "$LABEL" \
			--arg args "$ARGS" \
			--arg script "$SCRIPT" \
			--arg notify_pr "$NOTIFY_PR" \
			'{ref: $ref, inputs: {mode: $mode, platform: $platform, label: $label, args: $args, script: $script, notify_pr: $notify_pr}}')"

		code="$(api -o /dev/null -w '%{http_code}' -X POST \
			-d "$body" "$VW_API/actions/workflows/$WORKFLOW_FILE/dispatches")"
		if [ "$code" = "403" ]; then
			# リモートセッションのトークンは読み取り専用（actions: write が無い）。
			# これは設定ミスではなく仕様なので、回避策を具体的に示す。
			die "ディスパッチが権限で拒否されました（HTTP 403）。このトークンには actions: write がありません。GitHub MCP の actions_run_trigger（workflow_id=ci-debug.yml, ref=$REF, inputs.label=$LABEL）で起動し、'scripts/ci-debug.sh wait --label $LABEL' で待ってください"
		fi
		[ "$code" = "204" ] ||
			die "ディスパッチに失敗しました（HTTP $code）。ci-debug.yml が main にマージ済みか、ref='$REF' が push 済みかを確認してください"

		# 待機プロセスが失われても後から合流できるよう、label は必ず先に出す。
		echo "label=$LABEL"
		echo "ref=$REF mode=$MODE platform=$PLATFORM"

		run_id="$(resolve_run "$LABEL")" || die "起動した run を特定できませんでした（label=$LABEL）"
		echo "run_id=$run_id"
		echo "run_url=https://github.com/$VW_REPO/actions/runs/$run_id"

		if [ "$WAIT" -eq 0 ]; then
			echo "(--no-wait: 完了は 'scripts/ci-debug.sh wait --run-id $run_id' で待てます)"
			exit 0
		fi

		conclusion="$(wait_run "$run_id")"
		if report "$run_id" "$conclusion"; then exit 0; fi
		exit 1
		;;

	wait)
		[ -n "$TOKEN" ] || die "GITHUB_TOKEN / GH_TOKEN が未設定です"
		start_watchdog "$((TIMEOUT + 300))"
		if [ -z "$RUN_ID" ]; then
			[ -n "$LABEL" ] || die "--run-id か --label が必要です"
			RUN_ID="$(resolve_run "$LABEL")" || die "label=$LABEL の run が見つかりません"
			echo "run_id=$RUN_ID"
		fi
		conclusion="$(wait_run "$RUN_ID")"
		if report "$RUN_ID" "$conclusion"; then exit 0; fi
		exit 1
		;;

	logs)
		[ -n "$TOKEN" ] || die "GITHUB_TOKEN / GH_TOKEN が未設定です"
		start_watchdog 600
		if [ -z "$RUN_ID" ]; then
			[ -n "$LABEL" ] || die "--run-id か --label が必要です"
			RUN_ID="$(resolve_run "$LABEL")" || die "label=$LABEL の run が見つかりません"
		fi
		fetch_payload "$RUN_ID"
		;;

	*)
		die "未知のサブコマンド: '$CMD'（run / wait / logs）"
		;;
esac
