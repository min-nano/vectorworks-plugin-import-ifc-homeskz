# 設計の決めごと（実機テストを変えるときに守ること）

実機テストまわり（`draw/Feedback`・`core/FeedbackSession`・`parse/Feedback`）を
変えるときの決めごとです。**どれも実機で一度壊れて決まったもの**なので、緩める前に
[M23](../../dev-notes/milestones/m23-feedback-loop/README.md)・[M24](../../dev-notes/milestones/m24-feedback-palette.md)・[M25](../../dev-notes/milestones/m25-test-separate-from-import/README.md)・[M38](../../dev-notes/milestones/m38-local-mcp-verification.md)・[M39](../../dev-notes/milestones/m39-test-from-template.md)・[M43](../../dev-notes/milestones/m43-no-remembered-conditions.md) を読んでください。

**入口と分担**

- **本番の取り込みコマンドに実機テストを書かない（M25）。** 実機テスト（図面を準備
  する・報告する）を書いてよいのは `draw/Feedback` の `runTestRound` だけで、`draw/ImportCommand` と `Extensions/ExtMenu` には 1 行も書きません。
  **`#ifdef VW_DEV_BUILD` で囲っても制御フローは本番の入口に残る**ので、囲えばよいとも
  考えません。共有してよいのは `draw/ImportRun` の `runImportRound` だけです。
- **本体は待たない・入れない。** `draw/Feedback` にビルドを待つコードを書かない（モーダルの
  ダイアログが図面を塞ぐ。M23 の実機 round 3）。インストールも書かない（経路は
  `src/UpdaterFlow.cpp` の 1 本。入れ替えは MCP の `vw_update` が殻に頼む）。
- **勝手に回る時計を持たない（M38）。** 取り込みが起きるのは人が本番の取り込みを押したときと
  Claude が `vw_run_test` を頼んだときだけです。M24 の往復で、パレットの JS タイマーが誰も
  押していないのに取り込みを走らせた事故を繰り返さないでください。
- **PR へ投稿しない（M38）。** 報告は手元のファイルに置き、MCP で読みます。図面の情報が
  外へ出る経路をプラグインに持たせ直さないでください。

**尋ねる・伝える**

- **実機テストの周にダイアログを 1 枚も出さない。** 誰も見ていない Vectorworks が止まります。
  条件は毎周渡してもらい（`ifc` / `template` / `settings`。M43）、設定はダイアログの初期値と
  同じものを図面から組んで `settings` で上書きします（M40）。渡されたものが足りない・使えない
  なら、何も変えずに理由を返します（`InvalidRequest`）。
- **人の操作を要する入口を足さない（M43）。** M42 まではメニュー「実機テストを実行…」が
  同じ周を人の手で起こしていたが、人が手で確かめるなら本番の取り込みで足りる（診断ログは
  `vw_log` で読める）ので削除した。
- **実機テストの結末は実機テスト自身の言葉で言う。** 取り込みコマンドの完了文言
  （`parse::formatImportResult` / `formatImportError`）を借りず、`parse::formatTestRoundResult`
  を使います。
- **取り込みの最中に SDK に訊かせない。** Vectorworks 自身がモーダルを出す余地は渡す値の側で
  取り除きます（例: 図番が重なると「新しい図番を割り当てますか？」が出るので、軸組図の図番は
  `parse::uniqueSectionNumbers` が一意にする）。
- **条件を周をまたいで持ち越さない（M43）。** 持ち越してよいのは自分で保存した図面の記録
  （`ownedDocuments`）だけです。IFC・テンプレート・設定・前の周の内訳を記憶し直すと、頼んだ
  条件と記憶の条件が食い違う余地が戻ります。前の周との比較は報告を読む側（Claude）が行います。
- **渡された `template` からしか始めない。** 人の居ない周に、いま開いている図面を基準に
  採らせません（前の周の描画結果や利用者の図面が基準になりうる。M25 の実機 round 13・M39）。

**図面を閉じる（利用者のものを消すコード）**

`CloseDocument()` は**確認なしに未保存の変更を捨てます**（Findings「Documents」）。ここは
アンインストーラと並ぶ「利用者のものを消す」コードなので、**歯止めを緩める方向へ変えません**。

- **閉じるのは周の頭と、実機テストを終えるとき（M42。MCP の占有を解くときの
  `vw_test_cleanup`）だけ。** どちらも同じ `CloseOwnedDocuments` を通ります。
- **閉じてよいのは、実機テストが自分で保存した図面だけ。** 判定は `core/FeedbackSession` の
  `isOwnedTestDocument` 1 か所で、記録（`ownedDocuments`）に**名指しで在り**、そのパスが
  **一時ファイルの置き場の中**にあるものに限ります（壊れた記録・書き換えた記録から利用者の
  図面へ届かせない）。回帰テストは `tests/CoreFeedbackSessionTests.cpp`。
- **切り替えたあとにもう一度確かめてから閉じる。** `SwitchToOpenFile` のあとアクティブな図面を
  読み戻し、それが閉じる相手でなければ閉じません（別の図面がアクティブなまま `CloseDocument`
  を呼ぶと、利用者の変更を捨てる）。
- **`fFileRef` を記録に持ち越さない。** 再起動後に同じ番号が別の図面に割り当たり、利用者の
  図面を閉じる恐れがあります。図面はパスで見分け、`fFileRef` はその場で `GetOpenFilesList`
  から引き、1 つ閉じるたびに引き直します。
- **閉じたか・開けたかは戻り値ではなく読み戻しで確認する**（`CloseDocument` は閉じても `false` を
  返す。実機 round 9）。開けなかったら黙って続けず、何が起きたかを「準備:」の 1 行に
  **証拠つきで**載せて周ごと中止します（描画先が無いまま描画すると全要素 0 件になる。描画先が
  利用者の図面ならそこを汚す）。
- **保存先はいつもまだ無いパス**（`FreshTempPath`。既存のファイルへの別名保存が失敗した実測が
  ある）。
