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
# ことなので、**どんな異常でも必ず有限時間で exit する**必要がある。その歯止め
# （HTTP の時間上限・締切判定・ウォッチドッグ）と待機ループ本体は `ci-common.sh` に
# あり、PR の CI を待つ `ci-wait.sh` と共有している。詳細はそちらのヘッダを読むこと。
#
# 環境変数:
#   GITHUB_TOKEN / GH_TOKEN   必須（ディスパッチには write 権限が要る）
#   VW_REPO                   owner/repo（既定は ci-common.sh）
#   CI_DEBUG_POLL             ポーリング間隔・秒（既定 15。--poll と同じ）
#   CI_DEBUG_TIMEOUT          待機の上限・秒（既定 2700 = 45 分。ジョブ側の
#                             timeout-minutes と同じ。--timeout と同じ）
#   その他の共通設定（HTTP の上限・生存出力の間隔など）は ci-common.sh を参照。
#
# 終了ステータス: run の conclusion が success なら 0、それ以外（失敗・締切超過・
# API 断念）は 1。使い方の誤りや起動失敗は 2。
#
set -uo pipefail

# 待機の土台（必ず exit するための歯止め・API ヘルパ・ポーリングループ）を読み込む。
CI_TOOL="ci-debug"
CI_DEBUG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=scripts/ci-common.sh
. "$CI_DEBUG_DIR/ci-common.sh" || {
	echo "ci-debug: error: scripts/ci-common.sh を読み込めません" >&2
	exit 2
}

WORKFLOW_FILE="ci-debug.yml"
POLL="${CI_DEBUG_POLL:-15}"
TIMEOUT="${CI_DEBUG_TIMEOUT:-2700}"

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
			echo "ci-debug: run 一覧を取得できません（HTTP $code）: $(api_message "$body")" >&2
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

# probe_run: run の状態を 1 回調べる（poll_until 用の probe）。締切・生存出力・
# API 連続失敗の打ち切りは poll_until が持つので、ここは「今どうなっているか」だけを見る。
# 完了したら conclusion を PROBE_CONCLUSION へ入れて 0 を返す。
PROBE_RUN_ID=""
PROBE_BODY=""
PROBE_CONCLUSION=""

probe_run() {
	local code status
	code="$(api_json "$VW_API/actions/runs/$PROBE_RUN_ID" "$PROBE_BODY")"
	if [ "$code" != "200" ]; then
		echo "ci-debug: run の状態取得に失敗（HTTP $code, ${POLL_ELAPSED}s）" >&2
		if fatal_http "$code"; then
			echo "ci-debug: 回復しないエラーなので待機を打ち切ります: $(api_message "$PROBE_BODY")" >&2
			return 2
		fi
		return 3
	fi
	status="$(jq -r '.status // empty' "$PROBE_BODY" 2>/dev/null)"
	POLL_STATUS="status=${status:-unknown}"
	if [ "$status" = "completed" ]; then
		PROBE_CONCLUSION="$(jq -r '.conclusion // empty' "$PROBE_BODY" 2>/dev/null)"
		return 0
	fi
	return 1
}

# wait_run <run-id>: completed になるまで待ち、conclusion を echo する。
#
# 終了経路は 3 つだけで、**どれも有限時間で必ず返る**（歯止めは ci-common.sh）:
#   * completed を観測 → conclusion（成功経路）
#   * 締切（TIMEOUT）超過 → timed-out-waiting
#   * API の連続失敗が上限に達した / 回復しないエラー → api-error
wait_run() {
	PROBE_RUN_ID="$1"
	PROBE_BODY="$(workfile)"
	PROBE_CONCLUSION=""
	poll_until probe_run
	case "$?" in
		0)
			printf '%s\n' "${PROBE_CONCLUSION:-unknown}"
			return 0
			;;
		1)
			printf '%s\n' "timed-out-waiting"
			return 1
			;;
		*)
			printf '%s\n' "api-error"
			return 1
			;;
	esac
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
require_positive_int "$POLL" "--poll"
require_positive_int "$TIMEOUT" "--timeout"

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
