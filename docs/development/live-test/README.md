# 実機テスト（`draw/Feedback`）

（このフォルダのページの一覧は[開発ガイドの目次「MCP ブリッジと実機テスト」](../../DEVELOPMENT.md#mcp-ブリッジと実機テスト)）

> **本番の取り込みコマンドとは別の入口です（M25）。** 実機テストを回すのはメニューの
> **「実機テストを実行…」**（開発版だけ）と MCP の **`vw_run_test`** で、本番の「IFC (ホームズ君)
> 取り込み…」は実機テストを何も知りません。両者が共有するのは**絵を作るところ**
> （`draw/ImportRun` の `runImportRound`）だけなので、**テストで走るのは本番と同じコード**です。

**`draw/` の実描画は CI では検証できず、ローカルの Vectorworks でしか確かめられません**
（[`CLAUDE.md`](../../../CLAUDE.md)「テスト方針」）。そのため 1 往復ごとに

> 新しいビルドを入れる → Vectorworks を再起動する → 図面を戻す → IFC を選ぶ →
> 設定を選ぶ → 取り込む → ログを写して貼る

という手作業が挟まり、**これが実装そのものより時間を食っていました**。M23〜M37 はこれを
「dev ビルドが結果を PR へ投稿し、殻のパレットが新しいビルドを入れて取り込み直す」往復で
回していましたが、**M38 で外しました**（[M38](../../dev-notes/milestones/m38-local-mcp-verification.md)）。いまは**ローカルの
Claude Code が MCP ブリッジ越しに**、更新・再起動・取り込み・報告の読み出しを自分で起こします。
1 周目もリポジトリの IFC とテンプレートを名指しして Claude が始める（M40）ので、人がするのは
「絵を見て一言書く」だけです。

## 境界（どこに何があるか）

| | 役割 |
| --- | --- |
| `src/core/FeedbackSession.*` | 覚えておく値と、その読み書き。**どの周になるかの場合分け**（`feedbackRoundKind`。IFC を名指しされた無人の 1 周目 `AutoFirstRound` も。M40）と報告の置き場所もここ（無 SDK・テストあり） |
| `src/core/FeedbackScratch.*` | 一時ファイルの置き場（ブランチごと）と、PR が閉じたブランチの片付け（無 SDK・テストあり） |
| `src/parse/Feedback.*` | **報告の本文**（Markdown）・前の周との差分・診断ログの切り詰め（`keepTail`）・実機テストの結末の文言（無 SDK・テストあり） |
| `src/draw/Feedback.*` | 実機テストの 1 周（`runTestRound`）——記憶・取り込み前のダイアログ・名指しされたテンプレートを写す（`InstallTemplate`。M40）・図面の用意（テンプレートから開く・自分の図面を閉じる・描き上がりを保存する）・報告の書き出し（SDK 依存） |
| `src/draw/ImportRun.*` | 取り込み 1 周ぶんの部品。**本番の取り込みと実機テストが共有する唯一の実装**（M25）。診断ログの在り処（`importLogPath`）もここ |
| `src/draw/McpBridge.*` | `vw_run_test` / `vw_test_report` / `vw_log` の道具 |
| `src/Extensions/ExtTestMenu.*` | 実機テストの入口の登録と取り次ぎ（殻・**dev だけ登録**。M25） |

**安定版（stable）では動きません**（`draw::feedbackAvailable`）。開発の道具を利用者向けの
配布物に持たせないためです。
