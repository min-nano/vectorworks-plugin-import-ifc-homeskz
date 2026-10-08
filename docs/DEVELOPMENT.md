# 開発ガイド

このプラグインをビルド・テスト・lint し、CI とリリースを回すための手順です。トピックごとに
[`docs/development/`](development/) の下へ分けてあり、**このページはその目次**です。

- 利用者向けの説明（何をするプラグインか・使い方）は [`README.md`](../README.md)。
- 設計判断・ホームズ君 IFC の癖は [`DEV-NOTES.md`](DEV-NOTES.md)。**Vectorworks SDK の
  実測知見は [SDK リファレンスリポジトリ](https://github.com/min-nano/vectorworks-developer-sdk-reference)の
  `Findings/`**（下記「SDK ドキュメント」）。
- 全変更に共通する規約（アーキテクチャ・依存の向き・コード規約・テスト方針・PR とマージの
  規則）は [`CLAUDE.md`](../CLAUDE.md)。ここには、そこから分離した**領域ごとの細則**
  （置き場所の一覧・MCP ブリッジ・実機テスト・自動アップデート・CI の待ち方と
  デバッグ）も置いています。

ソースのコメントや PR にある `docs/DEVELOPMENT.md「<節の名前>」` は、下の一覧の同じ名前の
ページで参照できます。

## コードの構成

| ページ | 中身 |
| --- | --- |
| [ソースの構成](development/source-layout.md) | 2 フェーズ分離と殻・本体の分割、ディレクトリとファイルの一覧 |
| [置き場所の一覧（重複を作らない）](development/placement-index.md) | 共有する定数・述語・ヘルパーの唯一の置き場所（`core/` / `parse/` / `draw/` / 殻 / `tests/`・`scripts/`）。**新しく共有するものを作ったらここへ 1 行追加する** |
| [プラグイン識別子](development/identifiers.md) | 表示名・バンドル名・ユニバーサル名・UUID と、その在り処 |
| [SDK ドキュメント（API 仕様）](development/sdk-docs.md) | SDK リファレンス（`Findings/`）と公式リファレンスの在り処 |
| [文書とコメントの書き方](development/writing-style.md) | コメントの並べ方（目的 → 注意事項 → 理由）・用語の対応表・言い換えない用語 |

## ビルドとテスト

| ページ | 中身 |
| --- | --- |
| [ローカルでのビルド](development/build.md) | `VW_SDK_DIR`・安定版と開発版（`VW_BUILD_CHANNEL` / `VW_DEV_BUILD`）・macOS・Windows（x64 だけの理由） |
| [テストの実行（テストとカバレッジ）](development/testing/running.md) | 無 SDK のテストの回し方・サニタイザ・ビルドオプション |
| [`tests/README.md`](../tests/README.md) | テストの一覧・方針・何をテストしていないか（領域ごとのページは [`development/testing/`](development/testing/)） |

## CI とリリース

| ページ | 中身 |
| --- | --- |
| [継続的インテグレーション（CI）](development/ci/README.md) | `build.yml` のジョブとリリース（stable / dev）・dev プレリリースの片付け・stable の取りこぼし検知・手動ディスパッチを持つワークフロー |
| [カバレッジレポート](development/ci/coverage.md) | `test` / `coverage` ジョブ・差分カバレッジ・しきい値 |
| [自動レビュー（`pr-review.yml`）](development/ci/pr-review.md) | 起動の入口・門・権限・費用の上限 |
| [CI の完了待ち（`scripts/ci-wait.sh`）](development/ci/ci-wait.md) | 待ち方・`conclusion` の一覧・CI が始まらないときに疑う順序 |
| [CI デバッグ（`ci-debug.yml`）](development/ci/ci-debug.md) | 起動と待機・モード・結果の読み方 |

## Lint

| ページ | 中身 |
| --- | --- |
| [コーディング規則の強制（Lint）](development/lint/README.md) | `lint.yml` のジョブ・採用しているルール・ローカルでの実行（`scripts/lint.sh`） |
| [SDK 依存コードの静的解析](development/lint/clang-tidy-sdk.md) | `tidy-mac` / `tidy-windows`・速くする工夫・リリースのゲート・clang の版 |
| [clang-tidy の結果キャッシュ（`-c`）](development/lint/clang-tidy-cache.md) | 鍵に入るもの・安全側への倒し方・壊れ方の見分け方 |

## MCP ブリッジと実機テスト

| ページ | 中身 |
| --- | --- |
| [MCP ブリッジ](development/mcp-bridge.md) | ツールの一覧と、変えるときの決めごと |
| [実機テスト](development/live-test/README.md) | 概要と境界（どこに何があるか） |
| [ローカルセッションの準備（GitHub・iOS・許可）](development/live-test/local-session-setup.md) | ローカルとクラウドの分担・`gh` の認証・Remote Control・`.claude/settings.json` |
| [ローカルセッションでの回し方](development/live-test/running.md) | 1 周の流れ（push → `ci-wait` → `vw_update` → `vw_run_test`） |
| [報告の読み方（Claude 向け）](development/live-test/reading-reports.md) | 「図面の状態:」から読む・人に頼んでよいこと |
| [図面の用意（M39）](development/live-test/drawing-preparation.md) | テンプレートから開いた新しい図面へ描く |
| [周をまたいで持ち越すもの（`core/FeedbackSession`）](development/live-test/session-state.md) | 自分で保存した図面の記録と置き場所（条件は持ち越さない。M43） |
| [一時ファイルと片付け（`core/FeedbackScratch`）](development/live-test/scratch-files.md) | ブランチごとの置き場と、削除してよい条件 |
| [設計の決めごと（実機テストを変えるときに守ること）](development/live-test/design-rules.md) | 入口と分担・尋ねる／伝える・図面を閉じる安全弁 |

## 自動アップデート

| ページ | 中身 |
| --- | --- |
| [自動アップデートの仕組み](development/auto-update/README.md) | 同梱スクリプト・GitHub のトークン・殻の ID に入れないこと・非対話モード |
| [配置の仕組み（インストーラ）](development/auto-update/installer.md) | 探すのは同梱スクリプト、置くのはリリース側のインストーラ／プラグインは自分のフォルダを 1 つ持つ／OS ごとの事情 |
| [入れる前に、前の版をその版自身のアンインストーラで取り除く](development/auto-update/uninstaller.md) | アンインストーラの規則と安全弁 |
| [いつ確認するか — コマンドの入口で（起動時ではない）](development/auto-update/when-to-check.md) | 入口ごとの `UpdateCheckKind`・インストールの経路は 1 本 |
| [チャンネルごとの挙動](development/auto-update/channels.md) | stable / dev・「いま」の判定・ブランチの照合 |
| [インストール後 — まず「再起動が要るか」を決める](development/auto-update/after-install.md) | 殻の ID（`VW_SHELL_ID`）と本体の再読み込み |
| [殻まで変わったときの再起動](development/auto-update/restart.md) | `CloseAllFilesAndQuitVectorworks`・以前は SDK に依頼できなかった経緯 |
| [スクリプトを直接実行する（手動 CLI）](development/auto-update/manual-cli.md) | `vw-install` / `vw-uninstall` / `vw-update` の手動実行 |
