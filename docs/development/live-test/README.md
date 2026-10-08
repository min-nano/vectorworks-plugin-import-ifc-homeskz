# 実機テスト（`draw/Feedback`）

（このフォルダのページの一覧は[開発ガイドの目次「MCP ブリッジと実機テスト」](../../DEVELOPMENT.md#mcp-ブリッジと実機テスト)）

> **本番の取り込みコマンドとは別の入口です（M25）。** 実機テストを回すのは開発版の MCP の
> **`vw_run_test`** だけで、本番の「IFC (ホームズ君) 取り込み…」は実機テストを何も知りません
> （M42 まであったメニュー「実機テストを実行…」は M43 で削除した。人が手で確かめるなら本番の
> 取り込みで足りる）。両者が共有するのは**描画する部分**
> （`draw/ImportRun` の `runImportRound`）だけなので、**テストで走るのは本番と同じコード**です。

**`draw/` の実描画は CI では検証できず、ローカルの Vectorworks でしか確かめられません**
（[`CLAUDE.md`](../../../CLAUDE.md)「テスト方針」）。そのため 1 往復ごとに

> 新しいビルドを入れる → Vectorworks を再起動する → 図面を戻す → IFC を選ぶ →
> 設定を選ぶ → 取り込む → ログをコピーして貼る

という手作業が挟まり、**これが実装そのものより時間を消費していました**。M23〜M37 はこれを
「dev ビルドが結果を PR へ投稿し、殻のパレットが新しいビルドを入れて取り込み直す」往復で
回していましたが、**M38 で廃止しました**（[M38](../../dev-notes/milestones/m38-local-mcp-verification.md)）。いまは**ローカルの
Claude Code が MCP ブリッジ越しに**、更新・再起動・取り込み・報告の読み出しを自分で起こします。
IFC・テンプレート・設定は毎周 Claude が渡す（M40 / M43）ので、人がするのは
「描画結果を見て所見を書く」だけです。

## 境界（どこに何があるか）

| | 役割 |
| --- | --- |
| `src/core/FeedbackSession.*` | 自分で保存した図面の記録と、その読み書き・閉じてよい図面の判定（`isOwnedTestDocument`）・`vw_run_test` の `settings` の当て方（`applyTestSettings`。M43）・報告の置き場所（無 SDK・テストあり） |
| `src/core/FeedbackScratch.*` | 一時ファイルの置き場（ブランチごと）と、PR が閉じたブランチの片付け（無 SDK・テストあり） |
| `src/parse/Feedback.*` | **報告の本文**（Markdown）・診断ログの切り詰め（`keepTail`）・実機テストの結末の文言（無 SDK・テストあり） |
| `src/draw/Feedback.*` | 実機テストの 1 周（`runTestRound`）——渡された条件の確認・図面の用意（テンプレートから開く・自分の図面を閉じる・描画結果を保存する）・報告の書き出し（SDK 依存） |
| `src/draw/ImportRun.*` | 取り込み 1 周ぶんの部品。**本番の取り込みと実機テストが共有する唯一の実装**（M25）。診断ログの在り処（`importLogPath`）もここ |
| `src/draw/McpBridge.*` | `vw_run_test` / `vw_test_report` / `vw_log` の道具 |

**安定版（stable）では動きません**（`draw::feedbackAvailable`）。開発の道具を利用者向けの
配布物に持たせないためです。
