# 残っている宿題

- **配筋（保留）**。レコード `配筋` を立上り・底盤に設定する（`Pset_Reinforcement`、鋼種接頭辞
  除去、短辺/長辺→X/Y）。着手するときは**統合キーにも配筋を追加する**
  （`parse/Footing.cpp` の `wallSectionKey` / `slabMergeKey`）——配筋の違う部材を 1 つに統合
  すると片方の配筋が失われる。現状は配筋を持たないため、**配筋だけが違う底盤は 1 枚に
  統合されている**。
- **床付け（M17）の実機確認は「スラブ厚・梁せいが一定」の範囲まで。** 実機で確かめられたのは
  底盤の厚みも地中梁のせいも揃っているモデルで、**それらが場所によって違うモデルは未確認**。
  影響するのは切り下げの 2 か所で、どちらも**保守側（多めに切る）**になる:
  取り合う地中梁のほうが深いと切り下げ高さが負になり、その区間の捨てコン——場合によっては
  砕石も——がまるごと欠落する（相手のコンクリートが占める前提。実データではスキップフロアの
  外周 × 内部で起きている）。底盤の厚みが場所で違うと、帯を切り上げる高さ（底盤の砕石の底）が
  地中梁ごとにずれる。**床付けが欠けて見えるようなら、まずこの 2 つを疑う**
  （`parse/Footing.cpp` の `beddingSpans`）。
- **M9（立上り・底盤・基礎ストーリ）の実機確認は M9 固有の項目だけ残っている。** M10 / M11 の
  確認で立上り・底盤・基礎ストーリも一緒に見えているため、残りは壁厚の保持・統合本数・
  布基礎（升目状ラティス）が統合されないこと、といった細目。
- **床版の開口は塞がっている。** VW スラブへ穴を開ける API の調査が要る。
- **登り梁の実データ検証が薄い。** 検証済みフィクスチャに登り梁を含むモデルが 1 件も無く
  （`tests/fixtures/README.md`）、合成モデル・合成命令でのみ検証されている。
- **進捗バーの重みは 1 サンプルの粗い実測。** 別のモデルで大きくずれる要素があれば
  `core::drawWeight` の表を直す（診断ログの経過ミリ秒と比較する）。**ただし 1 周の数字で
  直さないこと**——同じモデル・同じコードの 2 周で構成層が 81 倍ばらついた実績がある
  （[「描画の高速化」の「1 周の数字で語ってはいけないものがある」](draw-performance.md#1-周の数字で語ってはいけないものがある)）。
- **draw/ 側の逐語コピーは一部を M19.5 の第 2 弾で整理済み**（`StoryBoundData`・バウンド ID・
  `SetObjectVariable` ラッパー・`AppendLine`・`PushUnique`・`kFitTol` → いずれも
  `draw/DrawUtil`）。**残っている候補**: 伏図と軸組図のフェーズ骨格・診断文言（`draw/Sheet` と
  `draw/Section` でほぼ同文。Sheet は 2 巡構造なので「1 枚ぶんの仕上げ」だけを切り出すこと）、
  スラブ描画手順（`draw/Floor` と `draw/Footing` の生成〜Reset。`ConvertToUnstyledSlab` より
  **前**に削り取りを渡す順序を崩さないこと）、要素ごとの描画ループ（進捗・レイヤ活性化・
  ハンドル記録が 12 か所で同型。ただしレイヤ欠落を数えるかは要素で違う＝挙動判断が要る）、
  `SetClassByName` と `SetAllAttributesByClass` の**対の適用漏れ**（フォールバック描画 4 か所は
  片方しか呼んでおらず、揃えると見た目が変わりうる＝実機確認必須）。いずれも実描画に触れる
  ため、**着手する PR は必ず実機確認を挟む**。`kCosts`（`core/Progress.cpp`）と `kElements`
  （`parse/Summary.cpp`）の命令数ラムダ 19 行の二重管理も未統合（表の統合は無 SDK で完結するが、
  `DrawPhase` の並びと手で揃える規約を壊さないこと）。
- **（済）伏図が 1 枚、用紙に収まらなかった**（実機 round 1・PR #125）。原因は凡例でも
  `planContentBounds` でもなく、**屋根面の勾配基準線が 1 スパンぶん飛び出していた**こと
  （`parse/Roof` の軒の軸）で、PR #128 で直した（[M29「はみ出しの正体は屋根面の勾配基準線
  だった」](milestones/m29-fit-overflow-jitter.md#はみ出しの正体は屋根面の勾配基準線だった見積もりの単位ではなかった)）。
  **残っているのは見積もりの余白の単位**——`core::kPlanContentMargin`（300mm）はモデル mm の
  定数で、縮尺に追随しない記号・タグのぶんは縮尺が上がるほど足りなくなる。直すには記号と
  タグが用紙で占める幅の実測が要る（[M29「見積もりの単位が合っていない」](milestones/m29-fit-overflow-jitter.md#見積もりの単位が合っていない構造上の限界数字が出てから直す)）。
- **完了報告をモーダルのままにするか**は実使用の感触待ち。
- **MCP ブリッジ（M24 / M30）の実機確認で残っているもの。** 繋がって Claude から図面を
  読めること（[M24「探し方は実機で外した」](milestones/m24-mcp-bridge.md#探し方は実機で外したtmpdir-が来ない)の修正のあと・M30 の 2 周目）と、
  時計の中からの実機テストの 1 周・更新・再起動（[M38「実機で分かったこと」](milestones/m38-local-mcp-verification.md#実機で分かったことpr-188-の確認)）は確かめた。
  残るのは [M30「まだ確かめていないこと（実機）」](milestones/m30-mcp-resident.md#まだ確かめていないこと実機)の 2 つと、種別番号の読み替え表が実物と合っているか。**種別番号 → 名前の一覧**が欲しくなったら、SDK
  リファレンス側へ調査を依頼する（本リポジトリでは調べない）。
- **macOS の探し方は CI では試されない**（`McpServerTests` を回すのは Linux のランナー
  だけで、`darwin_user_temp_dir` の枝はそこでは走らない）。実機で `vw_bridge_status` の
  「探した場所」に `/var/folders/…/T/…-mcp` が並ぶことを目視で確認すること。
- **（M38 の運用で決着）ブリッジはクラウドのセッションからは使えない。** 受け渡しが
  ローカルのファイルなので、繋がるのは**同じ計算機で動いている Claude** だけ。claude.ai/code
  のリモートセッション（＝M37 までこの開発を回していた側）へ図面の中身を渡すには、結局その場の
  Claude に読ませた結果を人が貼ることになる。**往復が不要になるのは「画面を読んで文章にする」
  ぶんだけ**で、そこは読み違いが入らなくなる分だけ確かに良くなっている、という位置づけ。
  **M38 の運用での答え**: 実機確認の周は Mac で `claude remote-control` から起動した
  ローカルのセッションが回し、人は iOS からそれを操作する。ローカルには GitHub MCP も PR の
  購読も無いので、GitHub は `gh` の認証でアクセスし（`scripts/ci-common.sh` が `gh auth token` を
  取得する）、レビューは周の区切りに確認する。橋をクラウドへ開く案は採らない（隠れていても
  受け付ける作りの前提が「同じ計算機の同じ利用者」なので）。手順は開発ガイドの
  [「ローカルセッションの準備」](../development/live-test/local-session-setup.md)。
- **MCP ブリッジで図面を直接「書く」道具はまだ無い**（図面を書くのは、本番の取り込みと
  同じ経路で 1 周を走らせる `vw_run_test` だけ。M38）。追加するときは undo の作法を通すこと
  （[M24「v1 は読むだけ」](milestones/m24-mcp-bridge.md#v1-は読むだけ)）。
- **（済）実機フィードバックの往復に残る手仕事は「取り消し」だけ（M24）。** 前の周の図の
  戻し方は M25 で取り消し・作業ファイル・レイヤ削除の 3 段にし、M39 で**毎周テンプレートから
  開いた新しい図面へ描画する**形に置き換えた（[M39](milestones/m39-test-from-template.md)）。
  残るのは**描画結果そのものを送る**（ビューポートを画像に書き出して添える）ことで、できれば所見の
  負担がさらに減るが、書き出し API の調査が先。
- **（M38 で対象が無くなった）モードレスのパレット（M24）で残る未確認は「自動で次の周が
  走るか」だけ。** 往復のパレットは M38 で削除した。開くこと・JS のタイマーが届くこと・自動で
  表示されること・ボタンの有効・無効の切り替えは実機 round 1 で確かめた
  （[M24「実機で確かめられたこと（round 1）」](milestones/m24-feedback-palette.md#実機で確かめられたことround-1)）。**確かめた結果を SDK リファレンスの Findings
  「モードレス（非モーダル）なパレット」へ追加し、【実機未確認】の注記を削除すること**——
  本リポジトリの開発メモに書いても、SDK の知見は SDK リファレンスが唯一の置き場所である。
- **端部オフセットのパラメータ名は実機で確定させる（M20）。** PIO のパラメータ名は SDK
  ヘッダのどこにも無い（`ci-debug` の `sdk-grep` で `AxisAlign|StartCondition|MajorBreadth`
  を引いても 1 件も出ない）ので、候補（`StartOffset` / `OffsetStart` / `StartExtension` /
  `StartCutOffset` とローカライズ名「始端オフセット」）を並べて `ResolveParamNameAmong` で
  引いている。実データでは横架材の 7 割・柱のほぼ全数にオフセットが入るので、**候補が誤っていれば
  診断ログに「端部オフセットを設定できなかった材 N 本（候補: …）」が必ず出る**
  （`DescribeParamsContaining` がその PIO の似た名前を並べる。「候補: …」は開発ビルドだけ）。
  出た名前を `draw/StructuralMember.cpp` の候補表へ入れて候補を絞る。
- **図面枠 PIO の登録名を SDK リファレンスへ送る（M28）。** 実機 round 1 で
  `"Title Block Border"` と確定し（VW 2026 / macOS）、`draw/TitleBlock.cpp` の候補は
  絞った。**まだ SDK リファレンスの `Findings/` には無い**ので、型番号 552 ↔ 登録名の対応として
  送ること（`Findings/Symbols.md` か `Findings/Parametric Objects.md`）。ついでに
  「PIO の型番号から登録名を引く呼び出しがあるか」も、同じ問題に直面する次の要素のために
  issue にしておくとよい。
- **図面枠のぶんを用紙から差し引く（M28）。** 軸組図は M35 で**枠の外形の内側**へ並べ、
  上へ寄せて余りを下へ回すようにした（枠線の無い表題欄だけのスタイルなら、その帯のぶん
  下を空ける。`core::reserveTitleStrip`）。**伏図はまだ**印刷可能領域のままで、枠線のある
  図面枠の中で**表題欄が占めるぶん**は軸組図でも差し引いていない（表題欄が枠のどこを
  占めるかは外形からは求まらないので、そこをどう測るかが先に要る）。
- **登り梁の端部はまだ「受け材の面まで詰める」まま**（`parse/Noboribari`）。傾斜梁は水平面内の
  矩形モデルが成り立たないので M20 の対象から除外した。フレーム解析モデルを組み立てるときに
  ここも接合点へそろえる必要が出る。
- **接合点へ送れていない端がまだ残る**（実データで全端の 1〜2 割）。軒の出・継ぎ手のような
  本当の自由端が大半だが、**柱の断面から 100mm 以上外れた端**（実測の最小はみ出しは 80〜145mm）
  が各モデルに十数件ある。取り付く相手を柱・横架材のどちらとも判定できていないので、必要になったら
  「何にも取り付いていない端」を診断へ出して実物と照合する。
- **殻のファイルのコメントに古い記述が残っている（次に殻を変更する PR で直す）。** 直すと殻の ID
  （`VW_SHELL_ID`）が変わり、利用者に再起動を求めるので、殻を直す用事のある PR に含める
  （PR #198 の点検で見つけたもの）:
  - `CMakeLists.txt` … 「殻の sources」の「メニュー 2 つと PIO 2 つ」→ メニュー 4（うち dev だけ 2）・
    パレット 1・PIO 2。冒頭の README「プラグイン識別子」→ `docs/development/identifiers.md`。
    「the plug-in links it」「draw/ is compiled into the plug-in target」→ 本体（payload）。
  - `src/ModuleMain.cpp` … README「SDK ドキュメント」→ `docs/development/sdk-docs.md`。
    「確認の入口は 2 つ」→ 4 つ（取り込み・アップデータ・実機テスト・MCP の `vw_update`）。
  - `src/Updater.h` … 「入口は次の 2 つだけ」→ 4 つ。節名の無い `docs/DEVELOPMENT.md` →
    `docs/development/auto-update/restart.md`（`src/Updater.cpp` も同じ）。
  - `src/UpdaterHost.h` … 副作用の列挙に `DropLoadedPayload` が抜けている（実際は 6 つ）。
    `Silent` の説明が取り込みのついでだけ。
  - `src/UpdaterParse.h` … 「Updater.cpp」→ `UpdaterFlow.cpp`（2 か所）。`ResolveDevSelection` の
    説明が `FindDevBuildForBranch` の上に付いている。`FindDevBuildForBranch` の用途はいま
    `Silent` の確認と `RemoteDevUpdateWith`。
  - `src/PayloadHost.cpp` … 冒頭の「印は std::filesystem で分岐しない」と、`StampOf`（std::filesystem を
    使わない）の説明が食い違う。
  - `src/PayloadAbi.h` … 殻に残るものの列挙に、パレットの登録と MCP から頼まれる更新・再起動が無い。
  - `src/Extensions/ExtMenu.cpp` … 「入れられなかった・殻まで変わった、のどちらかで取り込みへ
    進まない」→ 進まないのは入れられなかったときだけ（**実装が正**。`src/UpdaterHost.h` の戻り値）。
  - `src/Extensions/ExtMcpPalette.cpp` … M38 で削除された `ExtFeedbackPalette.cpp` への参照。
  - `src/Extensions/ExtTestMenu.h` … 「図面の戻し」「取り込み前へ戻してから」→ テンプレートから
    開いた新しい図面へ描画する（M39）。
  - `src/Extensions/ExtMcpMenu.h` … メニュー名（dev は「MCP ブリッジを表示… (Dev)」）。参照先の
    CLAUDE.md「ビルド・リント・リリース」→「命名」と `docs/development/identifiers.md`。
  - `src/Extensions/ExtShearWall.h` … 「dev ビルド（と HOMESKZ_IFC_TRACE 指定時）は診断ログへ書く」→
    取り込みの最中に書く。`draw/ShearWall` の `applyShearWallLayerScale` → `draw/Sheet` の
    `applyPlanLayerScale`。
  - `src/Extensions/ExtColumnMark.cpp` / `ExtShearWall.cpp` … どのページにも無い節 M12「ローカル確認」
    「追随の契機」（parse/ draw/ に倣って番号だけにする）。
