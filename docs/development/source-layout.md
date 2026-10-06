# ソースの構成

処理は **IFC 解析フェーズ（`src/parse/`）** と **VW 描画フェーズ（`src/draw/`）** に完全分離し、
両者は命令セット（`src/core/Document.h`）だけで接続します。`parse/` と `core/` は
**SDK を一切 include しない**ので、SDK 無しでコンパイル・単体テストできます
（設計の詳細は [「設計の考え方」](../dev-notes/design/architecture.md)）。

**ビルドの成果物も 2 つに割れています**（こちらはフェーズ分離とは別の軸）。

```
Vectorworks ──読み込む──▶ 殻 <name>.vwlibrary / .vlb    … 起動時に 1 度きり
                              │ dlopen / LoadLibrary
                              ▼
                          本体 <name>.vwpayload          … いつでも読み直せる
```

**殻**に入るのは「Vectorworks に番地を握られるもの」だけ——メニュー 4 つ（うち 2 つは
開発版だけ）と PIO 2 つ、パレット 1 つ（開発版だけ）の*登録*、
自動アップデート、そして本体を読み込む仕掛け（`src/PayloadHost.*` / `src/PayloadSession.*`）。
**本体**に `core/` `parse/` `draw/` のすべてが入ります。境界は C の ABI
（`src/PayloadAbi.h`）1 枚きりです。

こう割ってあるのは、**アップデートに Vectorworks の再起動を要らなくする**ためです
（[「自動アップデートの仕組み」](auto-update/README.md)／[SDK リファレンス「プラグインモジュールの読み込みと
入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。

```
CMakeLists.txt              macOS / Windows 両対応の CMake ビルド。SDK 非依存の
                            静的ライブラリ MinNanoStructureCore（core/ + parse/）と、
                            SDK 依存の殻（.vwlibrary / .vlb）と本体（.vwpayload。
                            draw/ ほか。MinNanoStructureCore をリンクする）に分かれる
src/
  ModuleMain.cpp            モジュールのエントリポイント。拡張機能を登録し、本体へ
                            渡す CallBackPtr を預ける（**アップデートの確認はここでは
                            しない**——「自動アップデートの仕組み」）
  PayloadAbi.h              **殻と本体の唯一の約束事**（C の ABI）。両方が include する
                            ので SDK にもプラットフォームにも依存しない
  PayloadHost.{h,cpp}       殻の側: 本体を一時ディレクトリへ複製して dlopen /
                            LoadLibrary し、版を確かめ、呼び、降ろす
  PayloadSession.{h,cpp}    殻の側: 「いま載っている本体」1 つと、入口ごとの載せ替え
                            判定（同梱ファイルの印が変わっていたら読み直す）
  PayloadHostHolder.h       本体の側: 殻から渡された構造体を**その場で写す**入れ物
                            （持ち続けると実機で落ちる。回帰テストあり）
  payload/
    PayloadMain.cpp           本体のエントリポイント。GS_InitializeVCOM を自分で呼び、
                              取り込み・実機テスト・MCP ブリッジ・PIO のリセットを中の
                              実装へ取り次ぐ
  Extensions/               **殻**に残る「登録」だけ（実処理は draw/ 側）
    ExtMenu.{h,cpp}           「IFC (ホームズ君) 取り込み…」メニューコマンドの登録と、
                              本体の draw::runImportCommand への取り次ぎ。実行の頭で
                              静かなアップデート確認も行う。**実機テストのことは何も
                              知らない**（M25。それは ExtTestMenu の仕事）
    ExtTestMenu.{h,cpp}       「実機テストを実行…」メニューコマンドの登録と、本体の
                              draw::runTestRound への取り次ぎ（**dev だけ登録**。M25）
    ExtUpdateMenu.{h,cpp}     「アップデータを確認 (みんなの構造設計支援)」メニュー
                              コマンドの登録と実行（**殻に残る唯一の実処理**である
                              自動アップデートを呼ぶ）
    ExtMcpMenu.{h,cpp}        「MCP ブリッジを表示…」メニューコマンドの登録（パレットを
                              出すだけ。**dev だけ登録**）
    ExtMcpPalette.{h,cpp}     MCP ブリッジを常駐させる**モードレスなパレット**の登録と、
                              JS の時計から本体の draw::serveMcpBridge への取り次ぎ（M30）。
                              本体から頼まれた更新・再起動をここで済ませる（M38）。
                              **dev だけ登録**。中身は resources/common.vwr/html/mcp.html
    ExtColumnMark.{h,cpp}     柱・小屋束の記号 PIO の登録（パラメータ定義・UUID）と、
                              本体の draw::recalculateColumnMark への取り次ぎ
    ExtShearWall.{h,cpp}      耐力壁 PIO の同上（→ draw::recalculateShearWall）
  core/                     フェーズ非依存の土台（SDK も STEP も知らない純粋コード）
    Document.{h,cpp}          命令セットの構造体定義・validateDocument・描画結果の件数
    ImportOptions.{h,cpp}     取り込み設定（配置するシンボルの対応・図面枠のスタイル・
                              寸法規格・伏図のまとめ方・軸組図から外す通り・垂木の断面）と
                              シンボルの役割の表 1 つ
    FeedbackScratch.{h,cpp}   実機テストの一時ファイルの置き場（ブランチごと）と、
                              PR が閉じたブランチの片付け
    FeedbackSession.{h,cpp}   実機テストで覚えておく値（1 周目の選択・前の周の内訳・
                              テンプレートと自分で保存した図面）とその読み書き、
                              閉じてよい図面の判定、報告の置き場所
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
    Feedback.{h,cpp}          実機テストの報告（内訳・前の周との差分・図面の状態・
                              診断ログの切り詰め）
    Grid / Story / Floor / Member / Noboribari / PlanLevel / Column / Rafter / Roof /
    Footing / AnchorBolt / FloorPost / FireBrace / Joint / Splice / ColumnMark /
    ShearWall / Sheet / Tag / Section / Dimension   要素ごとの解析
  draw/                     Phase 2: VW 描画（SDK 依存）。**まるごと本体に入る**
    ImportCommand.{h,cpp}     本番の取り込みコマンド（ファイル選択 → 設定 → 取り込み →
                              完了ダイアログ）。**実機テストの分岐が 1 つも無い**（M25）
    ImportRun.{h,cpp}         取り込み 1 周ぶんの部品——ファイル選択・ビルドの素性・
                              解析 → 描画 → 集計。**本番の取り込みと実機テストが
                              共有する唯一の実装**で、診断ログの見出し・区切り・結果も
                              ここが書く（M25）
    Feedback.{h,cpp}          実機テストの 1 周（runTestRound）——記憶・取り込み前の
                              ダイアログ・図面の用意・報告の書き出し。メニューと MCP の
                              vw_run_test の両方がここを通る。**実機テストを知っているのは
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
                              触るのはこの表 1 行**。殻に頼む道具は引き取って殻へ渡す）
    ProgressDialog.{h,cpp}    core::ProgressReporter を VW の進捗ダイアログへ橋渡し
    ResultDialog.{h,cpp}      完了・エラーのダイアログ（短い本文＋折り畳んだ診断ログ欄）
    SettingsDialog.{h,cpp}    取り込み設定ダイアログ（配置するシンボルを名前と絵で選ぶ）
    SectionPickDialog.{h,cpp} 軸組図にする通りの選択（外した通りは軸組図を描かない。M34）
    HostServices.{h,cpp}      殻から借りた道具（同梱スクリプトの実行）の置き場所（いまの
                              使い手は無い。M38）
    Symbol.{h,cpp}            ハイブリッドシンボルの配置（アンカーボルト・床束・火打・仕口・
                              継手の 5 要素で共有する唯一の実装）
    Tag.{h,cpp}               断面寸法データタグ（伏図・軸組図で共有。スタイルは当てず、
                              タグの中身はタグ 1 本ずつへ直接組む）
    DrawingLabel.{h,cpp}      軸組図の図面ラベル（真下の中央へ図面タイトルだけを出す。
                              スタイルは当てず、既定のレイアウトを複製して組み直す）
    TitleBlock.{h,cpp}        図面枠（伏図・軸組図で共有。図面にあるスタイルを当てる
                              だけで、スタイルは作らない）
    Grid / Story / Floor / Member / Column / Rafter / Roof / Footing /
    ColumnMark / ShearWall / Sheet / Legend / Section / Dimension   要素ごとの描画
  Updater*.{h,cpp}          同梱した更新スクリプトを起動してアップデートを駆動する
                            （同梱スクリプトの実行は本体へも貸し出す）
  BuildConfig.h             stable / dev の識別切り替えスイッチ（VW_DEV_BUILD）
  PluginPrefix.h            共有プレフィックスヘッダ（SDK を取り込む）
  Module-Info.plist.in      バンドルの Info.plist テンプレート（macOS 専用）
tests/                      無 SDK の単体テスト（詳細は tests/README.md）
  TestFramework.h           依存ゼロの極小テストハーネス
  Fixtures.h / StepText.h / RoofSample.h
                            共有するフィクスチャ読み込み・近似比較・合成 STEP テキストの
                            組み立て・試験用屋根面
  fixtures/                 ホームズ君 EX 出力の実 IFC・最小の合成 IFC・実機テストの
                            テンプレート（Default.sta）
resources/
  min-nano_structure.vwr/…           stable プラグインのメニュー・PIO の文字列
  min-nano_structureDev.vwr/…        dev プラグインのメニュー・PIO の文字列
  common.vwr/…                       両方に共通の中身（MCP ブリッジのパレットの
                                     html/mcp.html。M30。パレットを登録するのは dev
                                     だけ）。包む直前に各 .vwr の写しへ重ねる
                                     （CMakeLists.txt）
scripts/
  vw-update.sh              CI ビルドを探して落としてくる（macOS 用。バンドルに同梱
                            され、プラグインから起動される）。**配置はしない**
                            ——落とした zip の中の vw-install.sh へ委ねる
  vw-update.ps1             同上の Windows 版（PowerShell。.vlb の隣に同梱される）
  vw-install.sh             **配置の手順**（macOS 用）。配布 zip の直下に同梱され、
                            リリースのアセットとしても公開される。手動インストールの
                            入口でもある（「自動アップデートの仕組み」）
  vw-install.ps1            同上の Windows 版
  vw-uninstall.sh           **取り除く手順**（macOS 用）。zip の直下に同梱され、
                            **インストール先へ一緒に置かれる**——次のアップデートが
                            「前の版を、その版自身の知識で取り除く」ために使う
  vw-uninstall.ps1          同上の Windows 版
  vw-token.sh / .ps1        **GitHub のトークンの在り処**（探索順・gh の探し場所）。実行は
                            せず、vw-update が source する——読むほうにも必ず付ける
                            （認証なしの GitHub API は IP ごとに 1 時間 60 回。M27）
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
  mcp/vw-mcp-server.py      MCP ブリッジの Claude 側（依存の無い Python。リポジトリ直下の
                            .mcp.json が登録する。配布 zip へも同梱され、インストール先へ
                            一緒に置かれる）
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
                            調査は SDK リファレンス側で行う（「CI デバッグ」）
```

**依存の向きは厳守します。** `parse/` と `core/` は Vectorworks SDK を include せず、
`draw/` は STEP / IFC を include しません。両者をつなぐのは `core/Document.h` だけで、
この規律は CI（`core/` `parse/` を無 SDK でコンパイル・テストするジョブ）が担保します。
