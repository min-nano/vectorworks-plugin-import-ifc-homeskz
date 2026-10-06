# 覚えているもの（`core/FeedbackSession`）

1 周目の選択（IFC のパス・取り込み設定）・テンプレートのパス・自分で保存した図面のパス・
前の周の内訳は `key=value` のテキストで残ります。**ここが 2 周目以降からファイル選択と設定ダイアログを
消している唯一の仕掛け**で、MCP の `vw_run_test` が走れるのもこれがあるからです。報告は同じ
フォルダの `last-round.md` です。

- macOS … `~/Library/Application Support/HomeskzIfcImport/feedback.txt`
- Windows … `%LOCALAPPDATA%\HomeskzIfcImport\feedback.txt`

（フォルダ名はプラグインの改名に追随させていません。**識別子なので付け替えると記憶が
行方不明になる**ためです。）手で消せば次の実機テストは 1 周目から始まります。
`HOMESKZ_IFC_FEEDBACK_STATE` に別のパスを指定して差し替えられます。M37 までの記憶にある
PR の行（`send` / `repo` / `pr` / `branch` / `anon` / `posted` / `loop`）は黙って読み飛ばし、
書き直したときに消えます。M38 までの作業ファイル（`work`）と作ったレイヤ（`created.layer` /
`created.sheet`）の行も同じく読み飛ばします——古い記憶にはテンプレートが無いので、MCP の周は
走らず、①をもう一度人に頼みます。

**伏せません。** M37 までは PR コメントが公開されるのでファイル名・ユーザー名・図面枠の
スタイル名を伏せていましたが、報告は利用者の計算機の中だけで読まれます。
