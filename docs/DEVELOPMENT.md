# 開発ガイド

このプラグインをビルド・テスト・lint し、CI とリリースを回すための手順です。

- 利用者向けの説明（何をするプラグインか・使い方）は [`README.md`](../README.md)。
- 設計判断・ホームズ君 IFC の癖は [`DEV-NOTES.md`](DEV-NOTES.md)。**Vectorworks SDK の
  実測知見は [SDK リファレンスリポジトリ](https://github.com/min-nano/vectorworks-developer-sdk-reference)の
  `Findings/`**（下記「SDK ドキュメント」）。
- 全変更に共通する規約（アーキテクチャ・依存の向き・コード規約・テスト方針・PR とマージの
  規則）は [`CLAUDE.md`](../CLAUDE.md)。このファイルには、そこから外した**領域ごとの細則**
  （置き場所の一覧・MCP ブリッジ・実機フィードバックの往復・自動アップデート・CI の待ち方と
  デバッグ）も置いています。

## ソースの構成

処理は **IFC 解析フェーズ（`src/parse/`）** と **VW 描画フェーズ（`src/draw/`）** に完全分離し、
両者は命令セット（`src/core/Document.h`）だけで接続します。`parse/` と `core/` は
**SDK を一切 include しない**ので、SDK 無しでコンパイル・単体テストできます
（設計の詳細は [`DEV-NOTES.md`](DEV-NOTES.md)「設計の考え方」）。

**ビルドの成果物も 2 つに割れています**（こちらはフェーズ分離とは別の軸）。

```
Vectorworks ──読み込む──▶ 殻 <name>.vwlibrary / .vlb    … 起動時に 1 度きり
                              │ dlopen / LoadLibrary
                              ▼
                          本体 <name>.vwpayload          … いつでも読み直せる
```

**殻**に入るのは「Vectorworks に番地を握られるもの」だけ——メニュー 4 つ（うち 1 つは
開発版だけ）と PIO 2 つ、パレット 1 つの*登録*、
自動アップデート、そして本体を読み込む仕掛け（`src/PayloadHost.*` / `src/PayloadSession.*`）。
**本体**に `core/` `parse/` `draw/` のすべてが入ります。境界は C の ABI
（`src/PayloadAbi.h`）1 枚きりです。

こう割ってあるのは、**アップデートに Vectorworks の再起動を要らなくする**ためです
（下記「自動アップデートの仕組み」／[SDK リファレンス「プラグインモジュールの読み込みと
入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。

```
CMakeLists.txt              macOS / Windows 両対応の CMake ビルド。SDK 非依存の
                            静的ライブラリ MinNanoStructureCore（core/ + parse/）と、
                            SDK 依存のプラグイン本体（draw/ ほか）に分かれる
src/
  ModuleMain.cpp            モジュールのエントリポイント。拡張機能を登録し、本体へ
                            渡す CallBackPtr を預ける（**アップデートの確認はここでは
                            しない**——下記「自動アップデートの仕組み」）
  PayloadAbi.h              **殻と本体の唯一の約束事**（C の ABI）。両方が include する
                            ので SDK にもプラットフォームにも依存しない
  PayloadHost.{h,cpp}       殻の側: 本体を一時ディレクトリへ複製して dlopen /
                            LoadLibrary し、版を確かめ、呼び、降ろす
  PayloadSession.{h,cpp}    殻の側: 「いま載っている本体」1 つと、入口ごとの載せ替え
                            判定（同梱ファイルの印が変わっていたら読み直す）
  PayloadHostHolder.h       本体の側: 殻から渡された構造体を**その場で写す**入れ物
                            （持ち続けると実機で落ちる。回帰テストあり）
  FeedbackLoop.{h,cpp}      殻の側: **モードレスの往復の駆動**（M24。合図 → 新しいビルド →
                            入れて降ろす → 取り込み → 投稿、と止まる条件。SDK 非依存・
                            テストあり）
  FeedbackLoopHost.{h,cpp}  殻の側: その駆動に本物の副作用（本体への問い合わせ・同梱
                            スクリプト・尋ねないアップデート）を結び、パレットの表示を行う
  payload/
    PayloadMain.cpp           本体のエントリポイント。GS_InitializeVCOM を自分で呼び、
                              取り込み・MCP ブリッジ・PIO のリセットを中の実装へ
                              取り次ぐ
  Extensions/               **殻**に残る「登録」だけ（実処理は draw/ 側）
    ExtMenu.{h,cpp}           「IFC (ホームズ君) 取り込み…」メニューコマンドの登録と、
                              本体の draw::runImportCommand への取り次ぎ。実行の頭で
                              静かなアップデート確認も行う。**往復のことは何も知らない**
                              （M25。それは ExtTestMenu の仕事）
    ExtTestMenu.{h,cpp}       「実機テストを実行…」メニューコマンドの登録と、本体の
                              draw::runTestRound への取り次ぎ（**dev だけ登録**。M25）。
                              往復の最中は尋ねずに更新する分岐もここに閉じている
    ExtUpdateMenu.{h,cpp}     「アップデータを確認 (みんなの構造設計支援)」メニュー
                              コマンドの登録と実行（**殻に残る唯一の実処理**である
                              自動アップデートを呼ぶ）
    ExtMcpMenu.{h,cpp}        「MCP ブリッジを表示…」メニューコマンドの登録（パレットを
                              出すだけ。開発・デバッグ用。README「MCP ブリッジ」）
    ExtMcpPalette.{h,cpp}     MCP ブリッジを常駐させる**モードレスなパレット**の登録と、
                              JS の時計から本体の draw::serveMcpBridge への取り次ぎ（M30。
                              安定版にも登録。中身は resources/common.vwr/html/mcp.html）
    ExtColumnMark.{h,cpp}     柱・小屋束の記号 PIO の登録（パラメータ定義・UUID）と、
                              本体の draw::recalculateColumnMark への取り次ぎ
    ExtShearWall.{h,cpp}      耐力壁 PIO の同上（→ draw::recalculateShearWall）
    ExtFeedbackPalette.{h,cpp} 実機フィードバックの往復を回す**モードレスなパレット**の
                              登録と、JS からの呼び出しの取り次ぎ（M24。dev だけ登録。
                              中身は resources/<vwr>/html/index.html）
  core/                     フェーズ非依存の土台（SDK も STEP も知らない純粋コード）
    Document.{h,cpp}          命令セットの構造体定義・validateDocument・描画結果の件数
    ImportOptions.{h,cpp}     取り込み設定（配置するシンボルの対応）と役割の表 1 つ
    FeedbackSession.{h,cpp}   実機フィードバックの往復で覚えておく値（1 周目の選択）と
                              その読み書き
    Geometry.{h,cpp}          自前の Vec2 / Vec3 / Mat4（配置行列）と平面幾何の基本演算
    Layout.{h,cpp}            用紙の割り付け（縮尺の階梯と選び方・伏図の位置と凡例の列・
                              軸組図の上下 2 段とシートの分割）
    Region.{h,cpp}            部品が囲む平面領域の合成（ロフト床の外形）
    UnionFind.h               ペア述語による連結成分（立上り・大引・地中梁の統合が共有）
    Progress.{h,cpp}          進捗の報告先・文言整形・バー配分（実測の重み）
    Trace.{h,cpp}             診断ログ（フェーズ単位・毎行フラッシュ・本文はメモリにも控える）
    DrawTiming.{h,cpp}        描画の区間計測（名前ごとの累計・時間の長い順の整形。集計先は
                              drawTiming() ただ 1 つ。**開発ビルドでしか積まれない**
                              ——刻む側が draw/Verify.h の VW_DRAW_TIMING で消える）
    Json.{h,cpp}              最小 JSON（書き出し・読み取り。MCP ブリッジが使う唯一の器）
    Bridge.{h,cpp}            MCP ブリッジの受け渡し（要求／応答の形とスプールの作法）
  parse/                    Phase 1: IFC 解析（SDK 非依存）
    Step.{h,cpp}              最小 STEP リーダ（トークナイザ＋エンティティグラフ）
    Loader.{h,cpp}            ファイル読み込み（テキスト → STEP グラフ）
    IfcAttr.h                 IFC 属性インデックスの唯一の定義
    IfcGeometry.{h,cpp}       配置行列・断面・押し出し・屋根面の解決
    Context.{h,cpp}           解析中の共有キャッシュ（同じ前処理を繰り返さない）
    BuildDocument.{h,cpp}     解析のオーケストレーション
    Summary.{h,cpp}           完了・エラーダイアログの本文と診断ログの見出し／結果
                              （要素の一覧は kElements 表 1 つ）
    StructuralClass.{h,cpp}   部材種別 → VW クラスの純ロジック
    Feedback.{h,cpp}          実機フィードバックの PR コメント本文（内訳・前の周との差分・
                              匿名化）
    Grid / Story / Floor / Member / Noboribari / Column / Rafter / Roof /
    Footing / AnchorBolt / FloorPost / FireBrace / Joint / ColumnMark /
    Sheet / Tag / Section      要素ごとの解析
  draw/                     Phase 2: VW 描画（SDK 依存）。**まるごと本体に入る**
    ImportCommand.{h,cpp}     本番の取り込みコマンド（ファイル選択 → 設定 → 取り込み →
                              完了ダイアログ）。**往復の分岐が 1 つも無い**（M25）
    ImportRun.{h,cpp}         取り込み 1 周ぶんの部品——ファイル選択・ビルドの素性・
                              解析 → 描画 → 集計。**本番の取り込みと実機テストが
                              共有する唯一の実装**で、診断ログの見出し・区切り・結果も
                              ここが書く（M25）
    Feedback.{h,cpp}          実機テストの 1 周（runTestRound）と往復の運転——記憶・
                              取り込み前のダイアログ・準備・投稿。**往復を知っているのは
                              本体ではここだけ**
    ColumnMarkPio.{h,cpp}     柱・小屋束の記号 PIO のリセット本体（対象レイヤの構造材を
                              走査して断面記号 ×／／ と平面記号を描く）
    ShearWallPio.{h,cpp}      耐力壁 PIO のリセット本体（両端の柱から内法を求め、
                              伏図記号 2D と軸組図の面 3D を描く）
    ExecuteDocument.{h,cpp}   命令セットを検証して要素ごとにディスパッチ
    DrawUtil.{h,cpp}          クラス分け・by-class 属性・レイヤ／シートレイヤの用意・
                              構成層・ビューポートの仕上げ（縮尺・用紙の大きさ・位置合わせ）・
                              Undo スコープの共通ヘルパー
    StructuralMember.{h,cpp}  構造材ツール 1 本の生成・設定（横架材／柱で共有）
    ObjectHandles.h           「命令インデックス → 描いたオブジェクトのハンドル」の対応表
    Verify.h                  **開発ビルドにしかコンパイルしないもののスイッチを置く唯一の
                              場所**——検算（VW_DRAW_VERIFY）と区間計測（VW_DRAW_TIMING ＋
                              VW_DRAW_TIME）。囲むかどうかの基準も同じ 1 つ
    McpBridge.{h,cpp}         MCP ブリッジの本体（1 回ぶんの受け付けと道具の表。**道具を足すときに
                              触るのはこの表 1 行**）
    ProgressDialog.{h,cpp}    core::ProgressReporter を VW の進捗ダイアログへ橋渡し
    ResultDialog.{h,cpp}      完了・エラーのダイアログ（短い本文＋折り畳んだ診断ログ欄）
    SettingsDialog.{h,cpp}    取り込み設定ダイアログ（配置するシンボルを名前と絵で選ぶ）
    Feedback.{h,cpp}          実機フィードバックの往復（取り込みの前に送るか決め、
                              終わったら黙って PR へ投稿。待たないし入れもしない）
    HostServices.{h,cpp}      殻から借りた道具（同梱スクリプトの実行）の置き場所
    Symbol.{h,cpp}            ハイブリッドシンボルの配置（4 要素で共有する唯一の実装）
    Tag.{h,cpp}               断面寸法データタグ（伏図・軸組図で共有。スタイルは当てず、
                              タグの中身はタグ 1 本ずつへ直接組む）
    Grid / Story / Floor / Member / Column / Rafter / Roof / Footing /
    TitleBlock.{h,cpp}        図面枠（伏図・軸組図で共有。図面にあるスタイルを当てる
                              だけで、スタイルは作らない）
    ColumnMark / Sheet / Legend / Section   要素ごとの描画
  Updater*.{h,cpp}          同梱した更新スクリプトを起動してアップデートを駆動する
                            （同梱スクリプトの実行は本体へも貸し出す）
  BuildConfig.h             stable / dev の識別切り替えスイッチ（VW_DEV_BUILD）
  PluginPrefix.h            共有プレフィックスヘッダ（SDK を取り込む）
  Module-Info.plist.in      バンドルの Info.plist テンプレート（macOS 専用）
scripts/
  mcp/vw-mcp-server.py      MCP ブリッジの Claude 側（依存の無い Python。配布 zip へ
                            同梱され、インストール先へ一緒に置かれる）
tests/                      無 SDK の単体テスト（詳細は tests/README.md）
  TestFramework.h           依存ゼロの極小テストハーネス
  Fixtures.h / RoofSample.h 共有するフィクスチャ読み込み・近似比較・試験用屋根面
  fixtures/                 ホームズ君 EX 出力の実 IFC
resources/
  min-nano_structure.vwr/…           stable プラグインのメニュー文字列
  min-nano_structureDev.vwr/…        dev プラグインのメニュー文字列と、往復パレットの
                                     HTML/JS（html/index.html。M24）
  common.vwr/…                       両方に共通の中身（MCP ブリッジのパレットの
                                     html/mcp.html。M30）。包む直前に各 .vwr の写しへ
                                     重ねる（CMakeLists.txt）
scripts/
  vw-update.sh              CI ビルドを探して落としてくる（macOS 用。バンドルに同梱
                            され、プラグインから起動される）。**配置はしない**
                            ——落とした zip の中の vw-install.sh へ委ねる
  vw-update.ps1             同上の Windows 版（PowerShell。.vlb の隣に同梱される）
  vw-install.sh             **配置の手順**（macOS 用）。配布 zip の直下に同梱され、
                            リリースのアセットとしても公開される。手動インストールの
                            入口でもある（下記「自動アップデートの仕組み」）
  vw-install.ps1            同上の Windows 版
  vw-uninstall.sh           **取り除く手順**（macOS 用）。zip の直下に同梱され、
                            **インストール先へ一緒に置かれる**——次のアップデートが
                            「前の版を、その版自身の知識で取り除く」ために使う
  vw-uninstall.ps1          同上の Windows 版
  vw-feedback.sh            **実機フィードバックの投稿**（macOS 用。バンドルに同梱され、
                            本体が殻越しに起動する）。トークンの保管・PR の特定・
                            コメントの投稿・**往復を続けてよいかの確認**（`loop-control`。
                            M24）（下記「実機フィードバックの往復」）
  vw-feedback.ps1           同上の Windows 版（.vlb の隣に同梱される）
  vw-token.sh / .ps1        **GitHub のトークンの在り処**（保存先・探索順・gh の探し場所）。
                            実行はせず、vw-feedback と vw-update の両方が source する
                            ——読むほうにも必ず付ける（認証なしの GitHub API は IP ごとに
                            1 時間 60 回で、1 分ごとの往復の確認はちょうどそこに当たる。M27）
  lint.sh                   ローカルで全 lint（clang / cmake / yaml / shell …）
                            を実行する（CI と同じチェック。--fix で自動修正）
  clang-tidy-sdk.sh         SDK 依存の翻訳単位（src/draw/ ほか）に clang-tidy を
                            かける（対象一覧の唯一の定義）。-c で結果キャッシュ
  tidy-cache-key.py         その結果キャッシュの鍵を 1 翻訳単位ぶん計算する
                            （入力すべて＝推移的な include・コンパイル指令・規則・
                            clang-tidy の版・SDK とランナーイメージの同一性 を
                            ハッシュにまとめる）
  fetch-vw-sdk.sh           Vectorworks SDK をランナーへ用意する（ダウンロード →
                            トリミング → 検証。build.yml と ci-debug.yml が共有）
  ci-common.sh              CI の完了待ちの共通土台（必ず有限時間で exit する歯止め）
  ci-wait.sh                PR ／ブランチ ／コミットの CI が終わった瞬間に exit する
  ci-debug.sh               CI デバッグ実行を起動し、完了まで待って結果を取り出す
                            （SDK が手元に無い環境から SDK 依存の調査を行うため）
  ci-debug-job.sh           同・ランナー側の本体。調査モードの実装はこちらにある
  vw-dump-pio-fields.py     VW 実機の「スクリプト編集」に貼って走らせる読み取り専用の
                            ダンプ（選択オブジェクトのパラメトリックレコード・付いて
                            いるレコード・オブジェクト変数・文書内のビューポート一覧）。
                            SDK に API の無い PIO の設定を、UI で手作業したものと
                            見比べて突き止めるための道具（CI では使わない）
.clang-format               C/C++ フォーマット規則（タブ・Allman ブレース等）
.clang-tidy                 C/C++ 静的解析チェックの設定（WarningsAsErrors）
.cmake-format.yaml          CMake の整形（cmake-format）＋ lint（cmake-lint）設定
.yamllint.yaml              YAML の構造スタイル（yamllint）設定
PSScriptAnalyzerSettings.psd1  PowerShell 静的解析（PSScriptAnalyzer）のルール設定
.editorconfig               エディタ側のインデント／改行／文字コード規則
.editorconfig-checker.json  上記を CI で強制する editorconfig-checker の設定
.github/workflows/build.yml CI: macOS（Apple Silicon）と Windows でビルドし、
                            リリース（main=stable / PR=dev）を公開する
.github/workflows/test.yml  CI: 無 SDK の単体テスト（ASan+UBSan）とカバレッジ
.github/workflows/lint.yml  CI: ソース／非ソースを問わずコーディング規則を強制
.github/workflows/codeql.yml            CI: CodeQL による静的解析（週次＋PR）
.github/workflows/pr-review.yml CI: PR を Claude にレビューさせる（設計規約に
                            照らした指摘とインラインコメント。書式は lint.yml が見る）
.github/workflows/cleanup-dev-release.yml  PR のクローズ時に dev プレリリースを片付ける
.github/workflows/stable-release-healthcheck.yml
                            stable リリースの取りこぼしを検知して再ビルドする
.github/workflows/ci-debug.yml  CI: 手動ディスパッチ専用のデバッグ実行（SDK 依存の
                            ビルド再現）。push / PR では起動しない。SDK そのものの
                            調査は SDK リファレンス側で行う（下記「CI デバッグ」）
```

**依存の向きは厳守します。** `parse/` と `core/` は Vectorworks SDK を include せず、
`draw/` は STEP / IFC を include しません。両者をつなぐのは `core/Document.h` だけで、
この規律は CI（`core/` `parse/` を無 SDK でコンパイル・テストするジョブ）が担保します。

### 置き場所の一覧（重複を作らない）

同じ定数・述語・ヘルパーを 2 か所に書かないための、**既存の唯一の置き場所**の一覧です
（原則は [`CLAUDE.md`](../CLAUDE.md)「重複を作らない置き場所」）。新しく共有するものを
作ったら、ここに 1 行足してください。

**要素を 1 つ足すときの型**: `parse/<要素>.{h,cpp}`（解析）＋ `core/Document.h` の命令構造体と
`validateDocument` の検証＋ `draw/<要素>.{h,cpp}`（描画）＋ `tests/Parse<要素>Tests.cpp`
（無 SDK テスト）＋ `parse/Summary.cpp` の `kElements` に 1 行。PIO を足すときは
`Extensions/Ext<要素>.{h,cpp}`（殻: 登録と取り次ぎ）＋ `draw/<要素>Pio.{h,cpp}`（本体: 作図）に
割ります。

**`core/`**

| もの | 置き場所 |
| --- | --- |
| 平面座標の同一判定と許容（`samePoint` / `kPointEps`）・Vec2 の基本演算（`dot` / `cross` / `length` / `distance`）・同一直線上の線分成分の芯線射影（`collinearSpan`）・凸多角形の矩形クリップ（`clipPolygonToRect`。唯一の利用者は耐力壁の筋かいの形 `core::shearWallBracePolygon`） | `core/Geometry.h` |
| ペア述語による連結成分（Union-Find。立上り・大引・地中梁の統合と壁結合の交点クラスタ） | `core/UnionFind.h` |
| 構成層の総厚（`totalThickness`）・横架材の Z 範囲と重なり（`memberTopZ` / `memberBottomZ` / `zRangesOverlap`。許容値は呼び出し側）・端部オフセットの意味と値・オフセットを戻した「材の端」（`memberDrawnStart` / `memberDrawnEnd` / `columnDrawnTop` / `columnDrawnBottom`） | `core/Document.h` |
| 描画側から切り離せる純計算（レイヤの希望スタック順 `desiredStoryLayerOrder`・地中梁の可視ソリッドの呑み込み `raiseModifierTop`・図に映るものの広がり `planContentBounds` / `sectionContentSize`） | `core/Document` |
| 用紙の割り付け（`core::planLayout` ほか。縮尺の階梯と選び方・伏図の縮尺と位置——**縮尺は実測した凡例の幅を引いてから決め**、凡例の置き場所は `legendTopRight`——・軸組図の上下 2 段とシートの分割・タイトルの連番） | `core/Layout` |
| 取り込み設定（役割の表 `core::symbolRoles()`・図面枠のスタイル `core::ImportOptions::titleBlock` → `core::Document::titleBlockStyle`） | `core/ImportOptions` |
| 進捗の整形と配分の計算・診断ログのフェーズの行（`beginPhase`） | `core/Progress` |
| 往復の記憶と、どの周になるかの場合分け（`feedbackRoundKind`） | `core/FeedbackSession` |
| MCP ブリッジの受け渡しの作法（要求／応答の形・スプールのファイル名・原子的な書き方・id の綴り検査） | `core/Bridge.h` |
| 最小 JSON（MCP ブリッジ専用） | `core/Json` |

**`parse/`**

| もの | 置き場所 |
| --- | --- |
| IFC の属性インデックス | `parse/IfcAttr.h` |
| レベル種別名・`storyLayerName`・横架材レベルの定型（`beamTopLevelType` / `beamTopElevation` / `beamTopLayerName`）・階の要素の有無（`storyHasElement`）・span レベルの表記（`formatSpanLevel`） | `parse/Story.h` |
| 屋根組の名前 | `parse/Rafter.h` / `parse/Roof.h` |
| 基礎ストーリの名前・接尾辞・レベル・レイヤ名、基礎の許容値（統合・自由端・人通口・壁結合・地中梁・床付け） | `parse/Footing.h` |
| 要素の判別述語（`isFloorSlab` / `isRoofSlab` / `isFireBrace` / `isBaseSlab` / `isShearBrace` / `isShearPanel` 等） | その要素のヘッダ |
| 金物（`IfcMechanicalFastener`）の型名取得（`fastenerTypeName`） | `parse/Column` |
| 横架材の端部と相手の取り合いの幾何（`memberEndJoint`）・柱に取り付く端を柱芯へ送る関門（`resolveMemberColumnJoints`。`parse/BuildDocument` が一度だけ通す） | `parse/Member` |
| ローカル配置原点の取り出し（`resolveLocalPlacementOrigin`）・屋根面の勾配座標系と退化の閾値・押し出しを鉛直とみなす閾値（`kVerticalExtrudeTol`） | `parse/IfcGeometry` |
| 共有コンテキスト（下記）・階の屋根面の走査（`storyRoofPlanes`）・取り込み設定の参照（`options()`） | `parse/Context` |
| 伏図記号レイヤ名（`{to}-柱伏図記号`）と記号の作図クラス・シンボル名 | `parse/ColumnMark` |
| 耐力壁のレイヤレベル名・柱を探す許容 | `parse/ShearWall.h` |
| 断面の注釈空間への投影（`sectionAnnotationPoint`） | `parse/Tag` |
| 軸組図の図番の一意化（`uniqueSectionNumbers`） | `parse/Section` |
| 要素の一覧（表示名・助数詞・命令数・描けた数。`kElements`）・完了／エラーの文言（`importOutcome` 等） | `parse/Summary` |
| 実機テストの結末の文言（`formatTestRoundResult`）・PR コメントの本文（内訳・差分・匿名化） | `parse/Feedback` |

**`draw/`**

| もの | 置き場所 |
| --- | --- |
| SDK 呼び出しの定型（クラス分け・レイヤ用意・プラグインスタイル解決・構成層／基準面を各オブジェクトへ直接与える手順） | `draw/DrawUtil` |
| SDK へ渡す数値の列挙（`LayerKind` / `LayerVisibility` / `ClassVisibility` / `ObjectNodeType` / `ObjectVariable` / `StoryBoundSlot`。素の short に名前を付ける唯一の場所）・高さ基準の変換（`StoryBoundData`） | `draw/DrawUtil` |
| オブジェクト変数の書き込み（`SetBooleanVariable` / `SetRealVariable` / `SetPointVariable`）・クラス分けと属性の by-class 化（`SetClassWithAttributes`。構造材 PIO は作る前に既定として立てる `ScopedCreationClass` ＋ `FinishCreatedWithClass`） | `draw/DrawUtil` |
| PIO 定義の先出し（`PrepareCustomObjectDefinition`）・PIO のパラメータを読む口（`PioParamString`）・構造用途の述語（`StructuralUseOf`）・シンボル定義の有無（`HasSymbolDefinition`） | `draw/DrawUtil` |
| 描画ループの中止判定と歩進（`AdvanceProgress`）・診断の 1 文（`AppendCount`）・診断行の連結（`AppendLine`）・登場順の dedupe（`PushUnique`） | `draw/DrawUtil` |
| 収まり判定の遊び（`kFitTol`。**遊びは緩める向きに足す**）・収まらなかった 1 枚目の実測の文言（`DescribeFitOverflow` / `DescribePaperSize`） | `draw/DrawUtil` |
| シートレイヤの用意とビューポートの仕上げ・用紙と印刷可能領域の読み取り（`SheetPaperArea`）・測って動かす位置合わせ（`MeasureViewport` / `RefreshViewport` / `MoveViewportBy`） | `draw/DrawUtil` |
| 図面から自分が作ったレイヤを消す（`RemoveCreatedLayers`）・取り消しを 1 段掛ける（`UndoOneStep`） | `draw/DrawUtil` |
| 「命令インデックス → ハンドル」の対応表 | `draw/ObjectHandles`（宣言）＋ `draw/DrawUtil`（実体） |
| 断面寸法データタグ（`Data Tag` PIO の登録名・引出線・配置手順・タグレイアウトの組み方・クラス名 "寸法"） | `draw/Tag` |
| グラフィック凡例（`GraphicLegend` PIO の登録名・箱幅／線の太さ／塗り・配置・ソース定義（タグ付きデータ `'GrLe'`）・縮率（伏図の縮尺に合わせる）・幅の実測 `measureLegendWidth`） | `draw/Legend` |
| 図面枠（登録名の候補 `"Title Block Border"`・スタイルの当て方・用紙の中心への寄せ方） | `draw/TitleBlock` |
| 図面枠スタイルの選択肢の集め方（シンボル定義のサブタイプ 552） | `draw/SettingsDialog` |
| 構造材ツール（StructuralMember PIO）のフィールド名・値（`MemberTypeKey` / `AxisAlignKey` / `EndConditionKey`）・生成手順・失敗の内訳と文言（`StructuralFailures` / `DescribeStructuralFailures`） | `draw/StructuralMember` |
| ハイブリッドシンボルの配置（4 要素で共有） | `draw/Symbol` |
| 進捗の見出し・バー配分（要素ごとのフェーズ） | `draw/ExecuteDocument` |
| 結果ダイアログの器（短い本文＋折り畳んだログ欄） | `draw/ResultDialog` |
| 検算を dev ビルドだけにするスイッチ（`VW_DRAW_VERIFY`） | `draw/Verify.h` |
| 殻から借りた道具（同梱スクリプトの実行） | `draw/HostServices` |
| MCP ブリッジの道具の表（`kTools`） | `draw/McpBridge.cpp` |

**殻・`Extensions/`**

| もの | 置き場所 |
| --- | --- |
| 柱記号 PIO の登録名・パラメータ名 | `Extensions/ExtColumnMark.h` |
| 耐力壁 PIO の登録名・パラメータ名・PIO が自分の絵へ与えるクラス（伏図記号／面材の表・裏） | `Extensions/ExtShearWall.h`（伏図記号の寸法 `kMark*` は `ExtShearWall.cpp`） |
| 診断ログの見出し・区切り・結果・例外（`trace::note`） | `Extensions/ExtMenu`（ログへの書き出し口はここと `core/Progress` の 2 か所だけ。各要素へ `trace::log` を撒かない） |

**`tests/`・`scripts/`**

| もの | 置き場所 |
| --- | --- |
| フィクスチャ一覧・近似比較・実 IFC の読み込みと命令セットの組み立てのキャッシュ（`fixture` / `fixtureDocument`）・全フィクスチャ走査（`forEachFixture`） | `tests/Fixtures.h` |
| 合成 STEP テキストの組み立て（`StepText` と `num` / `ref` / `point3` / `makeStorey` 等） | `tests/StepText.h` |
| 試験用屋根面と最小 IFC | `tests/RoofSample.h` |
| GitHub のトークン（キーチェーン／DPAPI の保存先・探索順・`gh` の探し場所）。`vw-feedback` と `vw-update` が source する | `scripts/vw-token.{sh,ps1}` |
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

## プラグイン識別子

このプラグインを一意に識別する値は次の通りです（出発点にしたネイティブプラグイン
テンプレート `vectorworks-plugin-native-template` のプレースホルダーを、これらへ
置き換えてあります）。フォークして別プラグインを作るときは、同じ箇所を自分の値へ
置き換えます。

| 種別 | 値 | 場所 |
| --- | --- | --- |
| プラグイン表示名 | `みんなの構造設計支援` / `みんなの構造設計支援Dev` | `resources/*/Strings/*.vwstrings`（コマンド名の中）、`src/UpdaterFlow.cpp`（ダイアログの文言） |
| バンドル／出力名 | `min-nano_structure` / `min-nano_structureDev` | `CMakeLists.txt`、`src/BuildConfig.h`、`resources/` フォルダ名、`scripts/vw-update.sh`、`scripts/vw-update.ps1`、`.github/workflows/build.yml`（`scripts/vw-install.*` は名前を決め打ちせず、アーカイブから読み取ります） |
| CMake のプロジェクト／ターゲット名 | `MinNanoStructure(Dev)` / `MinNanoStructureCore` | `CMakeLists.txt`、`tests/CMakeLists.txt` |
| バンドル ID（macOS） | `io.github.min-nano.structure` / `io.github.min-nano.structure-dev` | `CMakeLists.txt` |
| メニューカテゴリ | `みんなの構造設計支援` / `みんなの構造設計支援Dev`（コマンド名 `IFC (ホームズ君) 取り込み…` / `アップデータを確認 (みんなの構造設計支援)`）。**このプラグインのコマンドは全部このカテゴリに入れる**——`.vwr` の `"category"` ただ 1 つを両方のメニュー定義が引く | `resources/*/Strings/*.vwstrings` |
| C++ 名前空間・クラス | `min-nano_structure` / `CExtMenuImportIfc` / `CExtMenuCheckUpdate` | `src/Extensions/Ext*.{h,cpp}`、`src/ModuleMain.cpp` |
| VCOM ユニバーサル名 | 取り込み: `CExtMenuImportIfc_HomeskzIfcImport(Dev)`／更新: `CExtMenuCheckUpdate_MinNanoStructure(Dev)`／MCP: `CExtMenuMcpBridge_MinNanoStructure(Dev)`／実機テスト: `CExtMenuTest_MinNanoStructure(Dev)`（登録は dev だけ）／往復パレット: `CExtFeedbackPalette_MinNanoStructure(Dev)`（登録は dev だけ）／MCP パレット: `CExtMcpPalette_MinNanoStructure(Dev)` | `src/BuildConfig.h` |
| 拡張機能 UUID | コマンド 2 つ × stable / dev の 4 個＋PIO 2 つ × 2＋往復パレット × 2＋MCP パレット × 2 | `src/Extensions/Ext*.cpp`（一意である必要があるため `uuidgen` で再生成） |

> **名前空間 `min-nano_structure` と取り込みコマンドのユニバーサル名・UUID は、改名後も
> 据え置いています。** ユニバーサル名と UUID は**コマンドの同一性そのもの**で、付け替えると
> 利用者のワークスペースからコマンドが消えます（登録し直しになります）。名前空間は
> 図面にも配布物にも現れない内部の綴りなので、改名の巻き添えで 200 ファイル超を書き換える
> 価値がありません。
| リポジトリ | `min-nano/vectorworks-plugin-import-ifc-homeskz` | `scripts/vw-update.{sh,ps1}` / `scripts/vw-install.{sh,ps1}` の `VW_REPO` 既定値 |

`.vwstrings` は UTF-16LE（BOM 付き・CRLF 改行）です。編集時はエンコーディングを保持
してください。現在の識別子は次で一覧できます。

```sh
grep -rniE "homeskzifcimport|io\.github\.min-nano|CExtMenuImportIfc|CImportIfcMenu" \
  --exclude-dir=.git .
```

## ローカルでのビルド

CMake 3.20+ と、対象プラットフォームの **Vectorworks 2026 SDK** が必要です。SDK は
`VW_SDK_DIR` を **`SDKLib` を含むフォルダ**に向けて渡します（`-DVW_SDK_DIR=...` また
は環境変数）。

### macOS

Xcode（Vectorworks 2026 は公式に **Xcode 16.2** を対象）と **mac SDK** が必要です。

1. SDK をダウンロードして展開します:
   <https://release.vectorworks.net/latest/Vectorworks/2026-NNA-eng-mac-SDK.zip>
   （約 800 MB）。展開すると `SDKLib/` を含むフォルダができます。

2. コンフィグとビルド:

   ```sh
   cmake -S . -B build -DVW_SDK_DIR=/path/to/2026-NNA-eng-mac-SDK
   cmake --build build --config Release
   ```

   成果物は `build/min-nano_structure.vwlibrary` です。

既定では Apple Silicon（`arm64`）向けにビルドします。ユニバーサルバイナリにするには:

```sh
cmake -S . -B build -DVW_SDK_DIR=/path/to/sdk \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

### Windows

Visual Studio 2022（v143 ツールセット、x64）と **win SDK** が必要です。

1. SDK をダウンロードして展開します:
   <https://release.vectorworks.net/latest/Vectorworks/2026-NNA-eng-win-SDK.zip>
   展開すると `SDKLib/` を含むフォルダができます。

2. コンフィグとビルド（既定の Visual Studio ジェネレータを使います）:

   ```pwsh
   cmake -S . -B build -A x64 -DVW_SDK_DIR=C:/path/to/2026-NNA-eng-win-SDK
   cmake --build build --config Release
   ```

   成果物は `build/Release/min-nano_structure.vlb`（DLL）と、その隣の
   `build/Release/min-nano_structure.vwr`（リソース）です。ビルドスタンプの
   `min-nano_structure.commit` / `min-nano_structure.branch`（どのコミット・どのブランチの
   ビルドか）と更新スクリプト `vw-update.ps1` も同じ場所に出力されます。

macOS の `.vwlibrary` バンドルと違い、Windows のプラグインは `<name>.vlb` 本体と
同名の `<name>.vwr` を**同じフォルダに一緒に**置く必要があります（`.commit` / `.branch` /
`.shell-id` と `vw-update.ps1` も同梱すると自動アップデートが機能します）。

> **アーキテクチャは x64 のみ（ARM も x64 でカバー）**
> Vectorworks の Windows 版は x64 アプリで、SDK も **x64 ライブラリのみ**を同梱して
> います（`LibWin` に ARM64 版はありません）。プラグイン DLL はホストプロセスと同じ
> アーキテクチャでないとロードされないため、ビルド対象は **x64 一択**です（`-A x64`）。
> これは **Windows on ARM でもそのまま動きます** — その環境では x64 版 Vectorworks が
> OS の x64 エミュレーション上で動作し、この x64 プラグインをそのまま読み込みます
> （ネイティブ ARM64 プラグインはエミュレート中の x64 ホストにロードできず、そもそも
> リンクもできません）。したがって ARM 向けの別ビルドは不要です。macOS 側で
> `arm64`／ユニバーサルにできるのは、Vectorworks Mac がネイティブ Apple Silicon
> アプリだからです。


## テストとカバレッジ

テストはすべて **Vectorworks SDK 無し**で走ります（SDK は約 800 MB のダウンロードを
伴うため）。外部依存のない極小のテストハーネス（`tests/TestFramework.h`）を使うので、
テストフレームワークのダウンロードも不要です。対象は 2 系統あります。

- **インポート機能の解析側**（`src/core/` + `src/parse/`）… 2 フェーズ分離により SDK に
  触れないので、実際のホームズ君 IFC（`tests/fixtures/`）に対して要素ごとに単体テスト
  します（STEP リーダ・幾何・通り芯・ストーリ・床・垂木・野地板・構造クラス判定…）。
  描画側（`src/draw/`）は SDK と実図面を要するため単体テストを持たず、実機での目視確認に
  委ねます（確認の作法は [`DEV-NOTES.md`](DEV-NOTES.md)「実機確認の作法」）。
- **アップデータ**（`src/Updater.cpp`）… SDK に依存しない純粋なロジック（スクリプト出力の
  パース、コマンドラインのクォート、インストール先パスの導出）を `src/UpdaterParse.h` に
  切り出し、更新フロー本体は `IUpdaterHost` のフェイク越しに丸ごと動かします。同梱
  スクリプトのバックエンド（`q-stable` / `q-dev` / `do-install`）も、ネットワーク境界だけを
  差し替えて SDK ／ネットワーク抜きにテストします — macOS 版 `scripts/vw-update.sh` は
  `tests/vw-update.test.sh`（bash＋`curl`/`plutil` スタブ）、**配置を担うインストーラ**
  `scripts/vw-install.sh` は `tests/vw-install.test.sh`、**取り除くアンインストーラ**
  `scripts/vw-uninstall.sh` は `tests/vw-uninstall.test.sh`、Windows 版はそれぞれ
  `tests/vw-update.Tests.ps1` / `tests/vw-install.Tests.ps1` /
  `tests/vw-uninstall.Tests.ps1`（PowerShell 7＋
  `Invoke-GH`/`Invoke-WebRequest` スタブ）で、いずれも Linux ランナー上で動きます。
  更新の流れそのもの（尋ねる・入れる・再起動を尋ねる／本体だけ読み直す）は
  `IUpdaterHost` の偽物を差し込んで `tests/UpdaterFlowTests.cpp` で検証します——
  **手動確認と取り込み時確認の違い**（最新・オフラインを伝えるか黙るか）もここです。

**テストの一覧・方針・何をテストしていないかは `tests/README.md`** に詳しくあります。

ローカルでの実行（SDK 不要）:

```bash
cmake -S . -B build-tests -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON
cmake --build build-tests --parallel "$(nproc)"
ctest --test-dir build-tests --output-on-failure -j "$(nproc)"
```

`--parallel` / `-j` を付けないと、cmake も ctest も**既定で 1 コアしか使いません**。
テストは 20 本以上の独立した実行ファイルなので、コア数を渡すだけで素直に短くなります
（CI の `test` ジョブも同じ指定で回します。下記「カバレッジレポート」）。

サニタイザ（AddressSanitizer + UBSan）を有効にして回す（メモリ不正・未定義動作の検出）:

```bash
cmake -S . -B build-san -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON -DVW_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel "$(nproc)"
ctest --test-dir build-san --output-on-failure -j "$(nproc)"
```

CI の `test` ジョブは常にこの設定（に `VW_ENABLE_COVERAGE=ON` を足したもの）で
テストを回すため、リファクタが招くメモリ不正
（境界外アクセス・use-after-free・リーク）や、updater パーサが GitHub 側の仕様変更で
崩れた入力を誤処理するケースは、その場でビルドを赤にできます。予期しない外部入力に
対する耐性は `tests/UpdaterRobustnessTests.cpp` の擬似ファズ／敵対的入力テストが担い、
サニタイザがその番人になります（詳細は `tests/README.md`）。

ビルドオプション:

- `VW_BUILD_PLUGIN`（既定 `ON`）… プラグイン本体をビルドします（SDK が必要で、
  macOS / Windows のみ）。テストだけをビルドしたいときは `OFF` にします。
- `VW_BUILD_TESTS`（既定 `OFF`）… ユニットテストをビルドします。
- `VW_ENABLE_COVERAGE`（既定 `OFF`）… テストに gcov 用の計測を付けます（GCC / Clang）。
- `VW_ENABLE_SANITIZERS`（既定 `OFF`）… テストを ASan + UBSan
  （`-fsanitize=address,undefined -fno-sanitize-recover=all`）でビルド・実行します
  （GCC / Clang）。

### カバレッジレポート（GitHub Actions で内製）

`.github/workflows/test.yml` は、テストを Linux ランナー（SDK のダウンロード不要
なので高速）で実行する **`test` ジョブ**と、それに続く **`coverage` ジョブ**の 2 つに
分かれています。分担は**「計測」と「レポート」**で、**テストのビルドと実行は `test`
ジョブでの 1 回だけ**です。

- **`test`** … ASan + UBSan **と gcov 計測を同時に有効にして**ビルドし、テストを
  1 回実行します。この 1 回の実行が、正しさの判定（テスト失敗・サニタイザ検出）と
  カバレッジデータ（`.gcda`）の生成を兼ねます。ビルドもテスト実行も**ランナーの
  全コア**を使います（`--parallel "$(nproc)"` / `ctest -j "$(nproc)"`）。続けて
  `gcovr` で **Cobertura 形式**のレポート（`coverage.xml`）と集計 JSON を生成し、
  アーティファクト `coverage-report` として `coverage` ジョブへ渡します。
- **`coverage`** … `test` の成功後にのみ実行され、**コンパイルもテスト実行もしません**。
  受け取った計測結果に `diff-cover` で差分カバレッジを加え、表を PR コメントとして
  投稿し、しきい値を判定します。

`test` の失敗はテスト自体の失敗（サニタイザ検出・計測の失敗を含む）を、`coverage` の
失敗はレポート生成またはしきい値割れを意味するので、原因を切り分けやすいという性質は
そのままです。

**並列実行とカバレッジ**: テストを `-j` で同時に走らせても、計測結果は逐次実行と
**完全に一致**します。共有されている状態は、どのテスト実行ファイルもリンクしている
`MinNanoStructureCore` の `.gcda` カウンタだけで、libgcov はそこへ書き戻すときファイルを
ロックするためです（この PR で逐次実行と付き合わせて確認済み: 行 4883/4994・
分岐 4346/6300・関数 418/418 が両者で完全一致）。テスト実行ファイル同士は独立した
プロセスなので、それ以外に共有するものはありません。

サニタイザと gcov は同じ GCC のフラグとして共存できるため、以前のように
「サニタイザ付きのビルド＋実行」と「カバレッジ付きのビルド＋実行」を 2 回行う必要は
ありません。唯一の注意点は無害なもので、サニタイザが異常を検出して abort した場合は
`.gcda` が書き出されません——つまり計測が完全なのはテストが通ったときだけですが、
`coverage` ジョブが走るのはまさにその場合だけです。

なお、サニタイザの計装が入る分、`gcovr` が数える行・分岐の**総数**がごくわずかに
増えるため、割合もわずかに動きます。同一コード（#42 時点）に対する CI の実測では、
行 98.1%（4379/4463）→ 98.0%（4393/4484）、分岐 70.1% → 69.0%、関数は 100% のまま
でした（総数はコードが増えれば動くので、比較の意味があるのは前後の差だけです）。しきい値（行 🟢≥95%）に対しては十分な余裕があり、分岐は参考扱い（⚪）です。

カバレッジの可視化は **GitHub Actions だけで完結**しており、GitHub Code Quality など
外部サービスには依存しません。レポートは次の 2 つの情報を **1 つの表**にまとめた
**1 つの PR コメント**として投稿され（2 回目以降は同じコメントを自動更新）、常に
ビルド成果物としても保存されます。

- **全体カバレッジ（`src/`）**… `gcovr` の集計（行・分岐・関数）。
- **差分カバレッジ**… `diff-cover` により、この PR が変更した行のみをベースブランチ
  （マージベース）と比較したカバレッジ。新しく追加・変更したコードがテストされて
  いるかがひと目で分かります。表の最終行に集計値だけを取り込みます（`diff-cover` の
  Markdown 出力はそのまま貼らず、JSON 出力から値を抽出）。

各行は絵文字で状態を色分けします（🟢 良好 / 🟡 注意 / 🔴 低 / ⚪ 参考）。しきい値は
行 🟢≥95% 🟡≥90%、関数 🟢100%、差分 🟢100% 🟡≥95%、分岐は参考扱い（⚪）で、
ワークフロー内の `THRESHOLDS` に一箇所でまとめており、変更できます。**🔴 が 1 つでも
あると `coverage` ジョブは失敗します**（表示だけでなく CI でしきい値を強制）。この
判定はすべてのイベントで走るため、`main` への push やフォーク PR（コメントはスキップ
されますが判定は有効）でも同様に適用されます。

コメントの投稿には `pull-requests: write` 権限が必要で、トークンが読み取り専用となる
フォーク PR ではスキップされます（レポートはアーティファクトとしては常に保存されます）。
`main` への push では比較対象の差分がないため、差分カバレッジとコメント投稿は行わず、
レポートの生成・アーティファクト保存・しきい値判定を行います。

`coverage` ジョブのチェックアウトは `fetch-depth: 0`（全履歴）です。差分カバレッジには
ベースブランチとのマージベースがローカルに必要で、浅いチェックアウトから段階的に
deepen する方法は履歴の少ないリポジトリでマージベースに届かず `diff-cover` が
「no merge base」で落ちることがあるためです。このリポジトリの履歴は小さいので、
全履歴を取る追加コストは無視できます。

ローカルでは同じレポートを次のように再現できます（CI と違いサニタイザは付けていません。
差分カバレッジはベースブランチを指定）:

```bash
cmake -S . -B build -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON -DVW_ENABLE_COVERAGE=ON
cmake --build build
ctest --test-dir build --output-on-failure
gcovr --root . --filter 'src/.*' build --cobertura coverage.xml --txt --print-summary
# 差分カバレッジ（例: origin/main と比較）
diff-cover coverage.xml --compare-branch origin/main --markdown-report diff-cover.md
```

## 継続的インテグレーション（CI）

`.github/workflows/build.yml` がプラグインをビルドします。`main` は保護された
デフォルトブランチで、機能開発は必ず PR 上で行うため、ブランチを二重にビルドしない
ようトリガを分けています。

- **`main` への push**（マージ）は **stable** リリースをビルドして公開します。
- **PR** はそのブランチをビルドして **dev** プレリリースを公開します。

ワークフローの内容:

- **4 つの並行ジョブ**を持ちます。ビルドの `build-mac`（`macos-15`・Apple Silicon、
  Xcode 16.2）と `build-windows`（`windows-latest`・Visual Studio 2022）、および
  静的解析の `tidy-mac` / `tidy-windows` です。4 つとも**同時**に走り、互いを待ちません
  （clang-tidy はビルドより時間がかかるので、ビルドの中に置かず並走させています。
  詳細は下記「SDK 依存コードの静的解析」）。
- SDK は一度だけダウンロードし、（トリミングした）SDK を**キャッシュ**するので、大きな
  zip は以降の実行で再ダウンロードされません。強制的に再ダウンロードするにはワーク
  フロー内の `VW_SDK_CACHE_KEY`（プラットフォームごとに 1 つ）を変更します。SDK を
  用意する手順そのものは `scripts/fetch-vw-sdk.sh` に 1 つだけあり、4 ジョブと
  `ci-debug.yml` が共有します（キャッシュがヒットしていれば検証だけして抜けます）。
- 各ジョブは**その実行が公開するチャンネルだけ**をビルドします（`-DVW_BUILD_CHANNEL`。
  `main` は `min-nano_structure`、PR は `min-nano_structureDev`）。コミットで刻印
  （`-DVW_BUILD_VERSION`）して成果物を確認・アップロードします（macOS はさらにアドホック
  署名）。PR ではエフェメラルなマージコミットではなく、PR の **head** コミット（あなたが
  push したもの）をビルドします。
- **ダウンロード可能なリリースを公開**し、アップデータが取得できる安定した URL を用意
  します。1 つのリリースに **macOS と Windows 両方のアセット**が入ります:
  - `main` はローリングな **`stable`** リリースを更新します
    （`min-nano_structure.vwlibrary.zip` + `min-nano_structure.vlb.zip`）。
  - PR はブランチごとの **`dev-<branch>`** プレリリースを更新します
    （`min-nano_structureDev.vwlibrary.zip` + `min-nano_structureDev.vlb.zip`。トークンで公開でき
    ないフォーク PR では `release` ジョブごとスキップされます）。

  リリースの公開は独立した **`release` ジョブ**が担当します。このジョブは 4 つのジョブ
  （`build-mac` / `build-windows` / `tidy-mac` / `tidy-windows`）が**すべて**完了してから
  走り（`needs: [build-mac, build-windows, tidy-mac, tidy-windows]`）、両ビルドジョブが
  アップロードした成果物をまとめてダウンロード
  し、**macOS と Windows 両方のアセットを 1 つのリリースに**添付して公開します。公開を
  ビルドから切り出したことで、どちらのプラットフォームも単独でリリースを作らなくなり、
  作成とアタッチが競合することがありません。静的解析ジョブも `needs` に入っているので、
  **ビルドが通っていても clang-tidy が通らなければリリースは公開されません**（解析は
  ビルドと並走しているため、このゲートを保っても所要時間は増えません）。どちらもローリング方式で、毎回タグを最新
  ビルドに貼り直します。**stable** の公開は GitHub API の長時間障害があってもリトライ
  します（stable リリースの取りこぼしは気づかれにくいため）。**dev** の公開はリトライ
  しません — dev ビルドはブランチ作業中にしか使わないので、一時的なエラーが出たら
  ジョブを再実行すれば十分です。

  公開に至らない実行 — トークンでリリースを作れない**フォーク PR** や、`main` 以外の
  ref での `workflow_dispatch` — では、**`release` ジョブ自体が起動しません**（ジョブの
  `if` で振り分けているので、成果物のダウンロードも走りません）。スキップされたジョブの
  チェック結論は `skipped` で失敗ではないため、ブランチ保護や `ci-wait` の判定には
  影響しません。

`.github/workflows/cleanup-dev-release.yml` は、**PR がクローズされたとき**（マージの
有無を問わない）にその `dev-<branch>` プレリリース（とタグ）を削除し、dev ビルドが
溜まらないようにします。プレリリースは**PR が開いているあいだの成果物**（公開するのは
`build.yml` の `pull_request` 実行だけ）なので、PR が閉じた時点が役目の終わりです。
以前はブランチ削除（`delete` イベント）を合図にしていましたが、それだと**ブランチを
残したままマージした PR**や**マージせず閉じた PR**のプレリリースが残り続け、逆に PR を
持たないブランチの削除でも起動していました。

`pull_request` の `closed` で起動するため、ワークフローの実体は PR のマージ ref
（head を base にマージしたもの）側のコピーから実行されます。したがってこの変更が
`main` に入って初めて有効になり、それ以前から開いている PR も、マージ ref が新しい
`main` に対して作り直された時点でこの版を拾います。フォークからの PR は
（そもそも `build.yml` がプレリリースを公開しないので）ジョブの `if` で除外します。

これと対になる取り決めが `build.yml` 側にもあります。ビルドの実行中に PR が閉じられると、
片付けの側は「まだ公開されていないプレリリース」を探して空振りするので、その後にビルドが
公開すると**誰も消さないプレリリース**が残ります。これを避けるため、dev の公開ステップは
**PR がまだ open か**を公開の前後で確かめます（以前はブランチの存在を見ていました）。

`.github/workflows/stable-release-healthcheck.yml` はスケジュール（6 時間ごと）で
安全網として実行されます。公開済みの `stable` リリースが `main` の先頭からずれている
場合 — つまり stable の公開を取りこぼした場合 — `main` で `build.yml` を再ディスパッチ
して再ビルド・再公開します。スケジュール起動のワークフローはデフォルトブランチから
実行されるため、`main` にマージされて初めて有効になります。

### 自動レビュー（`pr-review.yml`）

`.github/workflows/pr-review.yml` は、PR を **Claude にレビューさせて**指摘を
インラインコメントとレビューとして投稿します。公式の
[`anthropics/claude-code-action`](https://github.com/anthropics/claude-code-action) を
自動レビュー向けの形（agent モード）で使い、認証はサブスクリプションの OAuth トークン
（リポジトリシークレット `CLAUDE_CODE_OAUTH_TOKEN`。`claude setup-token` で発行）で行い
ます。**シークレットが未設定なら警告を出して素通りする**ので、設定しないままでも CI は
赤くなりません。

**役割は他のチェックと重なりません。** 書式と機械的な静的解析は `lint.yml` /
`build.yml` の clang-tidy / `codeql.yml` が済ませているので、こちらが見るのは
**機械が見られないもの** — [`CLAUDE.md`](../CLAUDE.md) の設計規約（2 フェーズ分離・
殻と本体の分割・依存の向き・図面リソースを作らないこと・決定性・置き場所の重複・
利用者のものを消すコードの歯止め・書き残しの行き先）と、変更の意図に照らした正しさです。
プロンプトにはその一覧が入っていて、レビューは日本語・重大度つきで返ります。

起動と安全のための取り決め:

- **入口は「CI が緑になったとき」と「コメントが付いたとき」の 2 つです。** コードの変更は
  `pull_request` ではなく **`workflow_run`**（`Lint` / `Build plug-in` / `Tests` の完了）で
  受けます。`pull_request` で受けるとこの 3 つと**同時に**始まってしまい、結末を見に行っても
  まだ走っている最中なので、下の門が素通りするためです。`pull_request` に残しているのは
  **CI が動かないのにレビューが要る**遷移だけ（`reopened` / `ready_for_review`）で、
  `opened` / `synchronize` は CI が動く＝`workflow_run` が来るので要らず、`edited`（題と
  本文）は CI とも差分とも関係がないので落としました。コメントとレビューでも走らせるのは、
  指摘への対応がコードではなく説明で行われることがあるためです。
- **`lint` / `build` / `test` が 3 つとも `success` でなければ走りません。** 失敗だけでなく
  「まだ終わっていない」でも、Actions の API を引けなかったときも止めます（分からないときは
  走らせない側へ倒します）。`workflow_run` は 3 つそれぞれの完了で呼ばれるので、実際に走るのは
  **最後の 1 つが終わった 1 回だけ**になります。機械が見つけるものを直せば差分は動くので、
  その前に規約のレビューを出しても出し直すだけになる、というのが理由です。

  見るのは**その PR の現在の head に対する最新の実行の結末**で、チェック名は見ません
  （ジョブを足しても改名してもここは直さなくて済みます）。種別は `event=pull_request` に
  絞ります — `build.yml` には `workflow_dispatch` も残っているので、絞らないと同じ sha の
  別起点の実行を拾いえます。この照会のために `permissions` に **`actions: read`** を
  足してあります。
- **`workflow_run` と PR 本体へのコメント（`issue_comment`）からの起動は、このワークフローが
  `main` にマージされて初めて効きます。** どちらも**デフォルトブランチにある版のワークフローが
  実行される**ためで、`cleanup-dev-release.yml` と同じ制約です（下記）。つまり**コードの変更を
  拾う主経路は、作業ブランチの段階では試せません**。インラインコメントへの返信
  （`pull_request_review_comment`）、レビューの提出（`pull_request_review`）、そして
  `pull_request`（`reopened` / `ready_for_review`）は **PR の head の版が走る**ので、
  こちらは作業ブランチから効きます（PR #112 で実測）。

  副作用として、**`workflow_run` 起点の実行は既定ブランチ側に紐づくため、`review` のチェックは
  PR の一覧に出ません**。ジョブは `continue-on-error` なので、もともと門ではありません。
- **打ち切りは `workflow_run` の側だけ**にしています。3 つのうち 2 つがほぼ同時に終わると、
  どちらの実行も「3 つとも success」を見て走り出しうるので、後から来たほうで前を打ち切って
  レビューが二重に出るのを防ぎます。**`pull_request` やコメントが起点のときは打ち切りません**
  — 走っている実行を打ち切るとそのチェックは `cancelled` になり、`scripts/ci-wait.sh` は
  それを**失敗として扱う**ので（下記「CI の完了待ち」）、レビューを止めただけで PR が赤く
  見えます。`workflow_run` の実行は既定ブランチ側に紐づく＝PR の head にチェックが付かない
  ので、そちらでは打ち切ってよいわけです。

  **同時実行の鍵はイベントで変えてあり**（コメント起点は PR 番号、`workflow_run` はその sha。
  `workflow_run` のイベントには PR 番号が載らず、fork では `pull_requests` が空になるため）、
  互いを打ち切らないので、**同じ PR で 2 つ走ることはありえます**。打ち切ると溜めた
  インラインコメントごと消えるため（投稿はセッション終了時のバッファ投稿）、そちらを選んで
  います。

  なお `cancel-in-progress` が false でも、**順番待ち（pending）の実行は、次の実行が来た
  時点で GitHub 自身が打ち切ります**（1 グループに待てるのは 1 つだけ）。ただしその場合は
  **ジョブが始まっていないのでチェックランが作られず**、`ci-wait` の判定には現れません
  （PR #112 で実測。`pull_request_review` 起点の実行が pending のまま打ち切られたが、
  `ci-wait` は `success` を返し `review` は 2 件とも成功だった）。
- **bot の投稿では走りません。** 自分のレビューが次の実行を呼んで際限なく回るのを
  防ぐためで、`test.yml` のカバレッジ表や `ci-debug.yml` の結果コメントもここで落ちます。
- **実機フィードバックの自動コメント**（`<!-- homeskz-ifc-feedback … -->`）でも走りません。
  あれは件数と診断ログで、人は 1 文字も書いていません（下記「実機フィードバックの往復」）。
  拾うと dev ビルドが出るたびにレビューが二重に走ります。
- コメント・レビューが起点のときは、**書き込み権限のある人の投稿だけ**を引き金にします。
  権限は実際に API で引き（`repos/{owner}/{repo}/collaborators/{user}/permission`）、
  引けなかったときだけ `author_association` に落とします。その受け皿が通すのは
  **`OWNER` と `MEMBER` だけ**です — **`COLLABORATOR` は通しません**。読み取り専用の
  collaborator も association は `COLLABORATOR` になるので、通せば「書き込める人だけ」
  という前提が受け皿の側から崩れます。API が引けなかった理由は `::warning::` に出るので、
  この受け皿が例外なのか毎回通る道なのかは実行ログで分かります（GitHub のドキュメントは
  このエンドポイントに admin 権限を求めており、ジョブの `GITHUB_TOKEN` では常に失敗する
  可能性があります）。
- コメント・レビュー・`workflow_run` が起点のときはシークレットの渡る特権的な文脈なので、
  **PR の head を checkout しません**（既定ブランチのまま、PR の中身は `gh pr diff` で
  読ませます）。入口を `workflow_run` に寄せたので、いまはこちらが**ふだんの経路**です。
  fork の PR と下書きの PR は走りません。
- **下書き（draft）の PR ではレビューしません。** 実機確認や設計判断が要る変更は下書きで
  作り、マージしたい状態になってから ready for review に昇格させます（[`CLAUDE.md`](../CLAUDE.md)
  「開発プロセス: PR とマージ」）。差分がまだ動く段階でレビューを回しても出し直すだけで、
  エージェントの使用量を無駄にするためです。昇格（`ready_for_review`）がそのままレビューの
  入口になります。イベントに PR が載っているもの（`pull_request` / インラインコメント・
  レビューの提出）はジョブの `if` でランナーを起こす前に落とし、載っていないもの
  （`workflow_run` / PR 本体へのコメント）は「レビューするか決める」の `isDraft` で落とします。
  **CI（lint / build / test）と dev ビルドは下書きでも走る**ので、実機フィードバックの往復は
  下書きのまま回せます。
- **レビューの失敗で PR を赤くしません**（`continue-on-error`）。失敗は実行のログと
  `::warning::` に残ります。
- ツールは差分の取得とレビューの提出に要るものだけを許可し、`Edit` / `Write` /
  `NotebookEdit` を明示的に外してあります（レビューがコードを書き換えることはありません）。
  判定を出す `gh pr review` は**その PR の番号まで含めて**許可します — 番号を縛らないと
  「この PR のレビュー」という許可が「どの PR でも承認・非承認できる」許可になり、
  レビューが読む外来の文章（PR の説明文やコメント）に紛れた指示で別の PR を承認させられる
  余地が残ります。読むだけの `gh pr diff` / `gh pr view` は縛りません（番号を間違えても
  他所を読むだけで書き換えは起きず、逆に縛ると引数の並びが変わっただけで拒否されて
  レビューが黙って痩せます）。
- **費用の上限は `--max-turns 40` / `--max-budget-usd 2.0`、思考の深さは `--effort high`**
  です。PR #140（31 ファイル・約 1,650 行）で `xhigh` のまま 37 ターン・$2 に達し、判定も
  インラインコメントも出ずに終わったため、次の 3 つを入れました。
  * **`CLAUDE.md` を読み直させない。** action はプロジェクトの設定（`settingSources` の
    `project`）を読むので、`CLAUDE.md` は最初からシステムプロンプトに入っています。以前の
    プロンプトは「まず読め」と指示しており、約 10 万字が会話にもう一度載って、以後の毎ターンで
    抱えたままになっていました。
  * **ツール呼び出し 25 回で提出させる。** 予算の残りはモデルから見えないので、「打ち切られ
    そうなら提出」は働きません。代わりに数えられる回数で区切り、見ていない部分があれば
    `--comment`（承認しない）で出させます。
  * **結末をステップサマリーに残す**（「実行の結末を残す」）。終わり方・ターン数・費用と、
    **拒否されたツール呼び出しの中身**（道具名と引数の頭 200 字）を出します。action は会話の
    全文を隠すので、ジョブログには拒否の件数しか残らず、許可を足すべきかを判断できなかった
    ためです。

**承認は実機確認の代わりではありません。** `draw/` を含む PR は、レビューが承認しても
人が Vectorworks 実機で見て「確認できた」と言うまでマージしません
（[`CLAUDE.md`](../CLAUDE.md)「開発プロセス: PR とマージ」）。

#### 手動ディスパッチ（`workflow_dispatch`）を持つワークフロー

「Run workflow」ボタンは**必要なものにだけ**付けています。PR とマージで自動的に走る
チェック系（`lint.yml` / `test.yml` / `codeql.yml`）は手動起動する用途が無く、失敗した
実行を回し直したいだけなら Actions 画面の **Re-run** で足りるためディスパッチを持ちません。

| ワークフロー | 手動ディスパッチ | 理由 |
| --- | :---: | --- |
| `build.yml` | あり | `stable-release-healthcheck.yml` が stable の取りこぼしを検知して再ディスパッチする（リリース経路） |
| `stable-release-healthcheck.yml` | あり | 6 時間の次回スケジュールを待たずに stable のずれを直したいとき |
| `ci-debug.yml` | あり（専用） | 手動ディスパッチ**のみ**で起動する調査用ワークフロー |
| `lint.yml` / `test.yml` / `codeql.yml` | なし | push / PR（+ CodeQL は週次スケジュール）で自動的に走る |
| `cleanup-dev-release.yml` | なし | PR のクローズ（`pull_request` の `closed`）専用 |
| `pr-review.yml` | なし | CI の完了（`workflow_run`）とコメント・レビューで自動的に走る |

### CI の完了待ち（`scripts/ci-wait.sh`）

PR やブランチの CI（`build.yml` / `lint.yml` / `test.yml` …）が終わるのを待つ道具です。
対象のチェックが全部終わった**瞬間に exit** し、最終行に結果を出します。リモートセッション
から「CI が終わった」ことを知る手段は、**完了した瞬間に exit するプロセスをバックグラウンドで
走らせる**ことだけです（PR 購読で配信されるのは CI の**失敗**とコメントだけで、**成功は配信
されない**）。バックグラウンドコマンドの終了はハーネスが通知するので、exit がそのまま完了
通知になります。

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
| `failure` | 1 つ以上が失敗・キャンセル・timed_out。**cancelled も失敗扱い**（新しい push で古い run が消えたものを green と取り違えないため） |
| `no-checks` | 猶予（既定 180 秒）を過ぎてもチェックが 1 件も登録されなかった。**「CI が始まってすらいない」を成功と読まない**ための結果 |
| `head-moved` | `--no-follow` 指定時に、待っている間に head が動いた（古い結果は返さない） |
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

### CI デバッグ（`ci-debug.yml`）

`.github/workflows/ci-debug.yml` は、**手動ディスパッチ専用**の「CI 上で 1 コマンドだけ
動かす」ワークフローです。SDK が手元に無い環境（クラウド上の開発セッションなど）から、
**本プラグインのコードが SDK でコンパイルできるか**を確かめるために使います。`push` /
`pull_request` では**決して起動せず**、リリースも公開しません（`contents: write` を持たない）。
SDK キャッシュは `build.yml` と同じキーで**読み取り専用**に復元するので、本番ビルドの
キャッシュを汚しません。

**SDK そのものの調査（「この API は SDK にあるか」「どう振る舞うか」）はここでは行いません。**
[SDK リファレンスリポジトリ](https://github.com/min-nano/vectorworks-developer-sdk-reference)で
issue を立て（テンプレート `調査`。どの機能で・何が分かれば実装に入れるかを書く）、あちらの
調査が `Findings/` に反映されるまでその部分の実装に入りません（その間は他の要素や `parse/`
`core/` の作業を進める）。本リポジトリで `sdk-grep` / `sdk-ls` を使うのは、**既に
`Findings/` に載っている宣言を写し取る**（引数の型や名前を確かめる）ときだけです。

**`build.yml` に一時的な調査ステップを挿してはいけません**——戻し忘れる・その commit が
dev プレリリースとして公開される・ccache / SDK キャッシュを汚す、と副作用が大きいためです。

**使い方。** リモートセッションの `GITHUB_TOKEN` は読み取り専用で `actions: write` を
持たない（REST でのディスパッチは 403）ので、**起動は GitHub MCP、待機はスクリプト**の
2 手順です。

```
1. mcp__github__actions_run_trigger
     method: run_workflow, workflow_id: "ci-debug.yml", ref: <ブランチ>,
     inputs: {mode, platform, label, args, script, notify_pr}
     ※ label は一意な文字列にする（これで run を特定する）

2. Bash(run_in_background: true):
     scripts/ci-debug.sh wait --label <label>
```

手順 2 は「run の特定 → 完了待ち → ペイロード抽出」を行い、完了した瞬間に exit します
（`ci-wait` と同じく `sleep` で待たない）。最終行は必ず
`ci-debug: done (conclusion=<結果> exit=<終了コード>)` で、既定の上限は 45 分（ジョブの
`timeout-minutes` と同じ。`--timeout` / `--poll` で変更可）。`timed-out-waiting` /
`api-error` の意味は `ci-wait` と同じです。待機プロセスを失ったら
`scripts/ci-debug.sh wait --label <label>` で合流でき、確実に追いつきたいときは
`--notify-pr <番号>` で完了時に結果を PR コメントとして投稿させられます。

書き込み権限のあるトークン（PAT など）がある環境では、起動と待機をまとめた
`scripts/ci-debug.sh run --mode build --platform windows` が使えます。

| mode | 用途 | `--args` |
| --- | --- | --- |
| `sdk-grep` | SDK ヘッダを拡張正規表現で検索（`Findings/` に載っている宣言の写し取り用） | 検索パターン |
| `sdk-ls` | ヘッダの全文表示 / パス部分一致の一覧 | ヘッダのパスまたは部分文字列 |
| `build` | configure してビルド（リリース公開はしない） | 単一ターゲット名（省略可） |
| `compile-one` | 1 翻訳単位だけコンパイル（数十秒。Windows 不可） | ソースのパス |
| `shell` | 任意の bash（`--script`）。逃げ道 | — |

`--platform` は `mac`（既定）/ `windows` / `linux`。**`linux` は SDK を用意しない**ので
SDK 非依存コード専用（速い）。`--ref` は既定で現在のブランチ。

**`build` / `compile-one` は本番 CI の代わりになりません。** どちらも clang-tidy を通さずに
コンパイルするだけなので、`build-mac` / `build-windows` が落とす lint（例:
`readability-uppercase-literal-suffix`）は素通りします。「ci-debug の build が通ったから CI も
通る」と報告しないこと。SDK 依存コードの最終確認は PR の CI が緑になったことで行います。

**結果の読み方。** 出力は必ず次のマーカーで挟まれます。`truncated=yes` なら全部は見えて
いないので、`--args` を絞るか `mode=shell` で件数を数えてください。

```
===== BEGIN PAYLOAD (mode=... platform=...) =====
...
===== END PAYLOAD (exit=N lines_total=N truncated=yes|no) =====
```

`... (annotation truncated by GitHub's 4096-char limit …)` が END の直前に出ていたら、
注釈経路の上限で切られています（END の `lines_total` が本当の行数）。マーカーが無ければ
調査コマンドに到達せずに失敗しており、代わりに理由が出ます。全文はジョブログと
アーティファクト（`ci-debug-<label>`）にありますが、**AI はアーティファクトを取得できない**ので、
モードを足すときは必要な情報を必ずログ側に出してください。

ペイロードの取得経路は 2 つで、`ci-debug.sh` はこの順に試します。

1. **チェックラン注釈**（`GET /repos/{owner}/{repo}/check-runs/{id}/annotations`）。
   `ci-debug-job.sh` がペイロードを `::notice::` としても出しているので、通常はここで取れます
   （`api.github.com` だけで完結し、ログのノイズも混ざらない）。
2. **ジョブログ**。ログ API は署名付きの Azure Blob Storage へ 302 で飛びますが、そのホストは
   組織の egress ポリシーで拒否されている（`curl: (56) CONNECT tunnel failed, response 403`）
   ので、コンテナからは取れません。**迂回してはならない制約**なので、必要なときは GitHub MCP の
   `get_job_logs`（`job_id` 指定・`return_content: true`）を使います（全ログが文脈に入るので、
   注釈で足りるならそちらで済ませる）。

**制約。** `workflow_dispatch` は**デフォルトブランチに存在するワークフロー**しか起動できません。
**モードの追加・修正は `scripts/ci-debug-job.sh`（ランナー側）で行います**——ワークフロー本体は
薄く保ってあるので、作業ブランチに push するだけで新しいモードを試せます（`--ref` がその
ブランチのため）。ワークフロー本体を変えると main へのマージが要ります。

## コーディング規則の強制（Lint）

`.github/workflows/lint.yml` が、`main` への push とすべての PR で
コーディング規則を機械的に強制します。対象は C/C++ ソースにとどまらず、
**CMake ビルドファイル・GitHub ワークフロー・シェルスクリプト、そしてすべての
テキストファイルの空白／文字コード**まで、それぞれ専用のフォーマッタ／リンタで
チェックします。原因を切り分けやすいよう、チェックごとに独立したジョブに
分かれています。

C/C++ を対象とするジョブ:

- **`clang-format`** — `src/` と `tests/` の**すべての** C/C++ ソースを
  `.clang-format` に照らしてチェックします。`--dry-run --Werror` なので 1 か所
  でも規則から外れると失敗し、書き換えは行いません。SDK もビルドも不要なので
  高速で、SDK が要る（`#if GS_MAC` / `GS_WIN`）プラットフォーム固有のグルー
  コードも含めて**全ファイル**を対象にできます。
- **`clang-tidy`** — **SDK 非依存の全翻訳単位**、すなわち 2 フェーズのインポート
  コード（`src/core/*.cpp` / `src/parse/*.cpp`）と アップデータロジック
  （`src/UpdaterFlow.cpp`。取り込む `UpdaterParse.h` / `UpdaterHost.h` も
  `HeaderFilterRegex` で対象）に対して静的解析を行います。clang-tidy は翻訳単位を
  実際にコンパイルする必要があり、SDK が不要なこの Linux ランナー上では**実ロジックを
  持つ SDK 非依存コード**を対象にします。`core/` `parse/` はハードコードした一覧では
  なく**グロブ**で拾うので、新しい parse モジュールを足した瞬間から対象になります
  （`scripts/lint.sh` も同一の一覧を使います）。`.clang-tidy` は
  `WarningsAsErrors: "*"` なので、検出があれば CI が失敗します。

ソース以外のファイルを対象とするジョブ:

- **`cmake-format`** — `CMakeLists.txt` 群の整形を `.cmake-format.yaml`
  （タブ・100 桁・コメントは再整形しない）に照らして `--check` します。あわせて
  **`cmake-lint`** が CMake のバグを招きやすいパターンを検出します。
- **`actionlint`** — ワークフロー YAML 自体（構文・式・参照アクション・ランナー
  ラベル）を検証し、同梱の **shellcheck** で各 `run:` のインラインスクリプトも
  静的解析します。
- **`shellcheck`** — `scripts/` 配下のスタンドアロンなシェルスクリプトを解析
  します（ワークフロー内のインラインスクリプトは actionlint が担当）。
- **`PSScriptAnalyzer`** — Windows 版アップデータとインストーラ（`scripts/*.ps1`）の
  PowerShell 静的解析です。未承認の動詞・未使用パラメータ・危険な null 比較など
  バグを招きやすいパターンを検出します。clang-tidy（`src/` の実ロジックのみ）や
  shellcheck（`scripts/*.sh` のみ）と同じく、テストハーネス（`tests/`）ではなく
  `scripts/` 配下の**本番スクリプト**を対象にします。ルールは
  `PSScriptAnalyzerSettings.psd1`（デフォルト全ルールから、このスクリプトの意図的な
  設計と衝突する数個だけを除外）で管理し、残った検出はすべて CI を失敗させます。
- **`yamllint`** — ワークフローや Dependabot 設定など YAML の構造スタイル
  （インデント・キー重複・記号まわりの空白）を `.yamllint.yaml` に照らして
  チェックします。
- **`editorconfig-checker`** — **すべての**テキストファイルについて、末尾改行・
  行末空白なし・UTF-8・LF を強制します（フォーマッタがカバーしない衛生面）。
  対象規則は `.editorconfig`、実行する検査と除外は `.editorconfig-checker.json`
  で設定します。インデントは各フォーマッタ（clang-format / cmake-format）が
  タブ＋スペース整列で管理するため、この検査ではあえて無効化しています。

**SDK 依存コードの静的解析（`build.yml` の `tidy-mac` / `tidy-windows`）** — 同じ
`.clang-tidy` ルールを、SDK がないとコンパイルできない側（`src/draw/*.cpp` と
`ModuleMain.cpp` / `Extensions/ExtMenu.cpp` / `Updater.cpp`）にも適用します。
`src/draw/` はグロブで拾うため、要素を追加しても対象漏れが起きません
（`core/` `parse/` を `lint.yml` がグロブで拾うのと同じ理屈）。

- **`tidy-mac`** — `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` で生成した compile
  database に対して clang-tidy を実行し、`#if GS_MAC` 側の分岐を解析します。
- **`tidy-windows`** — Visual Studio ジェネレータは compile database を出力しない
  ため、解析専用に **Ninja + clang-cl** でビルドせずに再コンフィグして database を
  生成し、`#if GS_WIN` 側の分岐（`Updater.cpp` の `Widen` / `Narrow` /
  `OwnModulePath` / `RunBundledScript`）を解析します。

macOS が `GS_MAC`、Windows が `GS_WIN` の分岐をそれぞれ担当するので、両者を合わせて
**すべての行**が clang-tidy でチェックされます。

**なぜ独立したジョブなのか（速度）** — clang-tidy はビルドを走らせずに解析するので
PCH が使えず（`VW_ENABLE_PCH`）、1 翻訳単位ごとに SDK のアンブレラヘッダを丸ごと
読み直すため 1 本あたり 30〜40 秒かかります。当初はこれをビルドジョブの中で 15 本
直列に回していて、**Windows ジョブの 12 分 41 秒のうち 10 分 10 秒**（mac は 7 分 44 秒の
うち 6 分 40 秒）がこの 1 ステップでした。そこで 4 段階で速くしています。

1. **翻訳単位をランナーのコア数ぶん並列に解析する**（Windows 4 コア・mac 3 コア）。
   対象一覧と並列実行は **`scripts/clang-tidy-sdk.sh`** が持ち、両ジョブが同じ
   スクリプトを呼ぶので一覧は 1 か所にしかありません。
2. **ビルドと並走させる**。解析をビルドジョブから独立したジョブへ出したことで、
   ビルドの後ろに積まれなくなりました。
3. **翻訳単位を複数のランナーへ分ける**（`strategy.matrix.shard`）。1 台の中の並列は
   コア数で頭打ちになり、実測で 4 コアのランナーは 4 並列でも 2.5 倍程度しか出ません
   （並列効率 63%）。そこから先を縮めるにはランナーを増やすしかないので、
   `clang-tidy-sdk.sh -s I/N` で対象そのものを分割します。割り当てはラウンドロビンで、
   **全シャードの和がちょうど元の一覧**になります（重複も漏れもありません）。
   分割数を変えるのは `matrix.shard` のリストを 1 か所いじるだけです
   （`strategy.job-total` がそのままスクリプトへ渡ります）。`fail-fast: false` なので、
   片方のシャードで検出が出てももう片方は最後まで走ります — 1 つ直すたびに次が
   出てくる、という進み方を避けるためです。
4. **前と同じ入力の翻訳単位は解析ごと飛ばす**（`clang-tidy-sdk.sh -c`）。下記
   「clang-tidy の結果キャッシュ」。

#### clang-tidy の結果キャッシュ（`-c`）

1〜3 を入れてもなお、**PR の待ち時間を決めているのはこの解析**でした（実測 2026-09、
41 翻訳単位・2 分割: `build-mac` 1 分 50 秒 / `build-windows` 2 分 7 秒に対して
`tidy-windows` は 6 分 55 秒、`tidy-mac` は 3 分 51 秒。ワークフロー全体が 7 分 15 秒）。
一方で 1 コミットが実際に触るのはたいてい数ファイルです。**コンパイルの側はこれを
ccache が「入力が同じなら結果を使い回す」で解いています**が、clang-tidy には同じものが
ありません——出力がオブジェクトではなく診断なので、ccache は面倒を見てくれないからです。

そこで同じ原理を自前で持ちます。翻訳単位の入力すべてを 1 つの鍵にまとめ
（**`scripts/tidy-cache-key.py`**）、**「この入力では診断が 1 つも出なかった」という事実
だけ**を控えます。次の実行で同じ鍵が出た翻訳単位は、解析そのものを飛ばせます。控えは
`actions/cache` で実行をまたいで持ち回ります（キーはシャードごと・チャンネルごと）。

**鍵に入るもの**——ここに漏れがあると「直したのに緑」という、**誰にも見えない失敗**に
なります。

| 入力 | 鍵への入り方 |
| --- | --- |
| その `.cpp` が**推移的に** include する `src/` 配下のファイル | 中身の SHA-256 |
| コンパイル指令（フラグ・定義。`VW_DEV_BUILD` も `VW_SHELL_ID` もここに現れる） | `compile_commands.json` の該当エントリ |
| clang-tidy の版 | `--version` から取り出した**版の番号だけ**（下記） |
| 規則 | `.clang-tidy` の中身（ソースの位置から根まで遡って全部） |
| SDK の同一性 | `VW_SDK_CACHE_KEY`（SDK ヘッダは膨大なので、**どの SDK を取ってきたかを決めている鍵**で代表させる。SDK を差し替えるときは必ずこの鍵を上げる、という既存の約束がそのままキャッシュの正しさを担保します） |
| **ツールチェインの標準ヘッダ** | ランナーイメージの同一性（GitHub が渡す `ImageOS` / `ImageVersion`。下記） |
| clang-tidy へ渡す引数 | 実行ファイルと `-p DB` を除いた**全部**（`--warnings-as-errors='*'` も、将来足す引数も自動で入る） |

安全側への倒し方は 3 つあります。**`#include` の走査は `#if` を評価せず、書かれている
ものをすべて辿ります**（多めに拾うのは「使い回せたのに解析した」だけで無害、取りこぼしは
見落としに直結）。**控えるのはきれいに通ったときだけ**で、診断が出た翻訳単位は控えません。
**入力を突き止められない翻訳単位は鍵を出さず、必ず解析します**——compile database に
無いもの、そして**取り込む行（`#include` / `#import` / `#include_next`）なのに綴りを
読み取れないもの**（マクロ・行継続・途中のコメント）。

ここは「読めない綴りだけを名指しで弾く」書き方にしないでください。名指しにすると、
**挙げ忘れた綴りが「無視」に落ちます**——無視された `#include` は鍵から抜けた依存であり、
それはまさに「直したのに緑」の形です。**取り込む行かどうかを先に判定し、綴りを読み取れ
なければ一律でキャッシュ不可**にします。

同じ理由で、**解決できない `"..."` の include もキャッシュ不可**です。検索パスには
`src/` に加えて**コンパイラに渡っている `-I` / `/I` / `-imsvc` 一式**を入れてあるので、
SDK のヘッダも実ファイルとして解決できます（解決はしますが**辿りません**——SDK は
`--sdk-key` が代表します）。つまり「解決できない」は前提が外れた合図で、鍵生成が
**自分で検算になっている**: パスの扱いが想定と違う環境では解決が総崩れになり、控えを
1 件も書かない＝必ず解析する側へ倒れます。

**「きれいに通った」の判定は終了コードで行います。** ここを「出力が空なら」にしては
いけません——clang-tidy は**成功時にも**「72231 warnings generated.」「Suppressed 72277
warnings (…)」を必ず出すので、その条件は永久に成り立たず、**1 件も控えられないまま緑に
なります**。実機でまさにそうなりました（復元は効いているのに `0 reused / 20 analysed
(0 restored)`）。終了コードで足りるのは、`clang-tidy-sdk.sh` が常に
`--warnings-as-errors='*'` を付けているからです——診断が 1 つでも出れば非ゼロになるので、
`rc == 0` は「言うべきことは何も無かった」と同じ意味になります。

**コンパイラの既定の検索パスから来るヘッダは、走査に映りません。** Xcode の SDK・MSVC の
STL・Windows SDK は `-I` ではなく既定のパスから来るので、上の include 走査は 1 つも
拾いません。これらが入れ替われば診断も変わりうるのに、数万ファイルをハッシュするわけには
いきません。そこで**どのランナーイメージで走っているか**（`ImageOS` / `ImageVersion`）を
鍵に入れ、それらの代表とします。**イメージが更新された実行は全件が外れます**が、それが
正しい挙動です——ヘッダが実際に入れ替わっているのですから。代償はイメージ更新ごとに
1 回 cold になること（実質週 1 回程度）で、手元では両方とも未設定なので空になり、値は
安定します。

**鍵に「実行機ごとに変わるもの」を入れないこと。** `clang-tidy --version` の出力には
`Host CPU: apple-m1` のような行があり、**ランナーの機種によって変わります**（GitHub の
プールは機種が混在しています）。出力を丸ごと鍵に入れると実行のたびに別の鍵になるので、
いまは `LLVM version X.Y.Z` の番号だけを取り出しています。同じ種類の入力を足すときも、
「同じコミットを別のランナーで走らせても同じ値か」で判断してください。

**この仕組みの壊れ方は、いつも「CI は緑のまま、ただ何も速くならない」です。** 結果が
正しいので誰も気付きません。上の 2 つ（控えを 1 件も書かない／鍵が毎回変わる）はどちらも
その形をしていて、前者は実機で実際に起きました。気付けるように、**控えが復元されたのに
1 件も引けなかった実行は `::warning::` を出し**、要約行には**復元できた件数**も出します
（`0 reused / 20 analysed (0 restored)` — この `(0 restored)` が原因の切り分けを決めました）。
規則（`.clang-tidy`）・SDK・**ランナーイメージ**・共有ヘッダ・clang-tidy の版や引数を
変えた実行なら警告が出るのは当然ですが、そうでないのに出ていたら壊れています。とりわけ
**ランナーイメージが更新された週は正当に出ます**——「鍵が壊れた」と読み違えないように。
この並びは `scripts/clang-tidy-sdk.sh` の警告文と同じものなので、**片方を直したらもう
片方も直してください**。

キャッシュはあくまで速さのための飾りで、**無くても結果は 1 ビットも変わりません**
——`python3` が無ければ黙って全部を解析します。この性質と上の表の各行は
`tests/tidy-cache.test.sh`（ctest の `TidyCacheScriptTests`）で回帰テストしてあります
（機種の違いで外れないことも、そのうちの 1 件です）。

**PR の 1 回目は効きません。** 翻訳単位はすべて `VW_DEV_BUILD` の有無でコンパイル指令が
変わるので、dev（PR）の実行が stable（main）の控えを引くことは原理的にありません。
効くのは**同じ PR への 2 回目以降の push**で、そこが実際に待たされている場面です。

**ゲートは緩めていません。** `release` ジョブの `needs` には 2 つのビルドジョブに加えて
`tidy-mac` / `tidy-windows` も入っているので、**ビルドが成功していても clang-tidy が
通らなければリリースは公開されません**。解析はビルドと同時に走っているため、この
ゲートを保っても所要時間は増えません。

解析用の compile database は、**その実行がビルドするチャンネル 1 つ**に絞って生成します
（`-DVW_BUILD_CHANNEL`。PR は `dev`、`main` は `stable`）。既定の `both` のままだと
1 ソースにつき database のエントリが 2 つでき、clang-tidy が同じファイルを 2 回解析して
所要時間が倍になっていました（Windows で約 9 分）。チャンネル間の差は `VW_DEV_BUILD`
の定義だけ（`ModuleMain.cpp` / `Extensions/ExtMenu.cpp` の 3 分岐）で、PR が dev 側、
`main` が stable 側を解析するので、パイプライン全体では両方が解析されます。

バージョンについて: SDK 非依存の `lint.yml` と `tidy-mac` は clang 18 に固定して
います。`tidy-windows` だけは**ランナーイメージに入っている LLVM**（現在 20 系）を
そのまま使います — ランナーの MSVC 標準ライブラリヘッダが「Clang 20 以降」を要求する
（`static_assert` と Clang 20 の組み込み関数を使う）ため、clang-cl / clang-tidy が
それを解析できる新しさである必要があるからです。以前は `choco install llvm` で最新版を
入れ直していましたが、実測すると**既に入っているものの入れ直しに 31 秒**かかるだけだった
ので、インストールはやめてバージョンが 20 以上であることを確認するだけにしました
（将来ランナーの LLVM が MSVC ヘッダの要求より古くなったら、パースエラーの山ではなく
その旨のメッセージで落ちます）。Ninja も同様にイメージに入っているものを使います。

`tidy-windows` に vcvars（`msvc-dev-cmd`）のステップもありません。clang-cl は MSVC
ツールチェインと Windows SDK をレジストリ／vswhere から自力で見つけるので、`INCLUDE` /
`LIB` を環境へ流し込む必要がなく、その 12〜17 秒も不要でした（compile database は
どちらでもバイト単位で同一になります）。代わりに clang-cl は**絶対パス**で指定して
います — ランナーの PATH には Visual Studio 同梱の LLVM（`VC\Tools\Llvm\x64\bin`）も
入っており、`clang-cl` という名前がどちらに解決されるかを運任せにしないためです。

> **このジョブの所要時間を測るときの注意:** clang-tidy ステップの実時間は、同じ作業
> でも**ランナーによって 1.4 倍ほど振れます**（同一の 1 翻訳単位が、あるホストでは
> 25 秒、別のホストでは 37 秒）。したがって**2 つの run を比べてもチューニングの
> 良し悪しは分かりません**。実際この節の内容は、その誤りによって一度「修正」され、
> 元に戻された経緯があります。比較するときは A と B を**同一ジョブ内で交互に**測り、
> 最後にもう一度 A を測ってドリフトの対照とすること。

**採用しているルール:**

- フォーマット（`.clang-format`）— タブインデント（幅 4）、Allman ブレース
  （`{` を次行に置く）、名前空間本体をインデント、ポインタ／参照は型側に寄せる
  （`int* p`）、コード幅 100 桁で折り返し。コメントは再整形しません
  （`ReflowComments: false`）— 手作業で整形された重厚なコメント（日本語・罫線を
  含む）を壊さないためです。
- 静的解析（`.clang-tidy`）— `bugprone-*`、`performance-*`、`modernize-*`、
  `readability-*`、`cppcoreguidelines-*`、`clang-analyzer-*` を有効化し、
  スタイル系のノイズ（フォーマットは clang-format が担当）や大規模な無関係リ
  ファクタを要求するチェックは無効化しています。

**ローカルでの実行**（CI と同じチェック）:

```bash
scripts/lint.sh          # チェックのみ（違反があれば非ゼロ終了）
scripts/lint.sh --fix    # その場で自動修正（clang-format -i / clang-tidy --fix /
                         # cmake-format -i）。残りは検査のみ
```

`scripts/lint.sh` は CI と同じ全ツールを走らせ、未インストールのツールは
「skip」と表示して飛ばすので、手元に一部しか入っていなくても部分実行できます
（完全なゲートは CI 側）。各ツールのインストール方法は未インストール時に表示
されます。CI が使うバージョンは `lint.yml` 冒頭の `env:` に固定してあります。

`.editorconfig` も用意してあり、多くのエディタがインデント・改行・文字コードを
保存時に合わせるので、CI に到達する前から規則に近い状態を保てます。

> **さらに強化するには（任意）:** コミット前の自動実行に
> [`pre-commit`](https://pre-commit.com/)、追加の C++ 静的解析に
> [`cppcheck`](https://cppcheck.sourceforge.io/) を組み合わせられます。SDK 依存の
> プラグイン本体（`Updater.cpp` など）は CI（`build.yml`）で clang-tidy を掛けて
> いますが、ローカルで掛けたい場合は SDK を用意したうえで
> `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` でコンフィグし、生成された
> `compile_commands.json` に対して `clang-tidy` を実行してください。

## SDK ドキュメント（API 仕様）

**Vectorworks SDK の API 仕様は GitHub 上の Markdown リポジトリで公開されています。**
かつての開発者 Wiki（`developer.vectorworks.net`）は廃止され、現在は用途ごとに分かれた
公開 GitHub リポジトリに移行しています（一覧はランディングページ
[`DeveloperLandingPage.md`](https://github.com/Vectorworks/developer-scripting/blob/main/DeveloperLandingPage.md)
を参照）。旧 Wiki の URL（`index.php?title=SDK:...`）は現在このランディングページへ
301 リダイレクトされます。

**本プラグインの開発でまず参照するのは、公式リファレンスをフォークして実測知見
（`Findings/`）と調査用 CI を足した
[`min-nano/vectorworks-developer-sdk-reference`](https://github.com/min-nano/vectorworks-developer-sdk-reference)。**
公式リファレンスに無い「実機でしか判明しない挙動」「SDK に無い／効かない API」は
そちらの [`Findings/`](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/README.md)
にあり、SDK の挙動について新しく分かったこともそちらへ足す（調査のフローは同
リポジトリの CLAUDE.md）。

| 内容 | リポジトリ |
| --- | --- |
| **C++ / VCOM SDK ＋ 実測知見**（このプラグインが参照する） | <https://github.com/min-nano/vectorworks-developer-sdk-reference> |
| C++ / VCOM SDK（上記の fork 元。公式） | <https://github.com/Vectorworks/developer-sdk> |
| Python / VectorScript / Marionette スクリプト | <https://github.com/Vectorworks/developer-scripting> |
| ワークシート関数 | <https://github.com/Vectorworks/developer-worksheets> |

すべて Markdown なので、Web ブラウザが使えない環境（CI やエージェントのサンドボックス
など）でも、GitHub へ到達できれば内容を確認できます。`git clone` でまるごと手元に
落とすこともできます:

```sh
git clone --depth 1 https://github.com/Vectorworks/developer-sdk
```

C++/VCOM SDK（[`developer-sdk`](https://github.com/Vectorworks/developer-sdk)）の主な
ドキュメント（`Info/` フォルダ）:

- [Using the SDK](https://github.com/Vectorworks/developer-sdk/blob/main/Info/Using%20the%20SDK.md) — SDK の使い方の全体像
- [Plug-in Module](https://github.com/Vectorworks/developer-sdk/blob/main/Info/Plug-in%20Module.md) — モジュールのエントリポイント（旧 `SDK:Module_Plug-in`。`src/ModuleMain.cpp` が対応）
- [VCOM (Vectorworks Component Object Model)](https://github.com/Vectorworks/developer-sdk/blob/main/Info/VCOM%20(Vectorworks%20Component%20Object%20Model).md) — VCOM の仕組み
- [Types](https://github.com/Vectorworks/developer-sdk/blob/main/Info/Types.md) — 基本型（`TXString`・`WorldPt` ほか）
- [The Vectorworks Environment](https://github.com/Vectorworks/developer-sdk/blob/main/Info/The%20Vectorworks%20Environment.md) — 実行環境
- バージョン別の情報は [`Versions/`](https://github.com/Vectorworks/developer-sdk/tree/main/Versions)（2026 / 2025 / … ）にあります。


## MCP ブリッジ（`core/Bridge` ＋ `draw/McpBridge` ＋ `scripts/mcp/vw-mcp-server.py`）

利用者から見た使い方は [`README.md`](../README.md)「MCP ブリッジ」、経緯は
`docs/DEV-NOTES.md` M24 / M30。ここは変えるときの決めごとです。

- **受け渡しの作法**（要求／応答の形・スプールのファイル名・原子的な書き方・id の綴り検査）は
  **`core/Bridge.h` ただ 1 つ**で、Python サーバはその対になる綴りを持ちます。どちらかを
  変えたら両方を直してください（`tests/vw-mcp-server.test.py` が落ちます）。
- **道具の表**（名前・説明・引数の形・実装）は **`draw/McpBridge.cpp` の `kTools` ただ 1 つ**
  で、Python サーバは起動時に `vw_tools` でそれを取りに行きます。**道具を足すときに触るのは
  その 1 行と実装 1 つだけ**で、Python 側は直しません。JSON は `core/Json`（ブリッジ専用）。
- **受け付けは常駐のパレットの時計が 1 回ずつ呼びます**（M30。`draw::serveMcpBridge` は待たずに
  戻る）。**本体の中にループを書かない**——書けば図面がまた塞がります。本体のコードが
  スタックに載っている間（`PayloadInUse`）は見送ります。
- **v1 の道具は読むだけ**です。パレットが隠れていても受け付けるのは読むだけだから許して
  いるので、**図面を書く道具を足すなら、その判断をやり直し**、undo の作法
  （[SDK リファレンス「Undo」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Undo.md)）を
  必ず通してください。
- **Vectorworks を起こすのは Python サーバの道具**（`vw_launch`）で、プラグイン側には書きません
  （起こす前にはプラグインが居ない）。

## 実機フィードバックの往復（`draw/Feedback`）

> **本番の取り込みコマンドとは別の入口です（M25）。** 往復を回すのはメニューの
> **「実機テストを実行…」**（開発版だけ）で、本番の「IFC (ホームズ君) 取り込み…」は往復を
> 何も知りません。両者が共有するのは**絵を作るところ**（`draw/ImportRun` の
> `runImportRound`）だけなので、**テストで走るのは本番と同じコード**です。M24 まではこれが
> 1 つのコマンドに同居していて、`#ifdef VW_DEV_BUILD` の外にある分岐が安定版にも入って
> いました（`docs/DEV-NOTES.md` M25）。

**`draw/` の実描画は CI では検証できず、ローカルの Vectorworks でしか確かめられません**
（[`CLAUDE.md`](../CLAUDE.md)「テスト方針」）。そのため 1 往復ごとに

> 新しいビルドを入れる → Vectorworks を再起動する → 図面を戻す → IFC を選ぶ →
> 設定を選ぶ → 取り込む → ログを写して PR へ貼る

という手作業が挟まり、**これが実装そのものより時間を食っていました**。M21 でアップデートに
再起動が要らなくなった（＝新しい本体をその実行のまま読み直せる）ので、この往復を
**プラグイン自身に回させられる**ようになりました。人がするのは「絵を見て一言書く」だけです。

### 1 周の流れ

```
   ① メニューで **「実機テストを実行…」**（人がするのはこれだけ。開発版だけのコマンド）
        ↓
   ② 1 周目だけ: IFC を選ぶ → 取り込み設定 → **結果を PR へ送るか**（宛先・伏せ字）。
      **いま開いている図面を作業ファイルへ別名保存し**、以後の周の基準にします
      2 周目以降: **ダイアログは 1 枚も出ません**。図面を取り込み前へ戻してから描きます（下記）
        ↓
   ③ 取り込みが走る（1 分以上）——**ここから先、人の操作は 1 つも要りません**
        ↓
   ④ 終わったら黙って PR へ投稿。結果ダイアログも出ません
        ↓
   ⑤ 直った版が push され、CI が dev プレリリースを出す
        ↓
   ⑥ **往復のパレットが見つけて、入れて、同じ条件で取り込み、投稿します**（M24。
      人の操作は 1 つも要りません。①〜④ を人が繰り返す必要はありません）
        ↓
   ⑤ へ戻る——Claude が PR に「もう要らない」の合図を付けるか、人がパレットで止めるまで
```

パレットが開いていない（止めた・閉じた・再起動した）ときは、⑥ の代わりに人が ① を
1 回実行します（新しいビルドが尋ねずに入り、ファイル選択も設定も再起動も出ません）。

**図面を「取り消し」で戻す必要は、たいていありません（M25）。** 周と周のあいだは
プラグインが自分で図面を取り込み前へ戻します（下記「図面の戻し方」）。どう戻したかは毎回の
PR コメントの「準備:」と「図面の状態:」の 2 行に出ます。

**同じビルドが動いているなら取り込みません。** 前の周と同じ数字が並ぶだけで 1 分を使う
意味が無いので、①を押しても**往復を回し直すだけ**です（止めた往復をその場で再開する入口を
兼ねます）。ただし手で押した周には「回り直しました」という結末を返します
（`TestRoundOutcome::Rearmed`）。M24 まではここで「新しい 1 周目」として取り込み直しており、
パレットが開いている最中にメニューを押した人が同じ round を二重に投稿する事故が実機で
起きました（`docs/DEV-NOTES.md` M25）。

**尋ねることは全部、取り込みが始まる前に尋ね切ります。** 取り込みは 1 分以上かかるので、
終わったところに確認が待っていると席を離れられません——それでは「実行して放っておく」が
成立しません（実機の指摘）。宛先も伏せ字もトークンの登録も②で済ませ、④は投稿して黙ります。

**本体はビルドを待ちません。** 当初は⑤をプラグイン自身（本体）が待っていましたが、待つ
あいだ進捗ダイアログが Vectorworks を止めてしまい、**その周の絵が見られません**でした
（実機 round 3 の所見）。M24 で待機を**モードレスなパレット**（`IExtensionWebPalette`。
下記）へ載せ替え、完全な無操作と図面の操作を両立させました（`docs/DEV-NOTES.md` M24）。

**入れるのは殻（更新の確認）の仕事です。** 本体（ペイロード）は投稿して戻るだけで、
インストールと差し替えは実機テストのコマンドの頭にある更新の確認が行います
（`UpdateCheckKind::Auto`。下記）。本体のコードがスタックに載っている間は本体を降ろせ
ないので、差し替えはどのみち殻へ返ってからにしかできず、**インストールの経路はこの
リポジトリに 1 本だけ**（`src/UpdaterFlow.cpp`）に保てます。

### 図面の戻し方（3 段構え・M25）

戻し方は**「取り消し」→「作業ファイルを開き直す」→「レイヤ削除」の順**で、前が効けば後ろは
要りません。順序は入れ替えないでください。

1. **取り消し**（`undoPreviousRound` → `draw/DrawUtil` の `UndoOneStep`）。取り込みは自分で
   undo イベントを開き、作ったレイヤを登録している（`ImportUndoScope`）ので、利用者に頼んで
   いた「取り消し」と同じものをプラグインから起こせます（VectorScript 経由の
   `DoMenuTextByName('Undo', 0)` が実機で効くと確定。
   [Findings「Undo」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Undo.md)）。
   **取り込み前から在ったレイヤ（テンプレートの「共通」等）へ描いた分まで戻る**のはこれだけです。
2. **作業ファイルを開き直す**（`openRoundDocument`。
   [Findings「Documents」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Documents.md)）。
   1 周目に別名保存した作業ファイルを毎周開き直し、まったく同じ初期状態から始めます
   （使う口は `ISDK::SaveActiveDocumentPath` / `CloseDocument` / `OpenDocumentPath` /
   `GetActiveDocument`）。
3. **前の周が作ったレイヤだけを消す**（`prepareDrawingForRound` → `draw/DrawUtil` の
   `RemoveCreatedLayers`）。作業ファイルを用意できなかった周の控えです。取り込み前から
   在ったレイヤへ描いた分は取り除けないので、そこだけ絵が二重になります（PR コメントがその周
   だけ「取り消しで戻してください」と言います）。

ISDK の API から丸ごと戻す道は 4 本とも塞がったままです（SDK リファレンス
[#23](https://github.com/min-nano/vectorworks-developer-sdk-reference/issues/23) /
[#27](https://github.com/min-nano/vectorworks-developer-sdk-reference/issues/27) /
[#31](https://github.com/min-nano/vectorworks-developer-sdk-reference/issues/31) /
[#39](https://github.com/min-nano/vectorworks-developer-sdk-reference/issues/39)）。上の 1 は
VectorScript を経由する道です。

### モードレスの往復パレット（M24）

取り込み結果を PR へ送った周の終わりに、**「実機フィードバックの往復」というパレットが
開きます**（dev ビルドだけ）。図面を操作したまま開いていられ、中身はこれだけです。

| 表示 | 意味 |
| --- | --- |
| 往復中（新しいビルドを待っています） | 1 分ごとに PR の合図と同じブランチの dev ビルドを見ています |
| 入れ替え・取り込みの最中です | 新しいビルドを入れて、同じ条件で取り込んでいます（進捗ダイアログが出ます） |
| 往復を終えました（理由） | 止まりました。続きは「実機テストを実行…」をもう一度実行すると同じ条件で走ります |

ボタンは 3 つ——**今すぐ確認**（1 分を待たずに見る）・**往復を止める**（PR へ「終えました」を
1 通投稿して止め、理由をパレットに残す）・**止めて閉じる**（同じく止めてから、パレットを
隠す）。**「隠すだけ」はありません**——隠したパレットの中の時計は止まらないので、往復を
残したまま隠すと、見えないところで無人の取り込みが走り続け、止める口が無くなります。

**回り出すのは、人がメニューの「実機テストを実行…」を押したときだけです。** パレットが
開いているだけでは回りません（Vectorworks を起動し直したときも同じ。記憶は残っているので、
1 回押せば続きから回り出します）。

**止まる条件**は次のどれかで、**どれも記憶は消しません**（「実機テストを実行…」をもう一度実行すれば
続きの周として走り、そこでまた回り出します）。

- 人がパレットで「往復を止める」または「止めて閉じる」を押した。
- Claude が PR に `homeskz-ifc-feedback v1 control=stop` の HTML コメントを**その行だけの
  1 行**として投稿した（**「もう要らない」の合図**。往復の終わり方はこれが本筋です）。
  文中に引用したものや ``` で囲んだものは合図になりません——そう縛らないと、**目印を
  説明した文章が合図になります**（実機 round 1 で、プラグイン自身の投稿が末尾で書いて
  いた案内文を自分の合図として読み、1 通目で止まりました）。
- PR が閉じた／マージされた。
- 新しいビルドを入れられなかった・殻まで変わった（再起動が要る）・取り込めなかった・
  投稿できなかった。パレットに理由が出て、人の操作が要る場合は PR にも「終えました」が
  載ります（読む側が待ち続けないように）。

オフラインなど**確認そのものができなかったときは止まりません**（次の周期にもう一度見ます）。

**どこに何があるか**: 駆動（状態機械と止まる条件）は `src/FeedbackLoop.*`（殻・SDK 非依存・
`tests/FeedbackLoopTests.cpp`）、殻の実物は `src/FeedbackLoopHost.*`、パレットの登録と JS の
取り次ぎは `src/Extensions/ExtFeedbackPalette.*` と `resources/<vwr>/html/index.html`。
本体が殻へ見せるのは往復の記憶（`vw_payload_loop_status`）・「止めた」の受け口
（`vw_payload_loop_end`）・周そのもの（`vw_payload_run_test`。M25）だけです
（`src/PayloadAbi.h`）。合図は同梱スクリプトの `loop-control <repo> <n> [since]` が読みます
（`state=open|closed|merged` / `control=stop|none`。since は `post` が返す `created=`）。

**実機未確認のところ**: この拡張種別（`IExtensionWebPalette`）は SDK リファレンス側で
ヘッダ根拠と構文チェックまでしか確かめられていません
（[Findings「モードレス（非モーダル）なパレット」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)）。
確かめること: パレットが開き図面を操作しながら見えるか・JS のタイマーからの呼び出しが
届き続けるか・表示／非表示が切り替わるか・取り込みの最中に届く呼び出しが無視されるか。
**効かなくても手動の周は壊れません**（メニューからの実機テストは動きます）。
分かったことは SDK リファレンスの `Findings/` へ足してください。

### 往復の最中は尋ねずに更新する（`UpdateCheckKind::Auto`）

実機テストのコマンドは頭で更新を確認します（下記「いつ確認するか」）。往復の中では、その
確認の性格が変わります。

| いつ | 種別 | ふるまい |
| --- | --- | --- |
| 1 周目 | `Silent` | 更新があるときだけ「インストールしますか？」と尋ねる。オフラインなら黙って取り込みへ進む |
| 2 周目以降（往復の最中） | `Auto` | 同じブランチの新しいビルドを**尋ねずに入れ、黙って取り込みへ進む** |

尋ねないのは、その人が**「直したから、もう一度実行してほしい」と言われて実行している**
からです。そこへ確認を挟むのは、この往復が無くそうとしている手間そのものになります。

`Auto` に切り替わる仕掛けは覚え 1 つだけです。本体が投稿できた周の終わりに「**次は尋ねず
に入れてよい**」を戻り値で返し、殻がそれを `static bool` で 1 回ぶん覚えます
（`src/Extensions/ExtTestMenu.cpp`）。**殻に置くのは本体が入れ替わっても残ってほしいから**で、
Vectorworks を閉じれば消えてよい類の覚えなのでファイルには落としません。

`Auto` が `false` を返したら（入れられなかった・殻まで変わった・本体を降ろせなかった等。
下記「いつ確認するか」）、殻は**その実行の取り込みを見送ります**（古い本体で 1 分以上
かけて前の周と同じ結果を出し、それをまた PR へ投げても仕方がありません）。

### 所見はプラグインが訊きません

**絵を見て気付いたことは、人が Claude とのチャットへ直接書きます。** 所見を書くには結局
その人が実機を見ている必要があり、見ているならチャットのほうが速く、スクリーンショットも
貼れます。M23 では一度「所見を別プロセスのダイアログで訊く」仕組みを作りましたが、
**プラグインが持つ理由が無かった**ので外しました（`docs/DEV-NOTES.md` M23）。**足し直さない
でください。**

プラグインが PR へ載せるのは**数字と診断ログ**だけです。数字だけで判断が付かないときは、
Claude が「実機で確かめてほしい点」を PR かチャットへ返します。

### ボタンは「何を」するのかを名乗ります

動詞だけのボタンは、対象が 1 つしかない画面でしか成立しません。1 周目の送るかどうかの
ダイアログのボタンは「取り込み結果を送る」／「送らない」で、**送るもの**（要素ごとの内訳・
描画側の注意・診断ログ）と、**投稿したあとは何も尋ねない**ことも書いてあります。

### 使いはじめ（初回だけ）

1. **dev ビルドを入れて起動します**（安定版では動きません。下記「境界」）。
2. GitHub の **fine-grained personal access token** を 1 つ作ります。要るのは対象
   リポジトリの **Pull requests: Read and write** だけです。
   （`gh` CLI で認証済みの機械なら、この手順は要りません——スクリプトが `gh auth token`
   を使います。）
3. 試したい図面（空のテンプレート等）を開き、メニューの **「実機テストを実行…」**
   （開発版だけ）を 1 回実行して、フィードバックのダイアログで **「取り込み結果を送る」**
   を押します。トークンが未登録なら 1 度だけ貼り付けを求められ、**macOS はキーチェーン**、
   **Windows は DPAPI で暗号化したファイル**へ保存されます（図面にもログにも残りません）。
4. 送信先の PR 番号は**ブランチから自動で引きます**（`find-pr`）。違っていればその場で
   直せます。

以後は**往復のパレットが回します**（直った版が出るたびに入れて取り込んで投稿します。上記
「モードレスの往復パレット」）。気付いたことは Claude とのチャットへ書いてください。

**やめたいときは、パレットの「往復を止める」（または「止めて閉じる」）を押します**（記憶は
残るので、「実機テストを実行…」をもう一度実行すれば続きから走ります）。

**別の図面で試したいときは、その図面を開いてから「実機テストを実行…」を実行します。**
手で実行した周は、開いている図面が前の周の作業ファイルでなければ**そちらを新しい基準として
採り直します**（宛先・IFC・設定はそのままで、変わるのは「どの図面から始めるか」だけです）。
自動の周は前の周の続きなので、常に作業ファイルへ戻ります。記憶ごと消したいときは、往復の
記憶が無い状態から 1 周目のダイアログを出して「送らない」を選びます（または記憶のファイルを
消します）。

### 覚えているもの（`core/FeedbackSession`）

1 周目の選択（IFC のパス・取り込み設定・宛先 PR）と作業ファイルのパスは `key=value` の
テキストで残ります。**ここが 2 周目以降からファイル選択と設定ダイアログを消している唯一の
仕掛け**です。

- macOS … `~/Library/Application Support/HomeskzIfcImport/feedback.txt`
- Windows … `%LOCALAPPDATA%\HomeskzIfcImport\feedback.txt`

（フォルダ名はプラグインの改名に追随させていません。**識別子なので付け替えると進行中の
往復の記憶が行方不明になる**ためで、同梱スクリプトが置くトークンも同じフォルダです。）

手で消せば往復は終わります（次の実機テストはいつもどおり選択から始まります）。
`HOMESKZ_IFC_FEEDBACK_STATE` に別のパスを指定して差し替えられます。

### 公開されることと、伏せるもの

PR コメントは**公開**です。そこで**既定で案件が分かるものを伏せます**:

| 伏せるもの | どうなるか |
| --- | --- |
| IFC のファイル名 | `model-8f3a12.ifc`（**同じ入力なら毎回同じ仮名**。周回どうしの同一性は保たれる） |
| パスのユーザー名 | `/Users/…` `C:\Users\…` |
| 図面枠のスタイル名 | `style-4c1d09`（同じ名前なら同じ仮名。事務所名などを含むのが普通なので）。「<名前>」と設定の行では必ず、それ以外では 4 字以上の名前だけ替える（短い名前で別の意味の綴りを巻き込まないため） |

伏せないもの——要素ごとの命令数と描けた数、描画側の注意、所要時間、ログの本文——は
**案件ではなくプラグインの話**で、これが無いと報告の意味がありません。フィクスチャや
実案件の中身そのものは、そもそもコメントに載りません。

**それでも公開したくない**ときは、投稿先を私有リポジトリへ向けられます（記憶の
`repo=` を書き換える）。ただし Claude はその PR のイベントで起こされないので、
**往復の自動性は落ちます**。

### 境界（どこに何があるか）

| | 役割 |
| --- | --- |
| `src/core/FeedbackSession.*` | 覚えておく値と、その読み書き。**どの周になるかの場合分け**（`feedbackRoundKind`）もここ（無 SDK・テストあり） |
| `src/parse/Feedback.*` | **コメント本文**（Markdown）・前の周との差分・匿名化・実機テストの結末の文言（無 SDK・テストあり） |
| `src/draw/Feedback.*` | 実機テストの 1 周（`runTestRound`）——記憶・取り込み前のダイアログ・図面の準備・投稿（SDK 依存）。**待たないし、入れもしない** |
| `src/draw/ImportRun.*` | 取り込み 1 周ぶんの部品。**本番の取り込みと実機テストが共有する唯一の実装**（M25） |
| `src/Extensions/ExtTestMenu.*` | 実機テストの入口の登録と取り次ぎ（殻・**dev だけ登録**。M25） |
| `src/draw/HostServices.*` | 殻から借りた道具（同梱スクリプトの実行）の置き場所 |
| `src/FeedbackLoop.*` | **自動の往復の駆動**（殻・無 SDK・テストあり。M24）。合図と新しいビルドを見て、入れて、取り込みを本体に頼み、止まる条件を持つ |
| `src/FeedbackLoopHost.*` / `src/Extensions/ExtFeedbackPalette.*` / `resources/<vwr>/html/` | 駆動の殻の実物と、モードレスなパレット（SDK 依存。**判断は持たない**） |
| `scripts/vw-feedback.sh` / `.ps1` | **ネットワーク**。`token-status` / `login` / `logout` / `find-pr` / `post` / `loop-control`（ダイアログは持ちません。C++ 側はネットワークを書かない） |
| `scripts/vw-token.sh` / `.ps1` | **トークンの在り処**（保存先・探索順）。`vw-feedback` と `vw-update` の両方が source します（読むほうにも必ず付ける。M27） |

**安定版（stable）では動きません。** 往復するのは PR のビルドであって main の配布物では
なく、開発用でないビルドに「図面の情報が外へ出る経路」を持たせないためです
（`draw::feedbackAvailable`）。

**なぜ入れ替えが殻の側で起きるのか**: 新しい本体をその実行のまま効かせるには載っている
本体を降ろす必要があり、それができるのは**本体のコードがスタックに 1 つも無いとき**だけ
です（[`src/PayloadSession.h`](../src/PayloadSession.h)）。したがって入れ替えは必ず
「本体から戻ったあと」＝実機テストのコマンドの頭になります。殻に足したのは「次は尋ねずに
入れてよい」を 1 回ぶん覚える `static bool` だけで、往復の中身はすべて本体側にあります
（＝往復のふるまいを直しても再起動は要りません）。

### 設計の決めごと（往復を変えるときに守ること）

往復まわり（`draw/Feedback`・`src/FeedbackLoop*`・パレット・`scripts/vw-feedback.*`）を
変えるときの決めごとです。**どれも実機で一度壊れて決まったもの**なので、緩める前に
`docs/DEV-NOTES.md` の M23〜M25 を読んでください。

**入口と分担**

- **本番の取り込みコマンドに往復を書かない（M25）。** 往復（記憶を読む・尋ねずに入れる・
  準備する・投稿する・パレットを開く）を書いてよいのは `Extensions/ExtTestMenu` ＋
  `draw/Feedback` の `runTestRound` だけで、`draw/ImportCommand` と `Extensions/ExtMenu` には
  1 行も書きません。**`#ifdef VW_DEV_BUILD` で囲っても制御フローは本番の入口に残る**ので、
  囲えばよいとも考えません。共有してよいのは `draw/ImportRun` の `runImportRound` だけです。
- **本体は待たない・入れない。** `draw/Feedback` にビルドを待つコードを書かない（モーダルの
  ダイアログが図面を塞ぐ。実機 round 3）。インストールも書かない（経路は
  `src/UpdaterFlow.cpp` の 1 本。本体が殻へ返すのは「次は尋ねずに入れてよい」の 1 ビット
  だけ）。待つのは殻のモードレスなパレットです。
- **パレットに判断を持たせない。** JS は数秒ごとに `homeskz.tick()` を呼んで返った文言を
  並べるだけで、GitHub を見に行く間隔も止まる条件も駆動（`src/FeedbackLoop`）が持ちます。

**回り出す・止まる**

- **回り出す入口は「人がメニューを押したとき」だけ。** 駆動は自分が武装したか
  （`FeedbackLoopDriver` の `fArmed`。`Arm` / `BeginRound` で立ち、止まると下りる）だけを見て
  回し、**記憶が `active` であることを理由に回さない**——パレットの JS タイマーは Vectorworks が
  生きているあいだ回り続けるので、記憶を根拠にすると誰も押していないのに往復が走り出します
  （実機で発生）。
- **止まっても記憶は消さない。** `loop` を下ろすだけにし、人がメニューから実機テストを実行
  すれば続きの周として走るようにします。**新しい停止の入口を足すときも、記憶を消す側へ
  倒さない。** 確認そのものができなかった（オフライン等）ときは止めません。
- **キャンセルされた周は「試し終えた」ことにしない**——記憶の `lastCommit` を進めません。
  進めると、進捗ダイアログのキャンセルを一度押しただけでそのビルドでは往復を再開できなく
  なります（実機 round 10）。
- **やめる入口はパレットだけに置く**（取り込みのダイアログには足さない）。記憶ごと消す意思は
  1 周目のダイアログの「送らない」が受け取ります。
- **パレットを隠すだけの口を作らない。** 「閉じる」は往復を止めてから隠します
  （`FeedbackLoopStopForClose` → `ShowFeedbackPalette(false)`）。
- **隠れたページは返事を受け取れない、と考えて設計する。** 順序は必ず**止める → 返事 →
  隠す**。JS 側も閉じる呼び出しの返事を待たず（待つと「返事待ち」が解けずパレットが二度と
  動かなくなる。実機で発生）、「呼び出し中」の印は時間で自動的に解きます。

**尋ねる・伝える**

- **尋ねるのは取り込みが始まる前だけ。** `postFeedbackRound`（取り込みのあと）にダイアログを
  足しません。投稿に失敗した理由も、アラートではなくいつもの結果ダイアログへ添えます。
- **続きの周にダイアログを 1 枚も出さない。** 確認は情報を生まずクリックだけ増やす（押した
  かどうかは起きたかどうかではない）。頼みごとは 1 周目のダイアログで言い切り、実際にどう
  だったかは描画側の実測を PR コメントへ載せます。図面が戻っていたかは 1 周目に採った基準と
  `DrawCounts::existingLayers` の引き比べで判定します（真偽 1 つでは、テンプレートにもとから
  在るレイヤと前の周の残りを区別できない）。
- **正しく何もしないことは、黙っていてよい理由にならない。** 自動の周は黙ってよいが、人が
  押した操作には必ず結末を返します（同じビルドで回り直しただけでも `Rearmed` を返す）。
- **実機テストの結末は実機テスト自身の言葉で言う。** 取り込みコマンドの完了文言
  （`parse::formatImportResult` / `formatImportError`）を借りず、`parse::formatTestRoundResult`
  を使い、結果ダイアログのタイトルも本番と別にします（`kTestResultTitle`）。共有してよいのは
  ダイアログの器（`draw/ResultDialog`）だけです。
- **取り込みの最中に SDK に訊かせない。** Vectorworks 自身がモーダルを出す余地は渡す値の側で
  潰します（例: 図番が重なると「新しい図番を割り当てますか？」が出るので、軸組図の図番は
  `parse::uniqueSectionNumbers` が一意にする）。
- **手動で押した周は、いま開いている図面を基準として採り直す**（`allowDialogs` のときだけ）。
  捨てるのは「どの図面から始めるか」だけで、宛先・IFC・設定はそのままです。自動の周は常に
  作業ファイルへ戻します。

**図面を戻す（利用者のものを消すコード）**

ここはアンインストーラと並ぶ「利用者のものを消す」コードなので、**歯止めを緩める方向へ
変えません**。

- **図面から消してよいのは「前の周が自分で作ったレイヤ」だけ。** 入口は `draw/Feedback` の
  `prepareDrawingForRound` ただ 1 つで、SDK の作法（自分で undo イベントを開いて閉じる・
  **シートを先に消す**）と安全弁（名前と種別が一致し、消したあとに 1 枚も残らなくならない
  こと）は `draw/DrawUtil` の `RemoveCreatedLayers` が 1 か所で持ちます。消す相手は
  `ImportUndoScope` が控えた作ったレイヤの名前（`DrawCounts::createdLayers` /
  `createdSheets` → `core::FeedbackSession::lastCreatedLayers` / `lastCreatedSheets`）だけで、
  **「基準に無いレイヤ」を消す作りにしない**（利用者が別の用途で足したレイヤを巻き込む）。
- **取り消しを押してよいのは、いま開いているのが自分の作業ファイルで、かつ前の周が作った
  レイヤが実際に残っているときだけ。** 利用者の図面では絶対に押さない（取り消しスタックの
  上にあるのは利用者自身の編集かもしれない）。効いたかは読み戻して確かめ（前の周のレイヤが
  1 枚も無くなったか）、1 周が undo イベント 1 つとは限らないので上限つきで数段掛けます。
  **Python では走らせない**（`ExecuteScript` から `vs.*` を呼ぶと VW ごと落ちる）・**`CompileScript` を呼ばない**
  （成功でもモーダルが出る）・**自分の undo イベントを開いたまま呼ばない**。スクリプトの文面は
  固定にし、図面の値や人の入力を混ぜません（失敗するとモーダルのエラーで無人の周が止まる）。
- **作業ファイルはテンプレートを複製して作らない。** 基準は**いま開いている図面**で、1 周目に
  それを別名保存します（複製する作りは実機で 3 周続けて失敗した）。
- **周の終わりは「別の捨て場所へ保存し直してから閉じる」。** 未保存の変更がある文書は
  `CloseDocument()` が閉じられない（実機確認済み）ので、この順序でなければ閉じられません。
  保存先はいつも**まだ無いパス**（`FreshTempPath`。既存のファイルへの別名保存が失敗した実測が
  ある）。**保存できなければ閉じない**（図面が 1 枚増えるほうが軽い）。
- **前の周の図面はパスで見分ける**（`core::FeedbackSession::workPath` と引き比べる）。
  アクティブな文書が作業ファイルでなければ触りません。`fFileRef` を記憶に持ち越すのは禁止
  （再起動後に同じ番号が別の図面に割り当たり、利用者の図面を閉じる恐れがある）。
- **開き直せたかは戻り値ではなく読み戻しで見る**（`GetActiveDocument` のパス）。開き直せ
  なかったら黙って続けず、何が起きたかを「準備:」の 1 行に**証拠つきで**載せます（いま開いて
  いる図面へ描くと前の周に重なり、数字は揃うのに絵が壊れる）。
- **作業ファイルを採る周は採る前に、開き直したあとにも、前の周が作ったレイヤを落とす。**
  汚れたまま採られた作業ファイルが以後の周をずっと汚すのを防ぎ（実機 round 13）、きれいな
  ファイルでは素通りするので自分で直ります。基準を採り直す周はレイヤの基準
  （`baselineLayers`）も採り直します。

### 届いたコメントの読み方（Claude 向け）

dev ビルドが投稿するコメントは必ず次の行で始まります。

```
<!-- homeskz-ifc-feedback v1 round=<N> build=<commit> branch=<branch> -->
```

この目印があるコメントは**利用者の Vectorworks が自動生成したもの**で、人は 1 文字も
書いていません。**数字と診断ログしか無く**、絵がどう見えたか（所見）は利用者がチャットへ
直接書きます。読む順序は:

1. **「図面の状態:」を見る。** 正常なら「取り込み前から在ったレイヤに描きました」に落ち着き
   ます。「前の周の図が残ったまま重ねて描きました」なら**戻しに失敗している**（「準備:」の行に
   何をしたかが出る）ので、**絵は二重になっている——絵の破綻をそのまま実装のせいにしない**
   でください。数字は重なっていても変わらないので、この行が唯一の手掛かりです。1 周目は
   「基準にします」としか言いません（テンプレートにもとから在るレイヤを「戻し忘れ」と読み
   違えないため。M23 で一度そう読み違えた）。
2. **「前の周からの変化」を見る。** 数字が動いていないなら、直したつもりのところに届いて
   いません。
3. 要素の内訳・注意・診断ログで裏を取る。
4. **命令の数が合っていても絵が破綻していることは普通にある**と常に疑う。怪しければ、
   「3 階の梁の天端が基準面と合っているか」のように**絵で見て答えられる形**で確かめてほしい
   点を返します（答えはチャットで来ます）。

**直したら push します。次の周は自動で来ます。** push したら、その周で**何が変わるはずか**を
PR に 1〜2 行で返しておきます（次のコメントと突き合わせられる）。

**頼んでよいのは「メニューの『実機テストを実行…』をもう一度実行してください」だけ**です
（パレットが回っている間はそれすら要りません。本番の「IFC 取り込み」では往復は動きません）。
図面を「取り消し」で戻すことは、PR コメントがそう言っている周だけ頼んでよい。ファイルを
選び直す・設定を選び直す・再起動する・ログを貼る——このどれかを頼みたくなったら、それは
仕組みが壊れている合図なので、頼む前に直してください。

**往復がもう要らなくなったら合図を投稿します。** 黙って push をやめるだけでは、利用者側の
パレットは待ち続けます。PR へ次の 1 行を含むコメントを投稿すると、次の確認（1 分以内）で
パレットが止まります。

```
<!-- homeskz-ifc-feedback v1 control=stop -->
```

**その行だけの行にします**（前後は空白のみ。人が読む文は別の行に添えてよい）。文中へ引用
したものも ``` で囲んだものも合図になりません（`docs/DEV-NOTES.md` M24「合図は行であって、
文中の引用ではない」）。逆にプラグインが `control=ended` のコメントを投稿してきたら、利用者側で
往復が止まった（パレットで止めた・入れ替えに失敗した・殻まで変わって再起動が要る）という
ことで、以後は push しても自動の周は来ません。続きが要るなら実機テストの再実行を頼みます。

**自動の周は実機確認の代わりになりません。** 描画に触れる PR をマージしてよいのは人が
「確認できた」と言ったときだけです（[`CLAUDE.md`](../CLAUDE.md)「開発プロセス: PR とマージ」）。

---

## 自動アップデートの仕組み

利用者から見た挙動は [`README.md`](../README.md)「自動アップデート」にあります。ここは
その内部の仕組みです。

アップデートはコマンドラインではなく、**プラグイン自身がネイティブの Vectorworks
ダイアログ**（`gSDK->AlertInform` / `gSDK->AlertQuestion`、およびドロップダウン選択は
`VWFC::VWUI::VWDialog` + `VWPullDownMenuCtrl`）を表示して行います（`src/Updater.cpp`）。ネットワーク・インストールなどの実処理（GitHub API の参照・
ダウンロード・`Plug-Ins` へのインストール）は**プラットフォームごとの更新スクリプト**
に集約され、ビルド時にインストール物と一緒に**同梱**されます:

- **macOS** — `scripts/vw-update.sh`（bash）。バンドル内の
  `Contents/Resources/vw-update.sh` に入ります。
- **Windows** — `scripts/vw-update.ps1`（PowerShell）。`.vlb` の隣に入ります。

**GitHub を読むときは、トークンがあれば必ず付けます**（`scripts/vw-token.{sh,ps1}` を
source して探します。無ければ従来どおり認証なしで続きます）。公開リポジトリなので認証は
要りませんが、**認証なしの GitHub REST は IP ごとに 1 時間 60 回**で、往復のパレットは
1 分ごとに `q-dev` を呼ぶ＝ちょうど上限に張り付きます（実機で「リリース一覧を取得でき
ませんでした」として出ました。`docs/DEV-NOTES.md` M27）。失敗したときは HTTP の番号・
curl の終了コード・API 制限なら**いつ戻るか**まで `error=` の行に載せます。

### 探すのは同梱スクリプト、置くのはリリース側のインストーラ

**同梱スクリプトはファイルの配置を行いません。** 走るのは常に**インストール済みの
（＝古い）**もので、そこに配置手順を持たせると「新しいビルドがどんなファイルでできて
いるか」を永遠に知らないままになるためです。実際 M21 で本体（`.vwpayload`）が増えた
とき、古いアップデータはそれを写さず、利用者は zip を手で落として置き直す羽目に
なりました。

そこで役割を 2 枚に割ってあります。

| | 何をするか | どこにあるか | いつの版が走るか |
| --- | --- | --- | --- |
| **同梱スクリプト**（`vw-update.*`） | リリースを探す・比べる・落とす・ダイアログの受け答え | インストール済みプラグインの中／隣 | **古い**（インストール済みの版） |
| **インストーラ**（`vw-install.*`） | **配置**（隔離解除・アドホック署名・差し替え・殻 ID の報告） | **配布 zip の直下**＋リリースのアセット | **新しい**（いま落としたビルドの版） |

`do-install` は zip を展開したあと、その直下にある `vw-install.sh` /
`vw-install.ps1` を `--machine --from <展開先> --name <プラグイン名> --plugins-dir <先>`
で呼び、**その機械可読な出力（`installed-shell=` / `ok` / `error=`）をそのまま
プラグインへ流します**（途中で組み直すと、将来キーが増えたときに落としてしまうため）。
zip にインストーラが無い＝この仕組みより前のリリースへ当たったときだけ、同梱スクリプト
自身の予備の配置へ落ちます。

インストーラ側の配置の規則はひとつだけです——**zip の直下にあるものを、そのまま置く**
（除くのはインストーラ自身）。ファイル名を列挙しないので、構成が変わっても——ファイルが
増えても——利用者は手で入れ直さずに済みます。これがこの設計の目的そのもので、テストの
中心もそこにあります（`tests/vw-install.test.sh` / `tests/vw-install.Tests.ps1`）。

### プラグインは自分のフォルダを 1 つ持つ

置き先は **`Plug-Ins` の直下ではなく、プラグイン名のフォルダ**です:

```
<Plug-Ins>/min-nano_structure/min-nano_structure.vwlibrary
<Plug-Ins>/min-nano_structure/min-nano_structure.vwpayload
<Plug-Ins>/min-nano_structure/vw-uninstall.sh
```

Vectorworks が `Plug-Ins` のサブフォルダも読みに行くことは実機で確認済みです。こうして
おくと**そのプラグインのものが 1 か所に閉じる**ので、取り除くのが「フォルダを 1 つ消す」
で済みます（下記）。

**渡された先が既にそのフォルダなら足しません。** 自動アップデートのとき、プラグインは
「いま自分が読み込まれたフォルダ」を `VW_PLUGINS_DIR` として渡してきます——サブフォルダ化の
あとはそれ自身が `<Plug-Ins>/<name>` なので、無条件に足すと更新のたびに `<name>/<name>/…`
と際限なく深くなります。この規則（`plugin_dir` / `Get-PluginDir`）は**インストーラ・
アンインストーラ・アップデータの 3 つで同じ**でなければなりません——片方だけ変えると、
入れた場所と読む／消す場所が食い違います。回帰テストがそれぞれに入っています。

### 入れる前に、前の版をその版自身のアンインストーラで取り除く

リリースには `vw-uninstall.sh` / `vw-uninstall.ps1` も同梱され、**インストール先へ一緒に
置かれます**（インストーラ自身は置かれません）。インストーラは配置に入る前に、
**いま入っているフォルダの中にあるアンインストーラ**を一時ディレクトリへ写して
`--machine --name <名前> --plugins-dir <そのフォルダ>` で叩きます。

置く側が「新しい版」の知識を持つのと**ちょうど対**で、取り除く側は「いま入っている版」の
知識を持ちます——その版が何を置いたかを正しく知っているのは、その版のアンインストーラ
だけだからです。見つからなければ何もしません（初回インストール、あるいはこの仕組みより
前の版）。失敗しても続行します——このあとどのみち上書きするので、取り除けなかったことを
理由にインストールごと失敗させるのは損です。

順序は **「アーカイブの検査 → 取り除く → 置く」** です。検査を先に済ませないと、
取り違えた zip で「消しただけで入れられない」状態を作ってしまいます。

アンインストーラが取り除くものの規則もひとつだけ——**そのプラグインのフォルダをまるごと。**
**ただし消してよい形かどうかを必ず確かめます**（フォルダ名が一致し、かつ中に殻がある
ときだけ）。ここは本リポジトリで唯一「利用者のディスク上のものを消す」コードなので、
その安全弁は回帰テストの中心になっています（`tests/vw-uninstall.test.sh` /
`tests/vw-uninstall.Tests.ps1`）。

Windows では**読み込み中の `.vlb` を削除できない**ので、中身は「退かしてから消す」
（消せなければ退かしたまま残す）。退いた `*.old-*` は拡張子が `.vlb` ではないので
Vectorworks は読み込まず、次のインストールが掃きます。

**スクリプトはどれも殻の ID（`VW_SHELL_ID`）に入れません**（`CMakeLists.txt` の
`VW_SHELL_INPUTS`）。インストーラ／アンインストーラはそもそも殻に入らず、同梱スクリプト
（`vw-update.*` / `vw-feedback.*`）も**プロセスへ読み込まれず、呼ぶたびにディスクから
読み直される**ので、置き換えれば次の呼び出しから効きます。狙いは「スクリプトを直した
だけで再起動を強いない」こと——それはこの仕組みが無くそうとしている手間そのものです。

**アセット名も決め打ちにしません。** 同梱スクリプトはまず `<プラグイン名>.vwlibrary.zip`
（Windows は `.vlb.zip`）を厳密に探し、無ければ**末尾がその拡張子のアセット**で拾い
直します（`plugin_zip_url` / `Get-PluginZipUrl`）。名前を変えた瞬間に、古いアップデータ
から何も落とせなくなる——つまりアップデートの経路そのものが切れる——のを避けるためです。

プラグインはこのスクリプトを**非対話モード**（`q-stable` / `q-dev` / `do-install`）で
呼び出して結果を受け取り、ユーザーへの表示はすべて自前のネイティブダイアログで行う
ため、利用者がターミナルを開く必要はありません。どちらの OS でも
`src/Updater.cpp` の同じフロー・ダイアログが動き、変わるのは「自分の場所を特定する
方法（macOS は `dladdr`、Windows は `GetModuleFileName`）」と「起動するスクリプト」
だけです。

### いつ確認するか — コマンドの入口で（起動時ではない）

**Vectorworks の起動時（`plugin_module_main`）には確認しません。** 以前は起動時に 1 度だけ
走らせていましたが、殻と本体に割れて以降（上記「殻と本体」）、機能追加以外の更新は
**再起動なしでその場から効く**ようになったので、起動のたびに問う理由が無くなりました。
起動を待たせずに済むうえ、後述のとおり**再起動を Vectorworks 自身に頼めるようになる**
という副産物もあります。**起動時の確認を復活させるなら、再起動の作りも一緒に戻してください**
（下記「以前は SDK に頼めなかった」）。

入口は次のとおりで、`src/UpdaterHost.h` の `UpdateCheckKind` がこの違いを表します。

| 入口 | kind | ふるまい |
| --- | --- | --- |
| メニューコマンド「アップデータを確認」（`src/Extensions/ExtUpdateMenu.cpp`） | `Manual` | 尋ねて入れる。**結末を必ず伝える**（最新です／確認できませんでした） |
| 取り込みコマンドの頭（`src/Extensions/ExtMenu.cpp`） | `Silent` | 更新があるときだけ尋ねる。**無ければ黙って取り込みへ進む**。往復の分岐は持たない（M25） |
| 実機テストの頭・1 周目（`src/Extensions/ExtTestMenu.cpp`。**dev だけ**） | `Silent` | 同上 |
| 実機テストの頭・2 周目以降（往復の最中） | `Auto` | **尋ねず・報せずに入れる**。入らなかったら走らせない（`false`） |
| 往復パレットの周期確認（`src/FeedbackLoop.cpp` の `PollDevBuildWith`。M24） | — | **ダイアログを 1 枚も出さず、結末を値で返す**（パレットがその文言を出す） |

**「いま入っているビルド」はどの入口もディスクで判定します**（`q-dev` の `installed=` /
`installed-branch=`。`src/UpdaterParse.h` の `ResolveCurrentDevBuild`。M26）。殻に
コンパイルされた sha とブランチは、本体だけ入れ替えたあと古いままだからです（下記
「チャンネルごとの挙動」）。

**インストールの経路は `src/UpdaterFlow.cpp` の 1 本だけです。** 往復のような別の都合で
本体側へ 2 本目を書かないでください——本体のコードがスタックに載っている間は本体を降ろせ
ないので、どのみち殻へ返ってからにしかできません。本体がしてよいのは「出るまで待つ」まで
（それもいまは殻のパレットが担う）です。

`Manual` が黙らないのは、押したのに何も起きないと「最新だった」のか「そもそも動いて
いない」のかが利用者に区別できないためです。逆に `Silent` は取り込みたいだけの人の前に
「最新です」を挟みません。

`Auto` は[実機フィードバックの往復](#実機フィードバックの往復drawfeedback)の 2 周目以降
だけで使います。そこにいるのは本体が「同じブランチの新しいビルドが出た」と見届けて
戻ってきた直後なので、改めて「インストールしますか？」と尋ねるのは、この往復が無くそうと
している手間そのものになります。**入れ替えたことも報せません**——モーダルのダイアログは
Vectorworks を止めるので、絵を見ている人の前に立ちはだかるだけです。

`Auto` が `false`（＝輪を止めよ）を返すのは、**入れられなかったとき全部**です——殻まで
変わった／インストールに失敗した／本体を降ろせなかった／そもそも新しいビルドが見つから
なかった（オフラインを含む）。**入らなかったなら何も変わっていない**ので、同じ周をもう
一度回しても同じ結果が出るだけであり、しかも本体は周ごとに PR へコメントを投げるので、
放っておくと同じ報告が並びます。口を開くのは前 2 つ（人が知らないと困るもの）だけで、
後ろ 2 つは黙って止まります——待っていた本体の側が既にそう伝えているからです。

取り込み側の確認は**本体（ペイロード）を確保する前**に置いてあります。そこで新しい本体が
入れば `PayloadUse` がそれを読み直すので、**その 1 回目の取り込みからもう新しいコードが
動きます**。確保したあとでは本体のコードがスタックに載っているぶん降ろせず、反映は次回に
回ってしまいます。

**PIO（柱記号・耐力壁）のリセットからは確認しません。** 取り込み直後には数百回リセットが
走るので、そのたびに GitHub を叩くわけにはいきません。PIO の更新は、手動の確認か、
取り込み時の更新に乗って入れ替わるのを待ちます。

### チャンネルごとの挙動

- **stable（`min-nano_structure` / main）** — より新しい安定版ビルドがないかを確認します。
  - 新しいビルドがあれば `AlertQuestion` で「インストールしますか？」と尋ね、選ばれた
    場合だけインストールします。インストール後は**再起動を促すのではなく尋ねます**
    （下記）。
  - 既に最新なら `Manual` では「みんなの構造設計支援は最新です。」、`Silent` では無言。
  - ネットワーク確認は時間制限付き（`vw-update.sh` の `--max-time`）で、オフラインや
    エラーは `Manual` では「更新を確認できませんでした。」、`Silent` では無言です。

- **dev（`min-nano_structureDev` / ブランチ）** — kind で挙動が大きく変わる唯一の流れです。
  - `Manual` … 使用するビルドを**ネイティブのプルダウンダイアログ**
    （`VWFC::VWUI::VWDialog` + `VWPullDownMenuCtrl`、`src/Updater.cpp` の
    `CBuildPickerDialog`）で問い合わせます。1 つのドロップダウンに候補を一覧表示します:
    - 先頭は**現在ロードされているビルド**（branch / commit、「インストール済み」と明示）。
    - 続いて**他のブランチのプレリリース**（現在のビルドと同じコミットは除外）。
    - **インストール済み（先頭）を選ぶ** → 何も入れず、「開発版ビルドはそのままです。」と
      現在のビルドを伝えます（**押した操作には必ず結末を返す**——黙って閉じると、別の
      ブランチを選んだつもりの人には「選んだのに切り替わらない」と映ります）。
    - **キャンセル** → 何もしません（取り消し自体が意思表示なので、ここは黙ります）。
    - **別のブランチを選ぶ** → それをインストールし、続けて再起動を尋ねます（下記）。
    - 選べるビルドが他に無ければ、その旨を伝えます（黙って終わりません）。
  - `Silent` … **ダイアログを出さず**、**いま動いているのと同じブランチ**の新しいビルドが
    あるときだけ「インストールしますか？」と尋ねます。取り込みのたびにブランチ選択が
    出ては邪魔なので、拾うのは「自分のビルドが新しくなった」に当たるものだけです。
  - `Auto` … `Silent` と同じものを拾い、**尋ねずに入れます**（上記）。

  **「いま」の判定はディスクに入っているビルドで行います**（`q-dev` の `installed=` と
  `installed-branch=`。`src/UpdaterParse.h` の `ResolveCurrentDevBuild`）。コンパイル時に
  埋め込まれた commit（`VW_BUILD_VERSION`）とブランチ（`VW_BUILD_BRANCH`）は**殻の**値で
  しかなく、**本体（`.vwpayload`）だけの更新は再起動せずに効く**ので、別のブランチの
  ビルドへ乗り換えたあともその 2 つは前のブランチを名乗り続けます。そちらを基準にすると、
  選択ダイアログが「現在: 前のブランチ」と出して**いま入れたビルドをもう一度候補に並べ**、
  `Silent` / 往復の確認は**前のブランチ**の新しいビルドで上書きして乗り換えを巻き戻します
  （実機で起きました。`docs/DEV-NOTES.md` M26）。ディスクから分からないとき（刻印を出さない
  古い同梱スクリプト等）だけ、ビルド一覧の sha 照合 → 殻の値、と落ちます。

  ブランチの照合には `q-dev` の出力の**5 列目**（`build<TAB>commit<TAB>name<TAB>url<TAB>branch`）
  を使います。素のブランチ名はリリース本文（CI が書く `branch=` の行）から読みます。
  **この列は任意**で、インストール済みの（＝古い）同梱スクリプトが走ると空になります。
  そのときは**表示名から補います**——`name` は `"Dev: <branch> (<sha>)"` の形なので、
  そこからブランチ名を確実に取り出せます（`src/UpdaterParse.h` の `DevBuildBranch`）。
  補えなければ照合できないので**何もしません**——別のブランチのビルドを勝手に入れるより
  ずっと安全です。

### インストール後（stable / dev 共通）— まず「再起動が要るか」を決める

プラグインは**殻**（`.vwlibrary` / `.vlb`）と**本体**（`.vwpayload`）に割れていて、
Vectorworks が起動時にしか読み込めないのは殻だけです（上記「ソースの構成」／
`src/PayloadAbi.h`）。したがってインストール直後の分岐は 2 つあります
（`src/UpdaterFlow.cpp` の `FinishInstall`）。

| 入れたビルド | どうなるか |
| --- | --- |
| **本体だけが新しい**（殻の ID が同じ） | 載っている本体を降ろすだけ。**再起動を尋ねません。** 次の取り込み・次の PIO リセットで新しいファイルが読み直されます |
| **殻まで変わった** | 読み込めるのは次の起動だけなので、従来どおり「再起動」を尋ねます（下記） |
| **判断できない**（同梱スクリプトが古く ID を出さない等） | **安全側＝再起動を尋ねる**へ倒します |

判断の材料は**殻の ID**（`VW_SHELL_ID`）です。CMake が「殻に入るものだけ」のハッシュを
計算してビルドへ焼き（`CMakeLists.txt` の `VW_SHELL_INPUTS`）、同じ値をインストール物にも
控えます（mac: `Info.plist` の `VWShellId`、win: `<name>.shell-id`）。**どのビルドが入って
いるか**も同じやり方で控えます（mac: `VWBuildCommit` / `VWBuildBranch`、win: `<name>.commit` /
`<name>.branch`）——`q-dev` がそれを `installed=` / `installed-branch=` として出し、開発版の
流れは「いま」をそこから決めます（上記）。`do-install` は入れ終えた
ビルドの ID を `installed-shell=<id>` として出し、プラグインは自分に焼かれた値と突き合わせ
ます（`src/UpdaterParse.h` の `NeedsRestartAfterInstall`。純粋関数なので単体テスト済み）。

ハッシュは**改行を正規化してから**取ります。Windows のランナーは CRLF でチェックアウト
するので、生のバイト列をそのまま混ぜると同じコミットから mac と Windows で違う ID が出ます
（実測しました）。判定は必ず同じプラットフォームの中で行われるので実害はありませんが、
**同じソースなら同じ ID** のほうが追いやすいので揃えてあります。

`VW_SHELL_INPUTS` に入れる基準はひとつ——**「起動のときにしか読まれないか」**です。
`draw/` や `parse/`、そして同梱スクリプトは**入っていません**。つまり描画やパースや
スクリプトをいくら直しても殻の ID は動かず、逆に境界（`src/PayloadAbi.h`）や登録まわりを
直せば必ず動きます——後者が肝で、**版の食い違った殻と本体を組ませない**ための歯止めに
なっています。**一覧は最小限に保ってください**。1 つ余計に入れるたびに、そこを直した
だけで利用者へ再起動を強いることになります。

その裏返しとして、**走っている殻より新しいスクリプトが来うる**ことになります（本体だけを
入れ替えた直後がそれ）。機械可読な出力の形（`installed=` / `build` 行 /
`installed-shell=` / `ok` / `error=`）は、**新旧どちらの組み合わせでも通るよう**に保って
ください。逆向き（古いスクリプト × 新しい殻）は以前からある前提で、そのために配置は
インストーラへ委ねてあります（上記）。

実際に本体が読み直されるのは、次に本体を使うとき（取り込みコマンド・PIO のリセット）です。
入口ごとに同梱ファイルの印（大きさ・更新時刻）を見て、読んだときと違っていれば降ろして
読み直します（`src/PayloadSession.cpp`）。**本体のコードがスタックに載っている間は決して
降ろしません**——降ろした瞬間にそのコードと静的データが消えるためです。

### 殻まで変わったときの再起動

コンパイル済みの殻は起動時にしか読み込まれないため、インストールしただけでは新しい殻は
動きません。そこでこの場合の表示は**通知ではなく質問**にしてあり、**「再起動」ボタン**を
その場に出します（`src/UpdaterFlow.cpp` の `OfferRestart`）。

- **「再起動」** → **Vectorworks 自身に終了と起動し直しを頼みます**（`src/Updater.cpp` の
  `CVectorworksUpdaterHost::Restart` → SDK の
  `CloseAllFilesAndQuitVectorworks(bAskForSave: true, bRestart: true)`）。開いている
  ファイルは**通常どおり保存を確認**してから閉じられ、保存ダイアログで取り消せば
  Vectorworks は落ちません（その場合もインストール済みのファイルはディスクに残るため、
  次回の起動で反映されます）。
- **「後で」** → 何もしません。反映は次に Vectorworks を起動したときです。

インストールに失敗したときは（当然）再起動を尋ねず、失敗の理由だけを表示します。再起動を
**頼めなかった**とき（SDK をまだ掴めていない）は「手動で再起動してください」と案内します
——押しても何も起きないように見えるのを避けるためです。

#### 以前は SDK に頼めなかった（実機で確かめた失敗と、その前提が消えた経緯）

かつてこの再起動は、**終了要求も起動し直しも切り離した（detached）ヘルパープロセス**に
任せていました。SDK の `CloseAllFilesAndQuitVectorworks` が macOS 実機で次のように
失敗したためです。

1. `bRestart: true`（終了＋再起動を SDK に任せる）→ 古いインスタンスが終了しきる前に新しい
   インスタンスが立ち上がり、**「サポートファイルの読み込みに失敗しました。」**で落ちる。
2. `bRestart: false`（終了だけ SDK に任せ、起動し直しは自前）→ **同じダイアログが出る**。

原因は呼ぶ**時機**でした。当時のアップデート確認は**プラグインのロード中**（スプラッシュ
表示中、`plugin_module_main` の中）に走っており、Vectorworks 本体がまだ自分を終了させ
られる状態になっていなかったのです。SDK には「起動完了後に実行する」フックが無く
（`RegisterNotificationProcedure` の通知一覧にも起動完了に相当するものは無い）、いつ呼べば
安全かを当てにいくのは筋が悪いので、OS 経由の通常の終了要求（macOS: `quit` Apple event、
Windows: `CloseMainWindow()`）を送るヘルパーへ逃がしていました。

**その前提は M23 で消えました。** 確認の入口が起動時からメニューコマンドと取り込み
コマンドへ移り（上記「いつ確認するか」）、**Vectorworks が完全に動いている最中にしか
呼ばれなくなった**ので、素直に SDK へ頼めます。ヘルパーの一式（`MacRelaunchCommand` /
`WinRelaunchCommand` / `PowerShellQuote` / `MacAppBundleFromExecutable` と、それらが
組み立てる shell / PowerShell を検証していたテスト）はまとめて削除しました。

> **もし将来また起動時に確認したくなったら、この失敗を思い出してください。** SDK の
> 終了は「Vectorworks が動いていること」を前提にしています。

新しいビルドが実際にロードされるのは、この再起動（または手動での再起動）以降です。

インストールそのものは OS ごとに事情が違います。macOS ではバンドルの隔離解除とアドホック
再署名をスクリプトが行います（**本体 `.vwpayload` にも個別に必要**です——バンドルの外に
あるので `--deep` に含まれず、署名が無いと Apple Silicon の検証で `dlopen` に失敗します）。
Windows では実行中の `.vlb` を削除できない（メモリにマップされている）ため、スクリプトは
古い `.vlb` をいったん退避（リネーム）してから新しいものを書き込みます。退避ファイル
（`*.old-*`）は次回の更新時に掃除します。

**同じ事故はもう起こりません。** M21 で本体（`.vwpayload`）が増えたときは、更新を実行
するのが「いま入っているビルドに同梱されていたスクリプト」＝割る前の版で、`.vwpayload`
の存在を知らなかったため、zip に入っているのにコピーされませんでした（殻だけが新しく
なり、起動後に「本体が見つかりません」と出る）。いまは配置を**落とした zip の中の
インストーラ**が行うので、増えたファイルはそのまま入ります（上記「探すのは同梱
スクリプト、置くのはリリース側のインストーラ」）。

ただし**この仕組みより前のビルドから更新する 1 回**は、まだ古い同梱スクリプトが配置を
行います（そのときのファイル構成は変わっていないので、正しく入ります）。その更新で
新しい同梱スクリプトが入り、以後は常にリリース側のインストーラが配置します。

**本体（`.vwpayload`）は実行中でも置き換えられます。** 殻は同梱ファイルそのものではなく
一時ディレクトリへ複製したものを読み込むので（`src/PayloadHost.h`「必ず複製してから読む」）、
インストール先のファイルは常に空いています。スクリプトは別名へ書いてから `mv` する
——**途中まで書かれたファイルを走行中の Vectorworks に掴ませない**ためです。

プラグイン経由の更新は、**実行中のモジュール自身が置かれているフォルダ**（＝Vectorworks
が実際に読み込んだ `Plug-Ins`）へインストールします（`src/Updater.cpp` が自分のパスを
解決し ― macOS は `dladdr`、Windows は `GetModuleFileName` ― `VW_PLUGINS_DIR` として
スクリプトに渡します）。ユーザフォルダを既定と違う場所に設定していても、読み込まれて
いるコピーを直接置き換えるので更新が確実に反映されます。なお手動 CLI（下記）は既定
パスを使うため、ユーザフォルダが独自の場合は `VW_PLUGINS_DIR` を実際の場所に合わせて
実行してください。

リポジトリは公開なので、認証や追加ツールは不要です。各スクリプトは OS 標準のものだけ
を使います — macOS は `curl`・`plutil`・`unzip`・`codesign`・`xattr`・`osascript`
（`osascript` は下記の手動 CLI パスのみ）、Windows は PowerShell 組み込みの
`Invoke-RestMethod` / `Invoke-WebRequest` / `Expand-Archive`。

プラグインを経由せず、スクリプトを直接実行することもできます（手動確認・トラブル
シュート用。macOS の CLI パスは osascript ダイアログ、Windows の CLI パスはコンソール
プロンプトを使います）:

```sh
# --- macOS (bash) -----------------------------------------------------------
# 配置だけを行うインストーラ（リリースのアセットにもあるので、単独で落として実行できる）:
./scripts/vw-install.sh                        # 最新の stable を入れる
./scripts/vw-install.sh --tag dev-feature-x    # そのプレリリースを入れる
./scripts/vw-install.sh --zip <file>           # 手元の zip から入れる
# 取り除く（リリースのアセットにもある）:
./scripts/vw-uninstall.sh                      # 既定の場所から取り除く
./scripts/vw-uninstall.sh --name min-nano_structureDev
# stable チャンネル（main → min-nano_structure）:
./scripts/vw-update.sh stable
# dev チャンネル — どのブランチのビルドを入れるか選ぶ（→ min-nano_structureDev）:
./scripts/vw-update.sh dev
# 引数なし（または Finder でダブルクリック）: 最初にチャンネルを尋ねます。
./scripts/vw-update.sh
# プラグインが内部的に使う非対話モード（ダイアログなし・機械可読出力）:
./scripts/vw-update.sh q-stable                # stable の状態を表示
./scripts/vw-update.sh q-dev                   # dev ビルド一覧を表示
./scripts/vw-update.sh do-install <url> <name> # ダウンロードしてインストール
```

```pwsh
# --- Windows (PowerShell) ---------------------------------------------------
# 配置だけを行うインストーラ（リリースのアセットにもある）:
powershell -ExecutionPolicy Bypass -File scripts\vw-install.ps1
powershell -ExecutionPolicy Bypass -File scripts\vw-install.ps1 -Tag dev-feature-x
powershell -ExecutionPolicy Bypass -File scripts\vw-uninstall.ps1
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 stable
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 dev
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1          # チャンネルを尋ねる
# 非対話モード（プラグインが使うもの。stable/dev/do-install は sh 版と同じ契約）:
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 q-stable
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 q-dev
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 do-install <url> <name>
```

環境変数で上書き可能: `VW_REPO`（owner/repo）、`VW_PLUGINS_DIR`（インストール先）。
2 つのチャンネルは別名のプラグインをインストールするので、stable と dev が互いを
上書きすることはありません。
