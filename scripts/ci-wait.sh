#!/usr/bin/env bash
#
# ci-wait.sh — PR / ブランチ / コミットの **CI（チェック）が全部終わるまで待って、
# 終わった瞬間に exit する**クライアント。
#
# なぜこれがあるか
# ----------------
# リモートセッションから CI の完了を知る手段は「完了したら exit するプロセスを
# バックグラウンドで走らせる」ことだけである（PR 購読は**失敗しか配信しない**）。
# ところが待機を毎回その場で書くと、
#
#   * `sleep` を挟んだ手書きループは締切もウォッチドッグも持たないので、API が
#     固まると永久にぶら下がる。**CI は終わっているのにセッションが気付かない。**
#   * 「チェックがまだ 1 件も登録されていない」状態を「完了」と誤判定して、CI が
#     始まってすらいないのに成功として返ってしまう。
#   * 待っている間に新しい push が入ると、古い run はキャンセルされ、その結果を
#     見て「CI 済み」と判断してしまう。
#
# という壊れ方をする（いずれも実際に起きた）。だからこのスクリプトを使う。
# **CI の完了待ちを手書きのループでやらないこと。**
#
# 使い方
# ------
#   scripts/ci-wait.sh                       いま checkout しているブランチの CI を待つ
#   scripts/ci-wait.sh --pr 34               PR の head（追随あり）
#   scripts/ci-wait.sh --ref main            ブランチ / タグ
#   scripts/ci-wait.sh --sha 728a572…        固定のコミット
#
# Claude Code からは **必ず** バックグラウンドで投げる（exit が完了通知になる）:
#
#   Bash(run_in_background: true): scripts/ci-wait.sh --pr 34
#
#   オプション:
#     --pr N        PR 番号。head コミットを追いかける
#     --ref R       ブランチ名・タグ名（既定: いま checkout しているブランチ）
#     --sha S       コミット SHA を直接指定（追随しない）
#     --no-follow   待機中に head が動いても追随せず、head-moved として終わる
#     --grace S     チェックが 1 件も現れないときに待つ猶予・秒（既定 180）
#     --poll S      ポーリング間隔・秒（既定 20）
#     --timeout S   待機の上限・秒（既定 3600 = 60 分）
#
# 出力の最終行は必ず
#
#   ci-wait: done (conclusion=<結果> exit=<終了コード>)
#
# で、この行が無ければ「まだ動いている」か「外から殺された」かのどちらか。
# conclusion は success / failure / cancelled / no-checks / head-moved /
# timed-out-waiting / api-error のいずれか。**success 以外は exit 1。**
#
# `timed-out-waiting` / `api-error` は「CI が失敗した」ではなく**待機側が見届けられ
# なかった**という意味で、CI 自体はまだ動いているかもしれない（同じ行に合流用の
# コマンドが出る）。
#
# **必ず有限時間で exit すること**が設計の要。その歯止め（HTTP の時間上限・締切判定・
# ウォッチドッグ）と待機ループ本体は `ci-common.sh` にあり、`ci-debug.sh` と共有して
# いる。詳細はそちらのヘッダを読むこと。
#
# 環境変数:
#   GITHUB_TOKEN / GH_TOKEN   必須（読み取り権限だけでよい）
#   VW_REPO                   owner/repo（既定は ci-common.sh）
#   CI_WAIT_POLL              ポーリング間隔・秒（既定 20。--poll と同じ）
#   CI_WAIT_TIMEOUT           待機の上限・秒（既定 3600。--timeout と同じ）
#   CI_WAIT_GRACE             チェック出現の猶予・秒（既定 180。--grace と同じ）
#   その他の共通設定は ci-common.sh を参照。
#
# 終了ステータス: 全チェックが成功なら 0、それ以外は 1。使い方の誤りは 2。
#
set -uo pipefail

# 待機の土台（必ず exit するための歯止め・API ヘルパ・ポーリングループ）を読み込む。
CI_TOOL="ci-wait"
CI_WAIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=scripts/ci-common.sh
. "$CI_WAIT_DIR/ci-common.sh" || {
	echo "ci-wait: error: scripts/ci-common.sh を読み込めません" >&2
	exit 2
}

POLL="${CI_WAIT_POLL:-20}"
TIMEOUT="${CI_WAIT_TIMEOUT:-3600}"
# チェックが 1 件も無い状態の扱い。push 直後は Actions がチェックを登録するまで
# 十数秒〜数十秒かかるので、その間を「完了（＝チェック 0 件で成功）」と誤判定しない
# ための猶予。猶予を過ぎても 0 件なら no-checks として終わる（黙って成功にしない）。
GRACE="${CI_WAIT_GRACE:-180}"

# ---------------------------------------------------------------------------
# 引数解析
# ---------------------------------------------------------------------------

# usage: ヘッダのコメントブロックをそのままヘルプとして出す（唯一の説明を二重に
# 持たないため）。行番号で切ると header を書き足すたびにずれるので、shebang の次から
# 最初の非コメント行の手前までを出す。
usage() {
	awk 'NR > 1 { if ($0 !~ /^#/) exit; print }' "$0"
}

PR=""
REF=""
SHA=""
FOLLOW=1

while [ "$#" -gt 0 ]; do
	case "$1" in
		--pr) PR="${2:-}" ; shift 2 ;;
		--ref) REF="${2:-}" ; shift 2 ;;
		--sha) SHA="${2:-}" ; shift 2 ;;
		--no-follow) FOLLOW=0 ; shift ;;
		--grace) GRACE="${2:-}" ; shift 2 ;;
		--poll) POLL="${2:-}" ; shift 2 ;;
		--timeout) TIMEOUT="${2:-}" ; shift 2 ;;
		-h | --help) usage ; exit 0 ;;
		*) die "未知のオプション: $1" ;;
	esac
done

[ -n "$TOKEN" ] || die "GITHUB_TOKEN / GH_TOKEN が未設定です"

# 締切・間隔・猶予は必ず数値であること。空文字や誤字がそのまま通ると比較が常に偽に
# なり、締切が効かない＝ぶら下がる。
require_positive_int "$POLL" "--poll"
require_positive_int "$TIMEOUT" "--timeout"
case "$GRACE" in '' | *[!0-9]*) die "--grace は秒数（整数）で指定してください: '$GRACE'" ;; esac

# 指定は 1 つだけ。複数あると「どれを待っているのか」が出力から読めなくなる。
targets=0
[ -n "$PR" ] && targets=$((targets + 1))
[ -n "$REF" ] && targets=$((targets + 1))
[ -n "$SHA" ] && targets=$((targets + 1))
[ "$targets" -le 1 ] || die "--pr / --ref / --sha は 1 つだけ指定してください"

if [ "$targets" -eq 0 ]; then
	REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
	if [ -z "$REF" ] || [ "$REF" = "HEAD" ]; then
		die "--pr / --ref / --sha のいずれかを指定してください（現在のブランチを判定できません）"
	fi
fi

# SHA 直指定は「そのコミット」が対象なので追随しようがない。
[ -n "$SHA" ] && FOLLOW=0

# ---------------------------------------------------------------------------
# 対象の解決
# ---------------------------------------------------------------------------

TARGET_SHA=""
TARGET_DESC=""
PR_STATE=""
RESOLVED_SHA=""

# resolve_head: 対象の現在の head SHA を **RESOLVED_SHA へ**入れる（成功なら 0）。
# 待機開始時と、追随のために完了時にも呼ぶ。
#
# stdout ではなくグローバルへ返すのは、`$(resolve_head)` だとサブシェルになって
# PR_STATE のような副次の情報が親へ返らないため。
resolve_head() {
	local body code sha
	RESOLVED_SHA=""
	body="$(workfile)"
	if [ -n "$PR" ]; then
		code="$(api_json "$VW_API/pulls/$PR" "$body")"
		if [ "$code" != "200" ]; then
			echo "ci-wait: PR #$PR を取得できません（HTTP $code）: $(api_message "$body")" >&2
			rm -f "$body"
			return 1
		fi
		sha="$(jq -r '.head.sha // empty' "$body" 2>/dev/null)"
		PR_STATE="$(jq -r 'if .merged then "merged" else .state end' "$body" 2>/dev/null)"
	elif [ -n "$REF" ]; then
		code="$(api_json "$VW_API/commits/$REF" "$body")"
		if [ "$code" != "200" ]; then
			echo "ci-wait: ref '$REF' を解決できません（HTTP $code）: $(api_message "$body")" >&2
			echo "ci-wait: ブランチを push 済みか確認してください（ローカルにしか無い ref は解決できません）" >&2
			rm -f "$body"
			return 1
		fi
		sha="$(jq -r '.sha // empty' "$body" 2>/dev/null)"
	else
		code="$(api_json "$VW_API/commits/$SHA" "$body")"
		if [ "$code" != "200" ]; then
			echo "ci-wait: コミット '$SHA' を取得できません（HTTP $code）: $(api_message "$body")" >&2
			rm -f "$body"
			return 1
		fi
		sha="$(jq -r '.sha // empty' "$body" 2>/dev/null)"
	fi
	rm -f "$body"
	[ -n "$sha" ] || return 1
	RESOLVED_SHA="$sha"
	return 0
}

# ---------------------------------------------------------------------------
# チェックの取得と判定
# ---------------------------------------------------------------------------

# 「悪い」結論。ここに載るものが 1 つでもあれば全体は失敗扱い。cancelled を成功側に
# 入れないのは、**新しい push で古い run がキャンセルされた**ときに「CI 済み」と
# 誤読するのを防ぐため（実際にそれで取り違えた）。
BAD_CONCLUSIONS='["failure","timed_out","cancelled","action_required","startup_failure","stale"]'

CHECKS_BODY=""
STATUS_BODY=""
CHECK_CONCLUSION=""
GRACE_BASE=0

# probe_checks: 対象コミットのチェックを 1 回調べる（poll_until 用の probe）。
#
#   * まだ全部 completed でない → 1（継続）
#   * チェックが 0 件 → 猶予の内は 1、猶予を過ぎたら no-checks として 0
#   * 全部 completed → 追随の要否を見てから 0
#
# 締切・生存出力・API 連続失敗の打ち切りは poll_until が持つので、ここは状態を
# 見るだけでよい。
#
# shellcheck disable=SC2317 # poll_until から名前で間接的に呼ばれる。
probe_checks() {
	local code total completed pending head

	code="$(api_json "$VW_API/commits/$TARGET_SHA/check-runs?filter=latest&per_page=100" "$CHECKS_BODY")"
	if [ "$code" != "200" ]; then
		echo "ci-wait: チェックの取得に失敗（HTTP $code, ${POLL_ELAPSED}s）" >&2
		if fatal_http "$code"; then
			echo "ci-wait: 回復しないエラーなので待機を打ち切ります: $(api_message "$CHECKS_BODY")" >&2
			return 2
		fi
		return 3
	fi

	total="$(jq -r '.check_runs | length' "$CHECKS_BODY" 2>/dev/null)"
	completed="$(jq -r '[.check_runs[] | select(.status == "completed")] | length' "$CHECKS_BODY" 2>/dev/null)"
	[ -n "$total" ] || return 3

	if [ "$total" -eq 0 ]; then
		if [ "$((POLL_ELAPSED - GRACE_BASE))" -lt "$GRACE" ]; then
			POLL_STATUS="checks=まだ登録されていません（${GRACE}s まで待ちます）"
			return 1
		fi
		CHECK_CONCLUSION="no-checks"
		return 0
	fi

	POLL_STATUS="checks=${completed}/${total} 完了"
	[ "$completed" -eq "$total" ] || return 1

	# チェックランが全部終わっていても、旧来のコミットステータス（Actions 以外の
	# 連携が付けるもの）が pending のことがある。0 件のときは state が "pending" に
	# なる仕様なので、**件数で場合分けしてから**見る。
	code="$(api_json "$VW_API/commits/$TARGET_SHA/status" "$STATUS_BODY")"
	if [ "$code" = "200" ]; then
		pending="$(jq -r 'if (.total_count // 0) > 0 and .state == "pending" then "yes" else "no" end' \
			"$STATUS_BODY" 2>/dev/null)"
		if [ "$pending" = "yes" ]; then
			POLL_STATUS="checks=${completed}/${total} 完了 / commit status=pending"
			return 1
		fi
	fi

	# **完了を返す前に head を見直す。** 待っている間に push が入っていると、いま見た
	# 結果は「1 つ前のコミットの CI」でしかない。追随する設定ならそちらへ乗り換え、
	# しない設定なら head-moved として終わる（古い結果を成功として返さない）。
	if [ -n "$PR" ] || [ -n "$REF" ]; then
		head=""
		resolve_head && head="$RESOLVED_SHA"
		if [ -n "$head" ] && [ "$head" != "$TARGET_SHA" ]; then
			if [ "$FOLLOW" -eq 1 ]; then
				echo "ci-wait: head が動きました: ${TARGET_SHA:0:7} → ${head:0:7}（新しい head の CI を待ちます）" >&2
				TARGET_SHA="$head"
				GRACE_BASE="$POLL_ELAPSED"
				POLL_STATUS=""
				return 1
			fi
			CHECK_CONCLUSION="head-moved"
			return 0
		fi
	fi

	CHECK_CONCLUSION="$(jq -r --argjson bad "$BAD_CONCLUSIONS" '
		[.check_runs[].conclusion // "unknown"] as $c
		| if ($c | map(select(. as $x | $bad | index($x))) | length) > 0 then "failure"
		  elif ($c | map(select(. == "success" or . == "skipped" or . == "neutral")) | length) == ($c | length) then "success"
		  else "unknown" end' "$CHECKS_BODY" 2>/dev/null)"
	return 0
}

# print_checks: チェックの一覧を読みやすく出す（失敗したものには URL を付ける）。
print_checks() {
	jq -r '.check_runs
		| sort_by(.name)
		| .[]
		| (if .conclusion == "success" then "  OK  "
		   elif .conclusion == "skipped" or .conclusion == "neutral" then "  --  "
		   else "  NG  " end) + .name + "  " + (.conclusion // .status)
		  + (if (.conclusion == "success" or .conclusion == "skipped" or .conclusion == "neutral")
		     then "" else "\n        " + (.html_url // "") end)' \
		"$CHECKS_BODY" 2>/dev/null
}

# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------

# 締切＋余裕を過ぎたら自分を殺す。締切判定より外側で何かが固まっても、プロセスは
# 必ず終わる（＝呼び出し側は必ず完了通知を受け取る）。
start_watchdog "$((TIMEOUT + 300))"

CHECKS_BODY="$(workfile)"
STATUS_BODY="$(workfile)"

resolve_head || die "待機対象を解決できませんでした"
TARGET_SHA="$RESOLVED_SHA"
if [ -n "$PR" ]; then
	TARGET_DESC="pr=#$PR (${PR_STATE:-unknown})"
elif [ -n "$REF" ]; then
	TARGET_DESC="ref=$REF"
else
	TARGET_DESC="sha 指定"
fi

echo "$TARGET_DESC sha=$TARGET_SHA"
echo "checks_url=https://github.com/$VW_REPO/commits/$TARGET_SHA/checks"
echo

poll_until probe_checks
case "$?" in
	0) CONCLUSION="${CHECK_CONCLUSION:-unknown}" ;;
	1) CONCLUSION="timed-out-waiting" ;;
	*) CONCLUSION="api-error" ;;
esac

rc=0
[ "$CONCLUSION" = "success" ] || rc=1

echo
echo "sha=$TARGET_SHA"
echo "conclusion=$CONCLUSION"
case "$CONCLUSION" in
	success | failure | unknown)
		echo
		print_checks
		;;
	no-checks)
		echo
		echo "(${GRACE}s 待ってもチェックが 1 件も登録されませんでした。ワークフローの"
		echo "  トリガ条件に合っていないか、push がまだ届いていない可能性があります)"
		;;
	head-moved)
		echo
		echo "(待っている間に head が動きました。--no-follow を外すか、新しい head で"
		echo "  もう一度実行してください)"
		;;
	timed-out-waiting | api-error)
		echo
		echo "(CI の失敗ではなく、待機側が完了を見届けられませんでした — CI はまだ"
		echo "  動いているかもしれません)"
		echo "  合流するには: scripts/ci-wait.sh --sha $TARGET_SHA"
		;;
esac

# 最後に必ず 1 行「終わった」と出す。呼び出し側が出力の末尾だけを見て
# 「exit したのか、まだ動いているのか」を判定できるようにするため。
echo
echo "ci-wait: done (conclusion=$CONCLUSION exit=$rc)"
exit "$rc"
