# CI デバッグ（`ci-debug.yml`）

`.github/workflows/ci-debug.yml` は、**手動ディスパッチ専用**の「CI 上で 1 コマンドだけ
動かす」ワークフローです。SDK が手元に無い環境（クラウド上の開発セッションなど）から、
**本プラグインのコードが SDK でコンパイルできるか**を確かめるために使います。`push` /
`pull_request` では**決して起動せず**、リリースも公開しません（`contents: write` を持たない）。
SDK キャッシュは `build.yml` と同じキーで**読み取り専用**に復元するので、本番ビルドの
キャッシュを汚しません。

**SDK そのものの調査（「この API は SDK にあるか」「どう振る舞うか」）はここでは行いません。**
[SDK リファレンスリポジトリ](https://github.com/min-nano/vectorworks-developer-sdk-reference)で
issue を立て（テンプレート `調査`。どの機能で・何が分かれば実装に入れるかを書く）、あちらの
調査が `Findings/` に反映されるまでその部分の実装に入りません（その間は他の要素や `parse/`
`core/` の作業を進める）。本リポジトリで `sdk-grep` / `sdk-ls` を使うのは、**既に
`Findings/` に載っている宣言を写し取る**（引数の型や名前を確かめる）ときだけです。

**`build.yml` に一時的な調査ステップを挿してはいけません**——戻し忘れる・その commit が
dev プレリリースとして公開される・ccache / SDK キャッシュを汚す、と副作用が大きいためです。

**使い方。** リモートセッションの `GITHUB_TOKEN` は読み取り専用で `actions: write` を
持たない（REST でのディスパッチは 403）ので、**起動は GitHub MCP、待機はスクリプト**の
2 手順です。

```
1. mcp__github__actions_run_trigger
     method: run_workflow, workflow_id: "ci-debug.yml", ref: <ブランチ>,
     inputs: {mode, platform, label, args, script, notify_pr}
     ※ label は一意な文字列にする（これで run を特定する）

2. Bash(run_in_background: true):
     scripts/ci-debug.sh wait --label <label>
```

手順 2 は「run の特定 → 完了待ち → ペイロード抽出」を行い、完了した瞬間に exit します
（`ci-wait` と同じく `sleep` で待たない）。最終行は必ず
`ci-debug: done (conclusion=<結果> exit=<終了コード>)` で、既定の上限は 45 分（ジョブの
`timeout-minutes` と同じ。`--timeout` / `--poll` で変更可）。`timed-out-waiting` /
`api-error` の意味は `ci-wait` と同じです。待機プロセスを失ったら
`scripts/ci-debug.sh wait --label <label>` で合流でき、確実に追いつきたいときは
`--notify-pr <番号>` で完了時に結果を PR コメントとして投稿させられます。

書き込み権限のあるトークン（PAT など）がある環境では、起動と待機をまとめた
`scripts/ci-debug.sh run --mode build --platform windows` が使えます。ローカルの
Claude Code では `gh auth login` 済みならそれで足ります（`ci-common.sh` が `gh auth token`
を使う。[「ローカルセッションの準備」](../live-test/local-session-setup.md)）。

| mode | 用途 | `--args` |
| --- | --- | --- |
| `sdk-grep` | SDK ヘッダを拡張正規表現で検索（`Findings/` に載っている宣言の写し取り用） | 検索パターン |
| `sdk-ls` | ヘッダの全文表示 / パス部分一致の一覧 | ヘッダのパスまたは部分文字列 |
| `build` | configure してビルド（リリース公開はしない） | 単一ターゲット名（省略可） |
| `compile-one` | 1 翻訳単位だけコンパイル（数十秒。Windows 不可） | ソースのパス |
| `shell` | 任意の bash（`--script`）。逃げ道 | — |

`--platform` は `mac`（既定）/ `windows` / `linux`。**`linux` は SDK を用意しない**ので
SDK 非依存コード専用（速い）。`--ref` は既定で現在のブランチ。

**`build` / `compile-one` は本番 CI の代わりになりません。** どちらも clang-tidy を通さずに
コンパイルするだけなので、`tidy-mac` / `tidy-windows` が落とす lint（例:
`readability-uppercase-literal-suffix`）は素通りします。「ci-debug の build が通ったから CI も
通る」と報告しないこと。SDK 依存コードの最終確認は PR の CI が緑になったことで行います。

**結果の読み方。** 出力は必ず次のマーカーで挟まれます。`truncated=yes` なら全部は見えて
いないので、`--args` を絞るか `mode=shell` で件数を数えてください。

```
===== BEGIN PAYLOAD (mode=... platform=...) =====
...
===== END PAYLOAD (exit=N lines_total=N truncated=yes|no) =====
```

`... (annotation truncated by GitHub's 4096-char limit …)` が END の直前に出ていたら、
注釈経路の上限で切られています（END の `lines_total` が本当の行数）。マーカーが無ければ
調査コマンドに到達せずに失敗しており、代わりに理由が出ます。全文はジョブログと
アーティファクト（`ci-debug-<label>`）にありますが、**AI はアーティファクトを取得できない**ので、
モードを足すときは必要な情報を必ずログ側に出してください。

ペイロードの取得経路は 2 つで、`ci-debug.sh` はこの順に試します。

1. **チェックラン注釈**（`GET /repos/{owner}/{repo}/check-runs/{id}/annotations`）。
   `ci-debug-job.sh` がペイロードを `::notice::` としても出しているので、通常はここで取れます
   （`api.github.com` だけで完結し、ログのノイズも混ざらない）。
2. **ジョブログ**。ログ API は署名付きの Azure Blob Storage へ 302 で飛びますが、そのホストは
   組織の egress ポリシーで拒否されている（`curl: (56) CONNECT tunnel failed, response 403`）
   ので、コンテナからは取れません。**迂回してはならない制約**なので、必要なときは GitHub MCP の
   `get_job_logs`（`job_id` 指定・`return_content: true`）を使います（全ログが文脈に入るので、
   注釈で足りるならそちらで済ませる）。

**制約。** `workflow_dispatch` は**デフォルトブランチに存在するワークフロー**しか起動できません。
**モードの追加・修正は `scripts/ci-debug-job.sh`（ランナー側）で行います**——ワークフロー本体は
薄く保ってあるので、作業ブランチに push するだけで新しいモードを試せます（`--ref` がその
ブランチのため）。ワークフロー本体を変えると main へのマージが要ります。
