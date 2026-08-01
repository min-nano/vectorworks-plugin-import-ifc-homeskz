#!/usr/bin/env bash
#
# r2-publish.sh — ビルド成果物を Cloudflare R2 へ公開する（CI 専用の書き込み側）。
#
# アップデータの取得元は GitHub Releases ではなく R2 バケットです。GitHub の
# プレリリース API はクライアントからの取得が不安定（アセット URL がリダイレクト
# を伴う・レート制限・未認証での取りこぼし）だったため、配布実体を R2 へ移し、
# クライアント（scripts/vw-update.sh / vw-update.ps1）は R2 上の小さな JSON
# マニフェストだけを読む、という契約に変えています。このスクリプトはその
# 「書き込み側」で、.github/workflows/build.yml と cleanup-dev-release.yml から
# 呼ばれます。GitHub Releases 自体は残りますが、アセットは添付せず R2 への
# リンクを本文に載せるだけの「人が辿る記録」になります。
#
# バケットのレイアウト（キーはバケットルートからの相対）:
#
#   stable/manifest.json                       現在の stable ビルド（no-cache）
#   stable/<short>/HomeskzIfcImport.vwlibrary.zip
#   stable/<short>/HomeskzIfcImport.vlb.zip
#   dev/index.json                             dev ビルドの一覧（no-cache）
#   dev/<slug>/manifest.json                   ブランチごとの最新 dev ビルド
#   dev/<slug>/<short>/HomeskzIfcImportDev.vwlibrary.zip
#   dev/<slug>/<short>/HomeskzIfcImportDev.vlb.zip
#
# zip を「コミット短縮 SHA のフォルダ」へ置くのは意図的です。同じキーを上書きすると
# CDN のキャッシュに古い実体が残り、まさに今回避けたい「不安定さ」を招きます。zip は
# 内容アドレス的に不変なので長期キャッシュ可能（immutable）にし、可変なのは manifest /
# index だけ（no-cache）——という分担にしてあります。古い <short>/ は公開のたびに
# 掃除するので溜まりません。
#
# マニフェストのスキーマ（stable/manifest.json と dev/<slug>/manifest.json 共通）:
#
#   {
#     "schema": 1,
#     "channel": "stable" | "dev",
#     "branch":  "main" | "feature/x",     dev ピッカーに出す表示名
#     "slug":    "feature-x",              dev のみ（バケット上のフォルダ名）
#     "commit":  "<40 桁 SHA>",
#     "short":   "<7 桁 SHA>",             インストール済みビルドとの比較に使う
#     "built":   "<ISO8601 UTC>",
#     "mac":     "<.vwlibrary.zip の公開 URL>",
#     "win":     "<.vlb.zip の公開 URL>"
#   }
#
# dev/index.json は上のオブジェクトを集めたもの:
#
#   { "schema": 1, "generated": "<ISO8601 UTC>", "builds": [ … ] }
#
# コマンド:
#   publish-stable --commit <sha> --dist <dir>
#   publish-dev    --commit <sha> --branch <name> --dist <dir>
#   remove-dev     --branch <name>
#   reindex-dev
#   slug           --branch <name>          （ワークフローと slug 規則を共有するため）
#
# 環境変数:
#   VW_R2_BUCKET        バケット名（必須）
#   VW_R2_ACCOUNT_ID    Cloudflare アカウント ID（VW_R2_ENDPOINT を渡すなら不要）
#   VW_R2_ENDPOINT      S3 API のエンドポイント（既定: アカウント ID から組み立て）
#   VW_R2_PUBLIC_BASE   公開ベース URL（例 https://dist.example.com。必須）
#   AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY   R2 の API トークン（必須）
#
# 必要なツール: aws-cli v2 と python3（GitHub Actions の ubuntu ランナーに同梱）。
#
set -euo pipefail

# R2 は S3 互換だが、aws-cli v2 が既定で送る追加チェックサムヘッダを受け付けない
# （501 Not Implemented になる）。必要なときだけ送る設定へ落とし、リージョンは
# R2 の決め打ち "auto" にする。呼び出し側は資格情報だけ渡せばよい。
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-auto}"
export AWS_REQUEST_CHECKSUM_CALCULATION="${AWS_REQUEST_CHECKSUM_CALCULATION:-when_required}"
export AWS_RESPONSE_CHECKSUM_VALIDATION="${AWS_RESPONSE_CHECKSUM_VALIDATION:-when_required}"

MAC_ASSET_STABLE="HomeskzIfcImport.vwlibrary.zip"
WIN_ASSET_STABLE="HomeskzIfcImport.vlb.zip"
MAC_ASSET_DEV="HomeskzIfcImportDev.vwlibrary.zip"
WIN_ASSET_DEV="HomeskzIfcImportDev.vlb.zip"

# manifest / index は毎ビルド書き換わるので、CDN にもブラウザにも溜めさせない。
# zip は <short>/ 配下で不変なので、逆に最大限キャッシュさせる。
JSON_CACHE="no-cache, max-age=0, must-revalidate"
ZIP_CACHE="public, max-age=31536000, immutable"

die() { # message
	echo "::error::$1" >&2
	exit 1
}

# ---------------------------------------------------------------------------
# 設定の解決と検証。足りないものは「何を設定すればよいか」まで含めて落とす。
# ---------------------------------------------------------------------------
require_config() {
	command -v aws >/dev/null 2>&1 || die "aws-cli が見つかりません。"
	command -v python3 >/dev/null 2>&1 || die "python3 が見つかりません。"

	[ -n "${VW_R2_BUCKET:-}" ] || die "VW_R2_BUCKET が未設定です。"
	[ -n "${VW_R2_PUBLIC_BASE:-}" ] || die "VW_R2_PUBLIC_BASE が未設定です。"
	[ -n "${AWS_ACCESS_KEY_ID:-}" ] || die "AWS_ACCESS_KEY_ID が未設定です。"
	[ -n "${AWS_SECRET_ACCESS_KEY:-}" ] || die "AWS_SECRET_ACCESS_KEY が未設定です。"

	if [ -z "${VW_R2_ENDPOINT:-}" ]; then
		[ -n "${VW_R2_ACCOUNT_ID:-}" ] \
			|| die "VW_R2_ENDPOINT も VW_R2_ACCOUNT_ID も未設定です。"
		VW_R2_ENDPOINT="https://${VW_R2_ACCOUNT_ID}.r2.cloudflarestorage.com"
	fi
	# 末尾のスラッシュはこちらで付けるので落としておく（"…//key" を作らない）。
	VW_R2_PUBLIC_BASE="${VW_R2_PUBLIC_BASE%/}"
}

# ブランチ名 → バケット上のフォルダ名。GitHub Releases のタグ用 slug と同じ規則を
# 使い続ける（既存のクリーンアップ経路・ドキュメントと食い違わせないため）。
slugify() { # branch
	printf '%s' "$1" | tr '/:@ ' '----' | tr -c 'A-Za-z0-9._-' '-'
}

now_utc() { date -u +%Y-%m-%dT%H:%M:%SZ; }

public_url() { # key
	printf '%s/%s' "$VW_R2_PUBLIC_BASE" "$1"
}

# ---------------------------------------------------------------------------
# R2（S3 API）の薄いラッパー。put は 3 回までリトライする——公開の取りこぼしは
# 気づかれにくく、ここが配布の唯一の経路になったため。
# ---------------------------------------------------------------------------
r2_put() { # local-file, key, content-type, cache-control
	local attempt=1
	while :; do
		if aws s3 cp "$1" "s3://${VW_R2_BUCKET}/$2" \
			--endpoint-url "$VW_R2_ENDPOINT" \
			--content-type "$3" \
			--cache-control "$4" \
			--only-show-errors; then
			echo "  put s3://${VW_R2_BUCKET}/$2"
			return 0
		fi
		[ "$attempt" -lt 3 ] || return 1
		echo "  put failed ($2); retrying..." >&2
		sleep $((attempt * 5))
		attempt=$((attempt + 1))
	done
}

r2_get() { # key, local-file -> 0 if the object exists and was downloaded
	aws s3 cp "s3://${VW_R2_BUCKET}/$1" "$2" \
		--endpoint-url "$VW_R2_ENDPOINT" --only-show-errors 2>/dev/null
}

# 指定プレフィックス配下の全キーを 1 行 1 件で出す（aws-cli が自動でページングする）。
r2_keys() { # prefix
	aws s3api list-objects-v2 \
		--bucket "$VW_R2_BUCKET" \
		--prefix "$1" \
		--endpoint-url "$VW_R2_ENDPOINT" \
		--output json |
		python3 -c '
import json, sys

# aws が失敗すると stdout は空になる。ここで例外を吐いても意味は無い（呼び出し側は
# pipefail で aws の終了コードを見ている）ので、静かに 0 件として返す。
data = sys.stdin.read()
if data.strip():
    for obj in json.loads(data).get("Contents", []):
        print(obj["Key"])
'
}

r2_rm() { # key
	aws s3 rm "s3://${VW_R2_BUCKET}/$1" \
		--endpoint-url "$VW_R2_ENDPOINT" --only-show-errors
	echo "  rm  s3://${VW_R2_BUCKET}/$1"
}

r2_rm_prefix() { # prefix
	aws s3 rm "s3://${VW_R2_BUCKET}/$1" --recursive \
		--endpoint-url "$VW_R2_ENDPOINT" --only-show-errors
	echo "  rm  s3://${VW_R2_BUCKET}/$1* (recursive)"
}

# ---------------------------------------------------------------------------
# JSON の組み立て。ブランチ名は外から来る文字列なので、手で組み立てず python の
# json モジュールにエスケープさせる。
# ---------------------------------------------------------------------------
write_manifest() { # out-file channel branch slug commit short mac-url win-url built
	python3 - "$@" <<'PY'
import json, sys

out, channel, branch, slug, commit, short, mac, win, built = sys.argv[1:10]
doc = {
    "schema": 1,
    "channel": channel,
    "branch": branch,
    "commit": commit,
    "short": short,
    "built": built,
    "mac": mac,
    "win": win,
}
if slug:
    doc["slug"] = slug
with open(out, "w", encoding="utf-8") as fh:
    json.dump(doc, fh, ensure_ascii=False, indent=2, sort_keys=True)
    fh.write("\n")
PY
}

# ダウンロード済みの dev マニフェスト群（1 ディレクトリ）から index.json を作る。
# 並び順は branch → slug で固定する（列挙順に結果を左右させない）。
write_index() { # out-file, manifest-dir, generated
	python3 - "$@" <<'PY'
import glob, json, os, sys

out, srcdir, generated = sys.argv[1:4]
builds = []
for path in sorted(glob.glob(os.path.join(srcdir, "*.json"))):
    with open(path, encoding="utf-8") as fh:
        try:
            builds.append(json.load(fh))
        except json.JSONDecodeError:
            # 壊れたマニフェスト 1 件で一覧全体を落とさない（クライアント側の
            # 寛容さと同じ方針）。
            print("skipping malformed manifest: %s" % path, file=sys.stderr)
builds.sort(key=lambda b: (b.get("branch", ""), b.get("slug", "")))
with open(out, "w", encoding="utf-8") as fh:
    json.dump({"schema": 1, "generated": generated, "builds": builds},
              fh, ensure_ascii=False, indent=2, sort_keys=True)
    fh.write("\n")
print("index: %d build(s)" % len(builds))
PY
}

# ---------------------------------------------------------------------------
# 掃除: あるプレフィックス直下の「今回以外の <short>/」を消す。
# ---------------------------------------------------------------------------
# 一覧に失敗した場合は 1 件も読めず、単に何も消さずに終わる（掃除の取りこぼしは
# 次の公開で回収されるので、消しすぎるより安全な側に倒れている）。
prune_old_builds() { # prefix (e.g. "stable/" or "dev/<slug>/"), keep-short
	local prefix="$1" keep="$2" key rest sub
	while IFS= read -r key; do
		[ -n "$key" ] || continue
		rest="${key#"$prefix"}"
		# 直下のファイル（manifest.json など）は対象外。
		case "$rest" in
			*/*) sub="${rest%%/*}" ;;
			*) continue ;;
		esac
		[ "$sub" != "$keep" ] || continue
		r2_rm "$key"
	done < <(r2_keys "$prefix")
}

# ---------------------------------------------------------------------------
# dev/index.json の再生成。
#
# PR ごとのビルドは同時に走りうるので、一覧の作り直しには「読んで・書いて・
# もう一度読む」を入れる。書いた後に対象集合が変わっていたら、他のビルドが
# 割り込んだということなので作り直す（自分の書き込みで相手を消したままにしない）。
# 3 回で収束しなければ警告だけ残す——次のビルドが必ず作り直すので恒久的な欠落には
# ならない。
# ---------------------------------------------------------------------------
# 一覧の取得は「失敗」と「1 件も無い」を厳密に区別する。ここを取り違えて grep の
# 空振り（終了コード 1）と API エラーを同じ扱いにすると、通信エラーのときに「dev
# ビルドは 0 件」と解釈して index.json を空で上書きし、全ブランチの dev ビルドを
# ピッカーから消してしまう。したがって r2_keys の失敗はそのまま伝播させ、grep の
# 空振りだけを握りつぶす。
dev_manifest_keys() {
	local all
	all="$(r2_keys "dev/")" || return 1
	printf '%s\n' "$all" | grep -E '^dev/[^/]+/manifest\.json$' | sort || true
}

reindex_dev() {
	local attempt=1 keys after work idx i key
	while [ "$attempt" -le 3 ]; do
		if ! keys="$(dev_manifest_keys)"; then
			echo "::warning::dev/ の一覧を取得できなかったため index.json は更新しません（空で上書きしない）。"
			return 0
		fi

		work="$(mktemp -d)"
		i=0
		while IFS= read -r key; do
			[ -n "$key" ] || continue
			i=$((i + 1))
			# 1 度だけ再試行する。それでも読めないものは、一覧した直後に
			# remove-dev が消したなどの理由が考えられるので飛ばす（対象集合が
			# 変わったことは下の再確認が検出して作り直す）。
			r2_get "$key" "$work/$(printf '%04d' "$i").json" \
				|| r2_get "$key" "$work/$(printf '%04d' "$i").json" \
				|| echo "::warning::$key を読めなかったため一覧から除外しました。" >&2
		done <<<"$keys"

		idx="$(mktemp)"
		write_index "$idx" "$work" "$(now_utc)"
		r2_put "$idx" "dev/index.json" "application/json" "$JSON_CACHE" \
			|| die "dev/index.json のアップロードに失敗しました。"
		rm -rf "$work" "$idx"

		# 書いた直後に一覧を取り直す。変わっていなければ収束（同時に走った
		# ビルドがいても、両者が同じ集合を書いたことになる）。
		after="$(dev_manifest_keys)" || return 0
		if [ "$after" = "$keys" ]; then
			return 0
		fi
		echo "dev/index.json: 対象が更新されたので作り直します（試行 ${attempt}）。"
		attempt=$((attempt + 1))
	done
	echo "::warning::dev/index.json が収束しませんでした（同時ビルドの可能性）。次のビルドで作り直されます。"
}

# ---------------------------------------------------------------------------
# コマンド本体。
# ---------------------------------------------------------------------------
publish_channel() { # channel, commit, branch, dist-dir
	local channel="$1" commit="$2" branch="$3" dist="$4"
	local short="${commit:0:7}"
	local slug="" prefix mac_asset win_asset mac_key win_key manifest built

	if [ "$channel" = "stable" ]; then
		prefix="stable/"
		mac_asset="$MAC_ASSET_STABLE"
		win_asset="$WIN_ASSET_STABLE"
	else
		slug="$(slugify "$branch")"
		prefix="dev/${slug}/"
		mac_asset="$MAC_ASSET_DEV"
		win_asset="$WIN_ASSET_DEV"
	fi

	[ -f "$dist/$mac_asset" ] || die "$dist/$mac_asset がありません。"
	[ -f "$dist/$win_asset" ] || die "$dist/$win_asset がありません。"

	mac_key="${prefix}${short}/${mac_asset}"
	win_key="${prefix}${short}/${win_asset}"

	echo "Publishing ${channel} build ${short} (branch: ${branch})"
	r2_put "$dist/$mac_asset" "$mac_key" "application/zip" "$ZIP_CACHE" \
		|| die "$mac_asset のアップロードに失敗しました。"
	r2_put "$dist/$win_asset" "$win_key" "application/zip" "$ZIP_CACHE" \
		|| die "$win_asset のアップロードに失敗しました。"

	# zip を置いてから manifest を差し替える。順序を逆にすると、まだ存在しない
	# zip を指す manifest を一瞬でもクライアントに見せてしまう。
	built="$(now_utc)"
	manifest="$(mktemp)"
	write_manifest "$manifest" "$channel" "$branch" "$slug" "$commit" "$short" \
		"$(public_url "$mac_key")" "$(public_url "$win_key")" "$built"
	r2_put "$manifest" "${prefix}manifest.json" "application/json" "$JSON_CACHE" \
		|| die "manifest.json のアップロードに失敗しました。"
	cat "$manifest"
	rm -f "$manifest"

	# 古いビルドの zip を掃除（manifest.json / index.json は残す）。
	prune_old_builds "$prefix" "$short"

	if [ "$channel" = "dev" ]; then
		reindex_dev
	fi

	# 呼び出し元（ワークフロー）がリリース本文へ URL を載せられるように出す。
	if [ -n "${GITHUB_OUTPUT:-}" ]; then
		{
			echo "mac_url=$(public_url "$mac_key")"
			echo "win_url=$(public_url "$win_key")"
			echo "manifest_url=$(public_url "${prefix}manifest.json")"
			echo "slug=${slug}"
		} >>"$GITHUB_OUTPUT"
	fi
}

usage() {
	sed -n '2,60p' "$0" >&2
	exit 2
}

main() {
	local cmd="${1:-}"
	shift || true

	local commit="" branch="" dist=""
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--commit)
				commit="${2:-}"
				shift 2
				;;
			--branch)
				branch="${2:-}"
				shift 2
				;;
			--dist)
				dist="${2:-}"
				shift 2
				;;
			*) die "不明な引数: '$1'" ;;
		esac
	done

	case "$cmd" in
		slug)
			# 設定を要さない純粋なヘルパー（ワークフローがタグ名を作るのに使う）。
			[ -n "$branch" ] || die "--branch が必要です。"
			printf '%s\n' "$(slugify "$branch")"
			;;
		publish-stable)
			require_config
			[ -n "$commit" ] || die "--commit が必要です。"
			[ -n "$dist" ] || die "--dist が必要です。"
			publish_channel "stable" "$commit" "${branch:-main}" "$dist"
			;;
		publish-dev)
			require_config
			[ -n "$commit" ] || die "--commit が必要です。"
			[ -n "$branch" ] || die "--branch が必要です。"
			[ -n "$dist" ] || die "--dist が必要です。"
			publish_channel "dev" "$commit" "$branch" "$dist"
			;;
		remove-dev)
			require_config
			[ -n "$branch" ] || die "--branch が必要です。"
			r2_rm_prefix "dev/$(slugify "$branch")/"
			reindex_dev
			;;
		reindex-dev)
			require_config
			reindex_dev
			;;
		*) usage ;;
	esac
}

main "$@"
