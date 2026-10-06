# 置き場所の一覧（重複を作らない）

同じ定数・述語・ヘルパーを 2 か所に書かないための、**既存の唯一の置き場所**の一覧です
（原則は [`CLAUDE.md`](../../CLAUDE.md)「重複を作らない置き場所」）。新しく共有するものを
作ったら、ここに 1 行足してください。

**要素を 1 つ足すときの型**: `parse/<要素>.{h,cpp}`（解析）＋ `core/Document.h` の命令構造体と
`validateDocument` の検証＋ `draw/<要素>.{h,cpp}`（描画）＋ `tests/Parse<要素>Tests.cpp`
（無 SDK テスト）＋ `parse/Summary.cpp` の `kElements` に 1 行。PIO を足すときは
`Extensions/Ext<要素>.{h,cpp}`（殻: 登録と取り次ぎ）＋ `draw/<要素>Pio.{h,cpp}`（本体: 作図）に
割ります。

**`core/`**

| もの | 置き場所 |
| --- | --- |
| 平面座標の同一判定と許容（`samePoint` / `kPointEps`）・Vec2 の基本演算（`dot` / `cross` / `length` / `distance`）・同一直線上の線分成分の芯線射影（`collinearSpan`）・凸多角形の凸多角形クリップ（`clipPolygonToConvex`。唯一の利用者は耐力壁の筋かいの形 `core::shearWallBracePolygon`）・その 1 段である半平面クリップ（`clipPolygonToHalfPlane`。ほかの利用者はたすき掛けの奥の筋かいの切り分け `core::shearWallBehindBracePieces`） | `core/Geometry.h` |
| ペア述語による連結成分（Union-Find。立上り・大引・地中梁の統合と壁結合の交点クラスタ） | `core/UnionFind.h` |
| ストーリレベルの種別名（`kLevelFL` / `kLevelBeamTop` / `kLevelGL` / `kLevelShearWall` 等。デザインレイヤ名の接尾辞にもなる。parse 側は要素ごとの名前で再公開するだけ） | `core/Document.h` |
| 構成層の総厚（`totalThickness`）・横架材の Z 範囲と重なり（`memberTopZ` / `memberBottomZ` / `zRangesOverlap`。許容値は呼び出し側）・端部オフセットの意味と値・オフセットを戻した「材の端」（`memberDrawnStart` / `memberDrawnEnd` / `columnDrawnTop` / `columnDrawnBottom`）・始端を低い端にした横架材（`memberLowEndFirst`） | `core/Document.h` |
| 描画側から切り離せる純計算（レイヤの希望スタック順 `desiredStoryLayerOrder`・地中梁の可視ソリッドの呑み込み `raiseModifierTop`・地中梁の押し出しの基面 `modifierBasePolygon`・図に映るものの広がり `planContentBounds` / `sectionContentSize`） | `core/Document` |
| 断面の注釈空間への投影（`sectionAlongOrigin` / `sectionAnnotationPoint`。断面寸法タグ＝`parse/Tag`・寸法＝`parse/Dimension`・図面ラベル＝`draw/DrawingLabel` が共有）・軸組図の図面ラベルを寄せる点と下げる量（`sectionLabelAnchor` / `sectionLabelDrop`。寸法が無いときの間隔 `kSectionLabelGap`） | `core/Document` |
| 用紙の割り付け（`core::planLayout` ほか。縮尺の階梯と選び方・伏図の縮尺と位置——**縮尺は実測した凡例の幅を引いてから決め**、凡例の置き場所は `legendTopRight`——・軸組図の上下 2 段とシートの分割・タイトルの連番・軸組図の辺ごとの帯 `SectionBands` と図を合わせる点 `sectionViewportCenter`・図面枠の内側へ絞る `insetFrameArea` / `kTitleBlockInset`・枠か表題欄の帯かの判定 `frameCoversPaper` と帯のぶん下を空ける `reserveTitleStrip`・通り芯の符号の帯 `kSectionGridBubbleAllowance`・図面ラベルの帯の見込み `kSectionLabelAllowance`） | `core/Layout` |
| 軸組図の辺ごとの注釈の帯を命令から数える（`sectionBands`） | `core/Document` |
| 寸法線の位置（用紙 mm の段の間隔 `kDimensionFirstGap` / `kDimensionTierPitch` と縮尺を掛けた位置 `dimensionLineCoord`）・寸法の帯（`dimensionBand`。最も外の段は `core/Document` の `core::outermostDimensionTier`） | `core/Layout` |
| 軸組図のレベル記号の形と位置（▽ と名前の配置 `levelMarkShape`・起点 `levelMarkStartX`・基準線の長さ `levelLineLength`・帯の見込み `kLevelMarkBandAllowance`） | `core/Layout` |
| 長さ・文字の単位の換算（インチ → mm `kMillimetersPerInch`・1 インチの pt 数 `kPointsPerInch`・紙の pt → mm `pointsToMillimeters`） | `core/Layout` |
| 回転して置いた注釈の自身の高さを外接矩形から戻す（`rotatedRectHeight`。45 度近くで解けない境 `kRotatedRectMinConditioning`。傾斜材のデータタグの逃がし量＝`draw/Tag`） | `core/Layout` |
| 取り込み設定（役割の表 `core::symbolRoles()`・図面枠のスタイル `core::ImportOptions::titleBlock` → `core::Document::titleBlockStyle`・寸法規格 `core::ImportOptions::dimension` → `core::Document::dimensionStandard`・軸組図から外す通り `core::ImportOptions::skippedSections` → `parse::dropSkippedSections`・垂木の断面 `core::ImportOptions::rafterWidth` / `rafterHeight`（既定 `core::kDefaultRafterWidth` / `kDefaultRafterHeight`、文字との相互変換 `core::parseRafterSize` / `formatRafterSize`）） | `core/ImportOptions` |
| 基準からの差の表記（`signedMillimetreText`。符号付き・3 桁ごとのコンマ。伏図レベルの高さとデータタグの高さの注記が共有）・伏図レベルの印（`planLevelTag` / `stripPlanLevelTag` / `planLevelHeightText`。"2-横架材天端(FL-872)" の "(FL-872)" を付ける・外す・高さの表記） | `core/Document.h` |
| 伏図のまとめ方（`core::PlanLevelKey`・`ImportOptions::mergedPlanLevels`）・設定ダイアログへ運ぶ候補（`core::PlanLevelChoice`） | `core/ImportOptions.h` |
| まだ決めていない設定の初期値（図面にあるものから組む既定 `core::presetImportOptions`・寸法規格の既定の選び方 `core::defaultDimensionStandardIndex`。設定ダイアログの初期値と実機テストの自動の 1 周目が共有する。M40） | `core/ImportOptions` |
| 進捗の整形と配分の計算・診断ログのフェーズの行（`beginPhase`） | `core/Progress` |
| 実機テストの記憶と、どの周になるかの場合分け（`feedbackRoundKind`）・報告の置き場所（`testReportPathFor`）・保存せずに閉じてよい図面か（`isOwnedTestDocument`。閉じる相手を絞る唯一の安全弁） | `core/FeedbackSession` |
| 実機テストの一時ファイルの置き場（`prepareBranchScratch`）・PR が閉じたブランチの片付け（`removeScratchDir` が唯一の削除口） | `core/FeedbackScratch` |
| ブランチの PR が開いているか（`vw-update` の `q-pr-state`） | `scripts/vw-update.{sh,ps1}` |
| MCP ブリッジの受け渡しの作法（要求／応答の形・スプールのファイル名・原子的な書き方・id の綴り検査） | `core/Bridge.h` |
| 最小 JSON（MCP ブリッジ専用） | `core/Json` |

**`parse/`**

| もの | 置き場所 |
| --- | --- |
| IFC の属性インデックス | `parse/IfcAttr.h` |
| 構造クラス名（`04構造-…` の階層の葉クラス・構成要素の素材クラス）と部材種別からのクラス判定 | `parse/StructuralClass.h` |
| レベル種別名（定義は `core/Document.h`。ここは再公開）・`storyLayerName`・横架材レベルの定型（`beamTopLevelType` / `beamTopElevation` / `beamTopLayerName`）・横架材の取り合いを見るときのレイヤの読み替え（`beamGroupLayer`。軒桁の専用レイヤ→横架材レイヤ）・階の要素の有無（`storyHasElement`）・span レベルの表記（`formatSpanLevel`）・span レイヤ名の組み立てと分解（`spanLayerName` / `parseSpanLayer`。柱・登り梁で接尾辞だけ違う） | `parse/Story.h` |
| 屋根組の名前 | `parse/Rafter.h` / `parse/Roof.h` |
| 基礎ストーリの名前・接尾辞・レベル・レイヤ名、基礎の許容値（統合・自由端・人通口・壁結合・地中梁・床付け） | `parse/Footing.h` |
| 要素の判別述語（`isFloorSlab` / `isRoofSlab` / `isFireBrace` / `isBaseSlab` / `isShearBrace` / `isShearPanel` 等） | その要素のヘッダ |
| 金物（`IfcMechanicalFastener`）の型名取得（`fastenerTypeName`） | `parse/Column` |
| 取り込み設定に依らないアンカーボルトの位置と役割（`collectAnchorBolts`）・床束の位置（`floorPostPositions`）。命令とは別に継手の向き（`parse/Splice`）が使う | `parse/AnchorBolt` / `parse/FloorPost` |
| 横架材の端部と相手の取り合いの幾何（`memberEndJoint`）・柱に取り付く端を柱芯へ送る関門（`resolveMemberColumnJoints`。`parse/BuildDocument` が一度だけ通す） | `parse/Member` |
| ローカル配置原点の取り出し（`resolveLocalPlacementOrigin`）・屋根面の勾配座標系と退化の閾値・押し出しを鉛直とみなす閾値（`kVerticalExtrudeTol`） | `parse/IfcGeometry` |
| 共有コンテキスト（下記）・階の屋根面の走査（`storyRoofPlanes`）・取り込み設定の参照（`options()`） | `parse/Context` |
| 伏図記号レイヤ名（`{to}-柱伏図記号`）と記号の作図クラス（シンボル名は取り込み設定 `core/ImportOptions` が持つ） | `parse/ColumnMark` |
| 伏図レベル（横架材の高さごとの伏図 1 枚ぶん。高さの集め方・まとめ方・標準の決め方・近い／届く伏図レベルの引き方・伏図レベルのレイヤ名とずらし量・横架材の振り分け・設定ダイアログの候補）。共有は `Context::planLevels()` を通す | `parse/PlanLevel` |
| 柱の span の番号（伏図レベルの通し番号。`spanFromOrdinal` / `spanToOrdinal`） | `parse/Column` |
| 耐力壁のレイヤレベル名・柱を探す許容 | `parse/ShearWall.h` |
| 切断面に乗る材・横切る材の判定（`memberOnCutPlane` / `columnOnCutPlane` / `memberCrossesCutPlane`。タグと寸法が共有） | `parse/Tag` |
| データタグに添える横架材の高さの注記（`memberLevelNote` / 連動させる基準名も返す `memberLevelNoteParts`。その階の FL から）・各階の標準の横架材の高さ（`standardBeamHeights`。`parse/PlanLevel`） | `parse/Tag` |
| 寸法の測点のまとめ方（`mergeStops` / `unionStops`。許容 `kDimensionMergeTol`）・通り芯の位置（`gridStops`）・レベル記号の表示名 | `parse/Dimension` |
| 軸組図の図番の一意化（`uniqueSectionNumbers`） | `parse/Section` |
| 要素の一覧（表示名・助数詞・命令数・描けた数。`kElements`）・完了／エラーの文言（`importOutcome` 等） | `parse/Summary` |
| 実機テストの結末の文言（`formatTestRoundResult`）・報告の本文（内訳・前の周との差分・図面の状態・診断ログの切り詰め `keepTail`） | `parse/Feedback` |

**`draw/`**

| もの | 置き場所 |
| --- | --- |
| SDK 呼び出しの定型（クラス分け・レイヤ用意・プラグインスタイル解決・構成層／基準面を各オブジェクトへ直接与える手順） | `draw/DrawUtil` |
| 注釈（寸法・データタグ・レベル基準線）のクラス名 `kDimensionClass`・PIO のプロファイルグループの取り出し（`HeldProfileGroup`。直接と aux の両方を見る）・文字列のオブジェクト変数（`SetTextVariable`）・寸法規格の一覧と文字スタイル（`DimensionStandards` / `DimensionStandardTextStyle`） | `draw/DrawUtil` |
| SDK へ渡す数値の列挙（`LayerKind` / `LayerVisibility` / `ClassVisibility` / `ObjectNodeType` / `ObjectVariable` / `StoryBoundSlot`。素の short に名前を付ける唯一の場所）・高さ基準の変換（`StoryBoundData`） | `draw/DrawUtil` |
| オブジェクト変数の書き込み（`SetBooleanVariable` / `SetRealVariable` / `SetPointVariable`）・クラス分けと属性の by-class 化（`SetClassWithAttributes`。構造材 PIO は作る前に既定として立てる `ScopedCreationClass` ＋ `FinishCreatedWithClass`） | `draw/DrawUtil` |
| PIO 定義の先出し（`PrepareCustomObjectDefinition`）・PIO のパラメータを読む口（`PioParamString`）・構造用途の述語（`StructuralUseOf`）・シンボル定義の有無（`HasSymbolDefinition`） | `draw/DrawUtil` |
| 描画ループの中止判定と歩進（`AdvanceProgress`）・診断の 1 文（`AppendCount`）・診断行の連結（`AppendLine`）・登場順の dedupe（`PushUnique`） | `draw/DrawUtil` |
| 収まり判定の遊び（`kFitTol`。**遊びは緩める向きに足す**）・収まらなかった 1 枚目の実測の文言（`DescribeFitOverflow` / `DescribePaperSize`） | `draw/DrawUtil` |
| シートレイヤの用意とビューポートの仕上げ・用紙と印刷可能領域の読み取り（`SheetPaperArea`）・測って動かす位置合わせ（`MeasureViewport` / `RefreshViewport` / `MoveViewportBy`）・断面の向きをビュー行列へ写す（`CopySectionViewMatrix`。注釈のレベル基準線に高さを出す） | `draw/DrawUtil` |
| 取り込みの終わりに全レイヤの選択を解く（`DeselectEverything`） | `draw/DrawUtil` |
| 「命令インデックス → ハンドル」の対応表 | `draw/ObjectHandles`（宣言）＋ `draw/DrawUtil`（実体） |
| 断面寸法データタグ（`Data Tag` PIO の登録名・引出線・配置手順・タグレイアウトの組み方・文字スタイル名 "寸法(6pt)"。クラスは `draw/DrawUtil` の `kDimensionClass`） | `draw/Tag` |
| 軸組図の図面ラベル（`Drawing Label2` PIO の登録名・ラベルレイアウトの組み直し・文字スタイル名 "図面ラベル(10pt)"） | `draw/DrawingLabel` |
| グラフィック凡例（`GraphicLegend` PIO の登録名・箱幅／線の太さ／塗り・配置・ソース定義（タグ付きデータ `'GrLe'`）・縮率は触らない（PIO 既定の 1:50 のまま）・幅の実測 `measureLegendWidth`） | `draw/Legend` |
| 図面枠（登録名の候補 `"Title Block Border"`・スタイルの当て方・用紙の中心への寄せ方） | `draw/TitleBlock` |
| 図面枠スタイルの選択肢の集め方（シンボル定義のサブタイプ 552） | `draw/SettingsDialog` |
| 構造材ツール（StructuralMember PIO）のフィールド名・値（`MemberTypeKey` / `AxisAlignKey` / `EndConditionKey`）・生成手順・失敗の内訳と文言（`StructuralFailures` / `DescribeStructuralFailures`） | `draw/StructuralMember` |
| ハイブリッドシンボルの配置（アンカーボルト・床束・火打・仕口・継手の 5 要素で共有） | `draw/Symbol` |
| 進捗の見出し・バー配分（要素ごとのフェーズ） | `draw/ExecuteDocument` |
| 結果ダイアログの器（短い本文＋折り畳んだログ欄） | `draw/ResultDialog` |
| 診断ログの見出し・区切り・結果・例外（`trace::note`） | `draw/ImportRun`（ログへの書き出し口はここと `core/Progress` の 2 か所だけ。各要素へ `trace::log` を撒かない。実機テストの周では `draw/Feedback` も図面の保存の結果などを `note` で足す） |
| 検算と区間計測を dev ビルドだけにするスイッチ（`VW_DRAW_VERIFY` / `VW_DRAW_TIMING` と区間を刻む `VW_DRAW_TIME`。集計先は `core/DrawTiming` の `drawTiming()`） | `draw/Verify.h` |
| 殻から借りた道具（同梱スクリプトの実行） | `draw/HostServices` |
| MCP ブリッジの道具の表（`kTools`） | `draw/McpBridge.cpp` |

**殻・`Extensions/`**

| もの | 置き場所 |
| --- | --- |
| 柱記号 PIO の登録名・パラメータ名 | `Extensions/ExtColumnMark.h` |
| 耐力壁 PIO の登録名・パラメータ名・PIO が自分の絵へ与えるクラス（伏図記号／面材の表・裏）・伏図記号のシンボル定義名（`kShearMarkBraceSymbol` / `kShearMarkPanelSymbol`）と記号の寸法（`kShearMarkTriangleLength` / `kShearMarkTriangleHeight` / `kShearMarkCircleDiameter`） | `Extensions/ExtShearWall.h` |

**`tests/`・`scripts/`**

| もの | 置き場所 |
| --- | --- |
| フィクスチャ一覧・近似比較・実 IFC の読み込みと命令セットの組み立てのキャッシュ（`fixture` / `fixtureDocument`）・全フィクスチャ走査（`forEachFixture`） | `tests/Fixtures.h` |
| 合成 STEP テキストの組み立て（`StepText` と `num` / `ref` / `point3` / `makeStorey` 等） | `tests/StepText.h` |
| 試験用屋根面と最小 IFC | `tests/RoofSample.h` |
| 実機テストの 1 周目のテンプレート（MCP の `vw_run_test` の `template` に渡す `.sta`。M40） | `tests/fixtures/Default.sta` |
| GitHub のトークン（キーチェーン／DPAPI の保存先・探索順・`gh` の探し場所）。`vw-update` が source する | `scripts/vw-token.{sh,ps1}` |
| GitHub を叩いて失敗したときの理由の文面（HTTP の番号・curl の終了コード・API 制限といつ戻るか） | `vw-update` の `http_reason` / `curl_reason`（Windows は `Get-ApiFailureReason`）。呼び出し側は `api_error` で 1 行に添えるだけ |

補足:

- **共有コンテキスト（`parse/Context`）**: 各要素の解析が共通して要る前処理（ストーリ一覧・
  通り芯のセンタリング中心・階に属する要素・屋根面、および複数の要素が参照する横架材・柱・
  立上りの命令）をキャッシュします。`buildDocument` は `Context` を **1 つだけ**作って全要素へ
  渡します。単体テスト用に `const Model&` を直接取るオーバーロードも各 `build*Commands` に
  残してあり、そちらは内部でコンテキストを作って捨てます。
- **取り込み設定（`core/ImportOptions`）**: 「どの要素を図面のどのシンボルで置くか」と
  「各シートレイヤへ置く図面枠のスタイル」は取り込みのたびに設定ダイアログ
  （`draw/SettingsDialog`）で決まります。**解析側はシンボル名の固定値を持たず**、
  `parse/Context` の `options()` か `build*Commands` の `options` 引数から引きます。決めるのは
  描画側・使うのは解析側なので、Document と同じく `core/` に置きます。
- **スタイルの扱い**: データタグ・凡例・スラブ・壁は**スタイルを作らないし当てない**（各
  オブジェクトへ直接設定）。図面枠は**スタイルを当てるが作らない**——利用者の図面にある
  スタイルを名前で指すだけなので、その名前が無ければ 1 つも置きません。置けた枚数は伏図と
  軸組図で別々に出します。
- **外形を測る前に、中身を変えたなら必ず描き直す**（`GetObjectBounds` は「最後に描いたときの
  外形」を返す。M29）。**`GetPageMargins` は戻り値を持たない**ので、負を種に置いてから呼んで
  「SDK が書いたか」を見ます。
- **伏図の広がりはデータタグも見ます**（注釈なのでレイヤに載らないが図には映る。絞り込みは
  関連付け先の横架材のレイヤで行う）。軸組図のタグは見ません（注釈空間が平面座標ではない）。
  縮尺に追随しないもの（通り芯の丸・柱記号・耐力壁の伏図記号・タグ）の見込みは
  `kPlanContentMargin`（モデル mm の定数 1 つ）です。
- **描画側が持ち帰る説明は行き先を分けます**——異常は `DrawCounts::diagnostics`（完了
  ダイアログの「問題あり」の根拠）、平常でも出る記録は `DrawCounts::notes`（ログにだけ出る）。
