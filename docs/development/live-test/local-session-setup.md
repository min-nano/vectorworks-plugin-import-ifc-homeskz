# ローカルセッションの準備（GitHub・iOS・許可）

**実機確認を回すセッションは、Vectorworks が動いている Mac で起動した Claude Code だけ**です。
クラウドのセッション（claude.ai/code）は橋のファイルに届かず、逆にローカルの Claude Code には
クラウドで注入される GitHub MCP も PR の購読（`subscribe_pr_activity`）もありません
（PR #188 で両方を踏んだ）。そこで次のように割ります。

| | ローカル（Remote Control） | クラウド |
| --- | --- | --- |
| 担うもの | `draw/` や殻など**実機確認が要る PR の 1 周**（push → CI 待ち → `vw_update` → `vw_run_test`） | `core/` `parse/`・テスト・ドキュメント・CI など**実機の要らない作業** |
| GitHub | `gh` の認証（＋任意で GitHub MCP） | GitHub MCP（注入済み） |
| CI・レビューの知らせ | `scripts/ci-wait.sh` の終了。レビューは周の区切りに読みに行く | PR の購読＋`scripts/ci-wait.sh` |
| iOS から | ✓ | ✓ |

`draw/` に触れる PR をクラウドで始めたら、実機確認の段でブランチをローカルへ pull して
引き継ぎます。**橋をクラウドへ開く（トンネル等）ことはしません**——橋は「相手は同じ計算機の
同じ利用者の Claude だけ」を前提に、隠れていても受け付ける作りです（[MCP ブリッジの「決めごと」](../mcp-bridge.md#決めごと)）。

**一度だけの用意**（Mac）:

1. **GitHub の認証は `gh auth login` だけでよい。** `scripts/ci-common.sh` は
   `GH_TOKEN` / `GITHUB_TOKEN` が無ければ `gh auth token` を使うので、`ci-wait.sh` も
   `ci-debug.sh run`（起動と待機をまとめた形）もそのまま動きます。PR の作成・コメント・
   ドラフトの昇格は `gh pr create` / `gh pr comment` / `gh pr ready` で足ります。
   この文書の手順どおり **GitHub MCP の道具名で呼びたい**なら、ユーザー設定に足します
   （PAT はこのリポジトリに絞った fine-grained で、Contents / Pull requests / Actions /
   Issues を読み書き・Checks を読み取り）:

   ```
   claude mcp add --scope user --transport http github https://api.githubcopilot.com/mcp/ \
     --header "Authorization: Bearer <PAT>"
   ```

2. **iOS から触れるようにする。** リポジトリで `tmux new -s vw` → `claude remote-control`。
   Claude アプリ（iOS）の Code にこのセッションが出ます。tmux で囲むのは端末を閉じても
   セッションを残すため。Mac はスリープさせない（`caffeinate -dims` を別の窓で走らせるか、
   省エネルギーの設定で）。Vectorworks は開いたまま、MCP ブリッジのパレットも出したままに
   します。
3. **許可の確認を絞る。** iOS で毎回承認しなくて済むよう、`.claude/settings.json`
   （無ければ作る。個人だけなら `.claude/settings.local.json`）へ次を置きます。
   **`vw_restart` は確認を残します**——CLAUDE.md の「頼む前に人へ一言断る」を、iOS の承認の
   画面がそのまま担います。`vw_call` は `vw_restart` も呼べるので同じく確認を残します。

   ```json
   {
     "enabledMcpjsonServers": ["vectorworks"],
     "permissions": {
       "allow": [
         "mcp__vectorworks__vw_bridge_status",
         "mcp__vectorworks__vw_launch",
         "mcp__vectorworks__vw_ping",
         "mcp__vectorworks__vw_layers",
         "mcp__vectorworks__vw_classes",
         "mcp__vectorworks__vw_layer_objects",
         "mcp__vectorworks__vw_object_counts",
         "mcp__vectorworks__vw_log",
         "mcp__vectorworks__vw_test_report",
         "mcp__vectorworks__vw_run_test",
         "mcp__vectorworks__vw_update",
         "Bash(scripts/ci-wait.sh:*)",
         "Bash(scripts/ci-debug.sh:*)",
         "Bash(scripts/lint.sh:*)",
         "Bash(git status:*)", "Bash(git diff:*)", "Bash(git log:*)",
         "Bash(git fetch:*)", "Bash(git pull:*)", "Bash(git add:*)",
         "Bash(git commit:*)", "Bash(git push -u origin:*)",
         "Bash(gh pr view:*)", "Bash(gh pr checks:*)", "Bash(gh pr diff:*)",
         "Bash(gh run view:*)", "Bash(gh run list:*)"
       ],
       "ask": ["mcp__vectorworks__vw_restart", "mcp__vectorworks__vw_call"],
       "deny": [
         "Bash(git push --force:*)", "Bash(git push -f:*)",
         "Bash(git push * --force*)", "Bash(git push * -f*)", "Bash(git push *+*)"
       ]
     }
   }
   ```

   `deny` の後ろ 3 つは `allow` の `git push -u origin:*` の抜け道を塞ぐもの。末尾の `:*` は
   「後ろに何が続いてもよい」なので、`git push -u origin <branch> --force`・`… -f`・
   `git push -u origin +<branch>`（先頭の `+` は強制更新）は `deny` の頭の 2 つに当たらず、
   `allow` だけに当たって確認なしで通ってしまう。後ろ 3 つは `*` を途中に置いて間を飛ばす。
   `*` はどこに置いても空白込みの任意の文字列に当たり（`:*` の書き方は末尾でしか効かない）、
   規則は `deny` → `ask` → `allow` の順に見られて `deny` が必ず勝つ
   （[Claude Code の Permissions](https://code.claude.com/docs/en/permissions)）。それでも
   文字列の照合なので万全ではない（`main` への force push は GitHub のブランチ保護で禁じて
   おく）。

**ローカルでの PR の見方。** 購読が無いので、CI は `scripts/ci-wait.sh` をバックグラウンドで
投げてその終了で知り（クラウドと同じ）、レビューとコメントは**周の区切りごとに**
`gh pr view <番号> --comments` で読みに行きます。放っておく時間が長いときは `/loop` で
間隔を決めて見に行かせます。
