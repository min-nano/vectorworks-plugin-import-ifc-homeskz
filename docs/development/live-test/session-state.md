# 周をまたいで持ち越すもの（`core/FeedbackSession`）

実機テストが周をまたいで持ち越すのは、**実機テストが自分で保存した図面のパス**
（各周の描画結果）だけです。次の周の頭で、そのうち
開いているものを保存せずに閉じます（[図面の用意](drawing-preparation.md)）。閉じてよいのは
ここに名前で記録され、一時ファイルの置き場の中にある図面だけです
（[設計の決めごと](design-rules.md)「図面を閉じる」）。本体は周の間に入れ替わる
（`vw_update`）ので、メモリではなく `key=value` のテキストに置きます。報告は同じフォルダの
`last-round.md` です。

- macOS … `~/Library/Application Support/HomeskzIfcImport/feedback.txt`
- Windows … `%LOCALAPPDATA%\HomeskzIfcImport\feedback.txt`

（フォルダ名はプラグインの改名に追随させていません。**識別子なので付け替えると記録が
行方不明になる**ためです。）`HOMESKZ_IFC_FEEDBACK_STATE` に別のパスを指定して差し替えられます。

**条件は持ち越しません（M43）。** どの IFC を・どのテンプレートから・どの設定で取り込むかは、
MCP の `vw_run_test` に Claude が毎周渡します。M42 までは
1 周目の条件・前の周の内訳とビルド・1 周目のレイヤ構成（基準）もここに記憶し、2 周目以降を
名指し無しで走らせていましたが、周を起こすローカルの Claude Code が条件も前の周の報告も
持っているので要らなくなりました（[M43](../../dev-notes/milestones/m43-no-remembered-conditions.md)）。
M42 までの記憶にある条件の行（`ifc` / `round` / `build` / `tally` / `template` / `baseline` /
`role.*` など）と、さらに古い PR の行・作業ファイルの行は黙って読み飛ばし、書き直したときに
消えます。

**伏せません。** 報告は利用者の計算機の中だけで読まれます。
