# CI の完了待ち（`scripts/ci-wait.sh`）

PR やブランチの CI（`build.yml` / `lint.yml` / `test.yml` …）が終わるのを待つ道具です。
対象のチェックが全部終わった**瞬間に exit** し、最終行に結果を出します。**完了した瞬間に
exit するプロセスをバックグラウンドで走らせる**と、その終了をハーネスが通知するので、exit が
そのまま完了通知になります。クラウドのセッションで PR を購読していれば、その PR の CI の完了
（check suite の完了。成功も失敗も）は購読でも届きますが、ローカルのセッションには購読が
無く、`ci-debug` の run は PR の CI ではないので、どちらもこのスクリプトで待ちます。

```
Bash(run_in_background: true):
  scripts/ci-wait.sh --pr 34        # PR の head（新しい push が入ったら追随する）
  scripts/ci-wait.sh --ref main     # ブランチ / タグ
  scripts/ci-wait.sh                # いま checkout しているブランチ
  scripts/ci-wait.sh --sha <sha>    # 固定のコミット（追随しない）
```

投げたら別作業を続け、終了通知が来たら出力ファイルを読むだけです。`git push` の直後に
投げてよい（チェックの登録待ちは `--grace` が吸収します）。**`sleep` で待つことと、待機
ループをその場で手書きすること（`while : ; do gh/curl …; sleep 30; done`）は禁止**です——
前者は完了時刻の予測が要り、後者は締切もウォッチドッグも HTTP の時間上限も無いので API が
固まればぶら下がります。どちらも「CI は終わっているのにセッションが気付かない」事故を実際に
2 度起こしています。

出力の最終行は必ず `ci-wait: done (conclusion=<結果> exit=<終了コード>)` で、この行が
無ければ「まだ動いている」か「外から殺された」かのどちらかです。`success` 以外は exit 1:

| conclusion | 意味 |
| --- | --- |
| `success` | 全チェックが成功（skipped / neutral を含む） |
| `failure` | 1 つ以上の結論が `failure` / `timed_out` / `cancelled` / `action_required` / `startup_failure` / `stale`。**cancelled も失敗扱い**（新しい push で古い run が消えたものを green と取り違えないため） |
| `no-checks` | 猶予（既定 180 秒）を過ぎてもチェックが 1 件も登録されなかった。**「CI が始まってすらいない」を成功と読まない**ための結果 |
| `head-moved` | `--no-follow` 指定時に、待っている間に head が動いた（古い結果は返さない） |
| `unknown` | 全チェックが終わったが、結論が上の成功側にも失敗側にも当たらないものがある（GitHub が見慣れない結論を返したときなど）。成功とは読まない |
| `timed-out-waiting` / `api-error` | **CI の失敗ではなく待機側が見届けられなかった**。CI 自体はまだ動いているかもしれない（同じ行に合流用のコマンドが出る） |

状態が変わらなくても 5 分ごとに生存行が stderr に出るので、固まっているのか単に長いのかは
出力で分かります。

**`success` を鵜呑みにしない。** `--pr` は「その sha に登録されているチェック」を見るので、
`ci-debug`（`workflow_dispatch`）の `debug` チェックしか無い状態でも `success` を返します。
並んだチェック名を読み、`build-mac` / `build-windows` / `clang-tidy` / `test` … があることを
確かめてください（**`debug` だけなら本来の CI は走っていない**）。

**CI が始まらない（`no-checks`・PR の Checks が 0 のまま）ときに疑う順序**（どれも「必ず
そうなる」規則ではないので、断定して報告しないこと）:

1. **PR にコンフリクトがある。** コンフリクトを抱えた PR ではチェックが 1 件も登録されない
   ことがある（main を取り込んで解消したら何も操作せずに CI が起動した実測がある）。ただし
   毎回そうなるわけでもない。
2. **PR がまだ無い／その head に PR が向いていない。** 作業ブランチへの push は
   `push: branches: [main]` に当たらないので、PR を作る前のコミットにチェックが付かないのは
   正常。
3. どれでもなければ、GitHub MCP で run（`actions_list` の `list_workflow_runs`）と
   check-run（`pull_request_read` の `get_check_runs`）を直接数えて、登録の有無を確かめる。

待機の土台は `scripts/ci-common.sh` で、`ci-debug.sh` と共有です。**どんな異常でも必ず
有限時間で exit する**ことが唯一にして最大の要件で、HTTP の時間上限・締切判定・ウォッチ
ドッグの三重の歯止めを持ちます（詳細は同ファイルのヘッダ）。この性質は
`tests/ci-wait.test.sh`（ctest の `CiWaitScriptTests`）で回帰テストしています。
**新しく「何かの完了を待つ」道具が要るときは、`poll_until` の上に probe を 1 つ書き**、
待機ループを増やさないでください。
