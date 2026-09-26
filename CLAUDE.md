# CLAUDE.md

このファイルは Claude Code（claude.ai/code）がこのリポジトリで作業するとき、および自動レビュー
（`pr-review.yml`）が差分を見るときの**共通の規約**です。**この指示は既定の挙動より優先されます。**

領域ごとの細則は別のドキュメントにあります。**その領域に触れる変更のときだけ**、下記
「領域ごとの規約」の表が指す節を読んでください。

## このリポジトリについて

**構造設計に使う機能をまとめて収める**、VectorWorks 2026 用の **C++ SDK 製ネイティブ
プラグイン**です。表示名は**みんなの構造設計支援**、ファイル名・フォルダ名は
`min-nano_structure`（dev ビルドは `…Dev`）。**機能ごとにプラグインを分けず、この 1 つへ
コマンドと PIO を足していきます。**

| 入口 | 種類 | 概要 |
| --- | --- | --- |
| IFC (ホームズ君) 取り込み… | メニュー | **ホームズ君構造EX** の木造軸組 IFC をパースし、ネイティブオブジェクトへ変換して配置する（主機能） |
| アップデータを確認 (みんなの構造設計支援) | メニュー | 新しいビルドの確認と入れ替え |
| MCP ブリッジを表示… | メニュー | Claude から図面を読むためのパレット（開発・デバッグ用。道具は読むだけ） |
| 柱記号 / 耐力壁 | PIO | 取り込みが置くプラグインオブジェクト |
| 実機テストを実行… | メニュー（**dev だけ**） | 取り込みを実機テストとして走らせ、結果を PR へ投稿する |
| 実機フィードバックの往復 | パレット（**dev だけ**） | 新しい dev ビルドが出るたびに入れて取り込み直し、結果を投稿する |

## ドキュメントの分担

| ファイル | 中身 |
| --- | --- |
| `README.md` | 利用者向け。何をするか・使い方・インストール・既知の制限 |
| `docs/DEVELOPMENT.md` | 開発ガイド。ソースの構成・**置き場所の一覧**・ビルド・テスト・lint・CI（待ち方・デバッグ）・自動レビュー・MCP ブリッジ・実機フィードバックの往復・自動アップデート |
| `docs/DEV-NOTES.md` | 開発メモ。設計の考え方・ホームズ君 IFC の癖・打ち切った調査・実装の経緯（M0〜） |
| [SDK リファレンス](https://github.com/min-nano/vectorworks-developer-sdk-reference)の `Findings/` | **VW SDK の実測知見**（実機でしか判明しない落とし穴・SDK に無い／効かない API・SDK 側の打ち切った調査）。別リポジトリ |
| `tests/README.md` | テストの一覧・方針・テストしていないもの |
| `CLAUDE.md`（本ファイル） | 全変更に共通する規約 |

**新しく分かったことは書き残す。** 行き先は 2 つで、取り違えない。

- **Vectorworks SDK の挙動** → SDK リファレンスの `Findings/`。本リポジトリには書かない。
- **本プラグイン固有のこと**（設計判断・ホームズ君 IFC の癖・描き方の方針） → `docs/DEV-NOTES.md`。

**どちらかの「打ち切った調査」に書いてあることは再調査しない。**

### 領域ごとの規約（触るときに読む）

| 触るところ | 読む節 |
| --- | --- |
| 共有する定数・述語・ヘルパーを足す／探す | `docs/DEVELOPMENT.md`「置き場所の一覧（重複を作らない）」 |
| 往復（`draw/Feedback`・`src/FeedbackLoop*`・`ExtTestMenu`・`ExtFeedbackPalette`・`scripts/vw-feedback.*`） | `docs/DEVELOPMENT.md`「実機フィードバックの往復」の「設計の決めごと」 |
| 往復のコメント（`<!-- homeskz-ifc-feedback … -->`）が PR に届いた | `docs/DEVELOPMENT.md`「実機フィードバックの往復」の「届いたコメントの読み方」 |
| 自動アップデート（`src/Updater*`・`scripts/vw-update.*` / `vw-install.*` / `vw-uninstall.*` / `vw-token.*`） | `docs/DEVELOPMENT.md`「自動アップデートの仕組み」 |
| MCP ブリッジ（`core/Bridge`・`draw/McpBridge`・`scripts/mcp/`） | `docs/DEVELOPMENT.md`「MCP ブリッジ」 |
| CI を待つ・`ci-debug` を使う | `docs/DEVELOPMENT.md`「CI の完了待ち」「CI デバッグ」 |
| 自動レビュー（`pr-review.yml`） | `docs/DEVELOPMENT.md`「自動レビュー」 |
| 実機での確認のしかた | `docs/DEV-NOTES.md`「実機確認の作法」 |

## 開発の基本方針

1. **小さく機能追加を重ねる。** 実描画は**ローカルの VectorWorks でしか最終確認できない**。
   1 変更＝1 要素（または 1 サブ機能）とし、「IFC パース → 命令セット → VW 描画 → 実機確認」の
   1 周が回る縦切りで完成させる。

2. **2 つの分割を崩さない。** **2 フェーズ分離**（解析と描画）と**殻と本体の分割**（ホット
   リロード）が設計の核で（下記「アーキテクチャ」）、これを崩す変更は他がどれだけ良く見えても
   採らない。

3. **仕様の根拠を残す。** なぜその値・その作りなのかはコードのコメント（**なぜ**を書く）と
   `docs/DEV-NOTES.md` に、SDK の落とし穴は `Findings/` に残す。

4. **既存の図面リソースを作らない・書き換えない。** 利用者の図面に名前付きリソースを増やさない。
   スラブ／ウォールスタイル・データタグスタイル・凡例スタイルは作らず、構成層・基準面・
   タグレイアウトは**各オブジェクトへ直接**与える。

   **唯一の例外は耐力壁の伏図記号のシンボル定義**（`耐力壁記号_筋かい` / `耐力壁記号_面材`。
   `Extensions/ExtShearWall.h`）。図面側で 1 か所を編集すれば全耐力壁の記号を差し替えられる
   ように、というご要望による。次を満たせないなら例外にしない:
   * **描く対象があるときだけ作る**（耐力壁が 1 枚も無い図面には作らない。下記 5）。
   * **同じ名前の定義が既にあれば触らない**（利用者が編集した絵を尊重する）。
   * 作った定義には**必ず `ResetObject` を呼ぶ**（呼ばないと外接が計算されず空に見える。
     [Findings「Symbols」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Symbols.md)）。

   図面枠は**スタイルを当てるが作らない**（利用者の図面にあるスタイルを名前で指すだけで、
   無ければ置かない）。

5. **空のもの（レイヤ・レベル・凡例）を先に作らない。** 描画対象がある要素にだけ作る。

6. **決定性を守る。** エンティティ列挙順に依存しない結果を出す。ソート・集約は明示的に。

7. **SDK の調査はリファレンス側で行う。** 「この API は SDK にあるか」「どう振る舞うか」が
   分からないまま実装に入らない。[SDK リファレンス](https://github.com/min-nano/vectorworks-developer-sdk-reference)で
   issue を立て、`Findings/` に反映されてから、その知見を根拠に実装する（待つ間は SDK に
   依らない作業を進める）。本リポジトリの `ci-debug` の `sdk-grep` / `sdk-ls` は、`Findings/`
   に載っている宣言を写し取るときだけ使う。

8. **利用者のものを消すコードは 2 か所だけで、歯止めを緩めない。** アンインストーラ
   （`scripts/vw-uninstall.*`。フォルダ名が一致し中に殻があるときだけ消す）と、往復の図面の
   戻し（`draw/Feedback` の `prepareDrawingForRound` → `draw/DrawUtil` の
   `RemoveCreatedLayers`。前の周が自分で作ったレイヤだけ消す）。どちらも回帰テストで押さえて
   あり、安全弁を緩める方向へ変えない。

## アーキテクチャ: 2 フェーズ分離

処理は **IFC 解析フェーズ** と **VectorWorks 描画フェーズ** に完全分離し、**命令セット
（Document）**だけで接続する。

```
IFC ファイル
   │  Phase 1: parse  （SDK 非依存 = VectorWorks SDK を include しない）
   ▼
Document（命令セット。プレーンな構造体の集まり）
   │  Phase 2: draw   （VectorWorks SDK のみに依存）
   ▼
VectorWorks ネイティブオブジェクト
```

### Phase 1 — `parse/`: SDK 非依存

- **VectorWorks SDK を一切 include しない。** 通常の C++ ツールチェインだけでコンパイル・
  テストできる。
- IFC は**自前の最小 STEP リーダ**（`parse/Step`）で読む。**幾何エンジン（OpenCASCADE 等）は
  使わない**——配置行列・断面・押し出しは `core/Geometry` ＋ `parse/IfcGeometry` で自前計算する。
- 読み込み時のサニタイズは持たない。ホームズ君の IFC2X3 に混入する IFC4 専用エンティティ
  （`IFCFOOTINGTYPE`）も、スキーマ検証をしない STEP リーダはそのまま読み、参照しないので無害。
  `parse/Loader` はファイル → STEP グラフだけを担う。
- 出力は Document。SDK ハンドルや STEP エンティティのポインタなど、フェーズ間で運べないものを
  入れない。

### Phase 2 — `draw/`: VectorWorks SDK 依存

- **SDK のみに依存**し、IFC / STEP の知識を持たない。
- Document を検証（`validateDocument`）してから SDK API で描画する。

### 命令セット（Document）

- **プレーンな構造体**（`std::vector`・`std::string`・`double`・`enum` 等）で表す。スキーマは
  `core/Document.h` の `Document` の定義が正。
- **同型が並ぶところは構造体 1 つへまとめる**（`anchorBolts` / `floorPosts` / `fireBraces` /
  `joints` は `core::SymbolCommand` 1 つで受け、区別は「どのリストか」が担う）。
- **突き合わせが要る関係は入れ子で持つ**（データタグは `ViewportCommand::tags`、凡例は
  `SheetCommand` の中）。平らに並べて番号で突き合わせない。
- **描くときにしか決まらないものは命令に持たせない。** 用紙の大きさはシートレイヤから SDK で
  読むので、それに依る値（縮尺・用紙上の位置・軸組図の枚数）は解析側で決めない。決め方
  そのものは SDK と無関係な算数なので `core/Layout` に置き、無 SDK でテストする。
- 受け渡しは**構造体のまま**。JSON 直列化は予定に無い（ダンプが実際に要る場面が出たら最小限を
  足す）。
- スキーマを変えるときは、構造体定義・`validateDocument`・テストを同時に更新する。

### 依存の向きは厳守する

`parse/` と `core/` は VectorWorks SDK を include しない。`draw/` は STEP / IFC を include
しない。両者をつなぐのは `core/Document.h` だけ。この規律は CI が **parse/core を無 SDK で
コンパイル・テストする**ことで担保している。

## アーキテクチャ: 殻と本体（ホットリロード）

2 フェーズ分離とは別の軸で、成果物が 2 つに割れている。目的は**アップデートに VectorWorks の
再起動を要らなくする**こと。境界は C の ABI（`src/PayloadAbi.h`）1 枚きり。

```
VectorWorks ──読み込む──▶ 殻 <name>.vwlibrary / .vlb   … 起動時に 1 度きり
                              │ dlopen / LoadLibrary
                              ▼
                          本体 <name>.vwpayload         … いつでも降ろして読み直せる
```

コンパイル済みプラグインは起動時にしか読み込まれず、差し替えられない
（[Findings「Plug-in Modules」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。
そこで実処理を VectorWorks が知らない別モジュール（本体）へ出し、殻が自分で読み込む。

| | 入るもの | 入れないもの |
| --- | --- | --- |
| **殻**（`src/ModuleMain.cpp` / `src/Extensions/` / `src/Updater*` / `src/FeedbackLoop*` / `src/Payload{Host,Session}.*`） | VectorWorks に**番地を握られる**ものの**登録**（メニュー・PIO・パレットの `SMenuDef` / `SParametricDef` / パラメータ定義 / UUID）・本体の読み込み・**自動アップデート**（本体を置き換える当人が本体の中にいては足元を外す）・**往復の駆動**（入れ替えを起こす側。`src/FeedbackLoop`） | **これ以外の実処理**。解析・描画・PIO の作図は置かない。パレットは判断を持たない（JS は殻の駆動を叩いて返った文言を並べるだけ） |
| **本体**（`src/payload/` / `src/draw/` / `src/parse/` / `src/core/`） | それ以外すべて（両フェーズまるごと） | 登録の定義（`.vwr` の文字列を引くもの）。本体は `.vwr` を持たない |

- **殻に実処理を足すと、そこを直すたびに利用者へ再起動を強いる**ことになり、ホットリロードの
  前提が静かに崩れる。
- **殻が本体へ貸すのは「同梱スクリプトの実行」だけ**（`VwPayloadHost` → 本体側は
  `draw/HostServices` に写して持つ）。貸すものを増やすのは本体でしかできないことのためだけで、
  殻でできることは殻でやる。
- **新しい入口（メニュー・PIO）は、登録を殻に、絵と処理を `src/draw/<要素>Pio.{h,cpp}` に置く。**
  殻の `Recalculate()` は `PayloadUse` で本体を確保して取り次ぐだけ（`ExtColumnMark` /
  `ExtShearWall` に倣う）。
- **境界に口を足したら `VW_PAYLOAD_ABI_VERSION` を必ず上げる**（殻と本体は別々に配られるので、
  食い違いは実行時にしか気付けない）。
- メニューコマンドのカテゴリは `.vwr` の `"category"` 1 つを全メニュー定義が引く（プラグイン名で
  揃える）。

### 破ってはならない決めごと

1. **境界は C の ABI に保つ。** 例外・C++ のオブジェクト・vtable・`std::string` を跨がせない
   （降ろした瞬間にそのモジュールのコードと静的データが消える）。
2. **境界を越えて来た構造体は、受け取った側がその場で写す。** 渡した側も降ろすまで生かす——
   **両方やる**。落とすと実機で VectorWorks ごと落ちる（`src/PayloadHostHolder.h` と
   `tests/PayloadHostHolderTests.cpp`）。
3. **本体のコードがスタックに載っている間は降ろさない。** 入れ替えの判定は入口で、入れ子の
   深さが 0 のときだけ（`src/PayloadSession.h`）。
4. **本体は必ず一時ディレクトリへ複製してから読む**（Windows は読み込み中の DLL を置き換え
   られない）。
5. **本体をバンドルの中に置かない**（mac の署名がリソースまで封をする）。殻の隣に置く。
6. **殻の ID（`VW_SHELL_ID`）が「再起動が要るか」を決める。** `CMakeLists.txt` の
   `VW_SHELL_INPUTS` には**起動のときにしか読まれないもの（＝殻にコンパイルされるもの）だけ**を
   最小限に並べる。`draw/`・`parse/`・インストーラ／アンインストーラ・同梱スクリプト
   （`scripts/vw-*.sh` / `.ps1`。呼ぶたびにディスクから読み直される）は入れない——入れると
   そこを直すたびに再起動を強いる（M23 で実際にそうなった）。判断できないときは「再起動が
   要る」へ倒す（`src/UpdaterParse.h` の `NeedsRestartAfterInstall`）。
7. **殻は `MinNanoStructureCore`（core/ + parse/）をリンクしない**（殻に入れてよいものの境界が
   曖昧になる）。

### 更新と配置の要点

詳細は `docs/DEVELOPMENT.md`「自動アップデートの仕組み」。全変更で守るのは次の 3 つ。

- **起動時（`plugin_module_main`）に更新を確認しない。** 確認はコマンドの入口だけで行う。
  再起動を SDK（`CloseAllFilesAndQuitVectorworks`）に頼めるのは、確認が VectorWorks が完全に
  動いている最中にしか走らないからである。PIO のリセットからも確認しない（取り込み直後に
  数百回走る）。
- **インストールの経路は `src/UpdaterFlow.cpp` の 1 本だけ。** 別の都合で 2 本目を書かない。
- **同梱スクリプト（`scripts/vw-update.*`）にファイルの配置を書かない。** 走るのは常に
  インストール済みの古い版なので、配置は配布 zip 直下のインストーラ（`scripts/vw-install.*`）が
  「zip の直下にあるものをそのまま置く」。配布物を増減するときに触るのは CI の梱包だけ。
  走っている殻より新しいスクリプトが来うるので、機械可読な出力の形は新旧どちらの組み合わせ
  でも通るよう保つ。

## 置き場所の規約

`src/` の全体像は `docs/DEVELOPMENT.md`「ソースの構成」にある。

### 重複を作らない置き場所

**同じ定数・述語・ヘルパー・文言を 2 か所に書かない。** 既存の唯一の置き場所の一覧は
`docs/DEVELOPMENT.md`「置き場所の一覧（重複を作らない）」にあり、新しく共有するものを作ったら
そこへ 1 行足す。特に次は取り違えやすい:

- **要素を 1 つ足すときの型**: `parse/<要素>.{h,cpp}` ＋ `core/Document.h` の命令構造体と
  `validateDocument` の検証 ＋ `draw/<要素>.{h,cpp}` ＋ `tests/Parse<要素>Tests.cpp` ＋
  `parse/Summary.cpp` の `kElements` に 1 行。どれも本体側なので殻は変わらない。PIO を足す
  ときだけ `Extensions/Ext<要素>.{h,cpp}`（殻: 登録と取り次ぎ）＋ `draw/<要素>Pio.{h,cpp}`
  （本体: 作図）に割る。
- **要素の一覧**（表示名・助数詞・命令数・描けた数）は `parse/Summary` の `kElements` ただ 1 つ。
- **SDK へ渡す素の数値・SDK 呼び出しの定型**は `draw/DrawUtil`。**SDK と無関係な純計算**は
  `core/` へ寄せる。
- **解析側はシンボル名の固定値を持たない**（取り込み設定 `core/ImportOptions` から引く）。
- **診断ログへの書き出し口**は `core/Progress` の `beginPhase` と `Extensions/ExtMenu` の 2 か所
  だけ（各要素へ `trace::log` を撒かない）。
- **GitHub のトークンの在り処**は `scripts/vw-token.{sh,ps1}` だけで、GitHub を読む側にも
  必ず付ける（認証なしは IP ごとに 1 時間 60 回で、往復の確認がちょうど当たる。M27）。

### 本番の取り込みコマンドに往復を書かない

往復（実機フィードバック）を書いてよいのは dev だけの実機テストのコマンド
（`Extensions/ExtTestMenu` ＋ `draw/Feedback` の `runTestRound`）だけで、`draw/ImportCommand` と
`Extensions/ExtMenu` には 1 行も書かない。`#ifdef VW_DEV_BUILD` で囲っても制御フローは本番の
入口に残るので、囲えばよいとも考えない。両者が共有してよいのは**絵を作るところ**
（`draw/ImportRun` の `runImportRound`）だけ（M25）。往復のそのほかの決めごとは
`docs/DEVELOPMENT.md`「実機フィードバックの往復」の「設計の決めごと」。

## C++ コード規約

### フォーマット

`.clang-format` と `.clang-tidy` が**単一の真実**で、手で例外を作らない。

- `.clang-format`: タブインデント（幅 4）・Allman ブレース・名前空間の本体をインデント・
  ポインタ／参照は型側（`int* p`）・100 桁・**コメントは自動再整形しない**
  （`ReflowComments: false`。折り返しは著者責任）・**include は並べ替えない**
  （`PluginPrefix` / `BuildConfig` を先頭に保つ）。
- `.clang-tidy`: **警告をエラー扱い**。`core/` `parse/` は lint のジョブで、SDK 依存コードは
  `build.yml` の `tidy-mac` / `tidy-windows` で同じルールをかける。後者は `release` の `needs` に
  入っているので、**clang-tidy が通らなければリリースは公開されない**。
- コミット前に `scripts/lint.sh`（必要なら `--fix`）を通す（CI と同一のチェック）。

### 命名

- **名前空間**: トップは `HomeskzIfcImport`、フェーズは `::parse` / `::draw` / `::core`。
  プラグインを改名しても据え置く（図面にも配布物にも現れない内部の綴り）。
- **SDK 拡張クラス**は SDK の作法（`CExt…` / `…_EventSink`）に従う。
- **`core/` `parse/`**: 型は `PascalCase`、関数・変数は `camelCase`、定数は `kPascalCase` または
  `UPPER_SNAKE`（ファイル内で統一）。要素の組み立て関数は `build<要素>Commands`。
- **Document のフィールド名**は図面側の語彙に合わせる（`class` は予約語なので `drawClass` /
  `className`）。意味は各構造体の doc コメントに書く。
- **ユニバーサル名と拡張機能 UUID は付け替えない**（コマンド・PIO の同一性そのもので、付け替えると
  ワークスペースからコマンドが消え、図面上の既存オブジェクトが孤児になる）。一覧は
  `docs/DEVELOPMENT.md`「プラグイン識別子」。

### 幾何の型

- `parse/` は SDK の幾何型（`WorldPt3` 等）を使えないので、`core/Geometry.h` の `Vec2` / `Vec3` /
  `Mat4` を使い、数式もそこに置く。
- `draw/` での SDK 型との相互変換は 1 か所の薄いヘルパーに集める。

### エラーハンドリング・所有権

- STEP パースの失敗・想定外エンティティには**寛容に**ふるまう（スキップ・フォールバック描画）。
  **1 要素の欠損で全体を止めない。**
- 例外は parse 内部の局所処理に留め、フェーズ境界（`buildDocument` の戻り）は値で返す。SDK
  コールバック（`plugin_module_main` / `DoInterface`）へ例外を漏らさない。
- **RAII で所有権を明示**し、生ポインタで所有しない。SDK ハンドルは Document に載せず、描画で
  要る受け渡しは「命令インデックス → ハンドル」の対応（`draw/ObjectHandles`）で行う。

### コメント・言語

- **日本語コメントを基本**とし、既存ソースの手折りコメントの密度に合わせる。
- **なぜ（意図・仕様の根拠）を書く。** 大きな知見は、プラグイン固有なら `docs/DEV-NOTES.md`、
  SDK の挙動なら `Findings/` にも足し、コメントからはその置き場所を指す。

## テスト方針

一覧と「テストしていないもの」は `tests/README.md`。

- **`core/` `parse/`**: `tests/TestFramework.h` で**無 SDK の単体テスト**を書く。
  - **実 IFC フィクスチャ**（`tests/fixtures/`）に対してテストする。要素を足したら、全フィクス
    チャで `buildDocument` が例外なく通ること（＋決定性）を確かめる。
  - **期待値は手書きする。** 数値は許容誤差付きで比較する。
  - CI（`test.yml`）が ASan + UBSan 付き・カバレッジ付きで回す。
- **`draw/`**: SDK 依存で CI では実行できない。
  - **描画側から切り離せる純計算は `core/` へ寄せて**無 SDK でテストする（レイヤ順・地中梁の
    呑み込み・用紙の割り付け等）。
  - 実描画は**ローカルの VectorWorks で目視確認**する（`docs/DEV-NOTES.md`「実機確認の作法」）。
    SDK 呼び出しの薄いラッパーは、要るならモックで「正しい引数で呼んだか」を見る程度に留める。

### 検算は開発ビルドだけに置く

`draw/` は書いた値（ストーリバウンドの record・PIO のパラメータ・パス）を**書いた直後に読み
戻して命令と引き比べ**、食い違いを診断ログへ持ち帰る。これは往復で絵の破綻を数字から手繰る
ための足場で、利用者には不要なので **`#if VW_DRAW_VERIFY` で囲み、dev ビルドにだけコンパイル
する**（`src/draw/Verify.h`）。

囲むかどうかの基準はひとつ——**「外したら利用者の絵が変わるか」**。

- 囲む … 検算そのもの・その件数・実測を文字列にする道具（`DescribeSizeParams` /
  `DescribeStoryBound` / `DescribePioPath` / `DescribeParamsContaining`）・取り込み後の測り直し
  （`recheckColumns`）・パスの観測（`PathProbe`）。
- 囲まない … 読み戻した結果が絵を変えるもの（`SetParamRealChecked`・`CreatePath` の
  `NurbsSetPt3D`・`retryWithFreshPath`・データタグのレイアウトの取り直し・`draw/Symbol` の
  置けたことの確認）。

`#if` の中は dev の CI でしか型検査されないので、**両方の分岐がコンパイルできることは
`ci-debug` の `build`（既定の `VW_BUILD_CHANNEL=both`）で確かめる**。

## 既知の制限・非目標

- **ホームズ君 IFC 以外の汎用 IFC 対応は非目標**（既知サブセット前提）。
- そのほかの制限は `README.md`「既知の制限」と `docs/DEV-NOTES.md`「残っている宿題」。

## 開発プロセス: PR とマージ

1. **PR は自動で作ってよい。** 疑義が無ければ PR を作り、`subscribe_pr_activity` で CI と
   レビューを監視し、CI の失敗は原因を直して push する（待ち方は下記「CI の完了を待つ」）。
   * **下書き（draft）で作る**: 実機確認か設計判断が要る変更——`draw/` を含むもの・殻・境界・
     同梱スクリプトなど実機でしか確かめられないもの・方針をユーザーに決めてもらうもの。
     **迷ったら下書き。**
   * **本番（ready for review）で作る**: それ以外（下記 5）。
2. **下書きの間は自動レビューが走らない**（使用量を食うので、差分が動く段階では回さない）。
   CI と dev ビルドは走るので、往復はそのまま回る。下書きの間に Claude が頼まれずに
   `/code-review` 等を回すこともしない。
3. **下書きを本番へ昇格させるのは「マージしたい状態」になってから**——実機確認が済んだ
   （ユーザーが「確認できた」と言った）・設計判断が決着した・CI が green。GitHub MCP の
   `update_pull_request`（`draft: false`）で昇格させると、それがレビューの入口になる。指摘に
   対応し、承認が出たらマージへ進む。**昇格後に実機確認をやり直すなら、下書きへ戻してから
   直す**（戻さずに push を重ねるとそのたびにレビューが走る）。
4. **実描画が変わる変更（`draw/` を含む PR）は、ユーザーが実機で「確認できた」と言うまで
   マージしない。** CI green もレビューの承認も、往復の件数が揃ったことも実機確認の代わりには
   ならない（命令の数が合っていても絵が破綻していることは普通にある）。確認前にマージすると、
   不具合が出たときにどの変更が原因か切り分けられなくなる。
5. **実機確認の要らない変更は CI green（＋レビュー）でマージしてよい**——`core/` `parse/` だけ・
   テスト・ドキュメント・CI 設定など描画に触れないもの。迷ったら 4 に倒す。
6. **コミットメッセージ**には Claude セッション URL（`https://claude.ai/code/session_<ID>`）を
   入れる。

## 実機フィードバックの往復

dev ビルドは取り込みの結果を自分で PR へ投稿する（`<!-- homeskz-ifc-feedback v1 round=… -->`
で始まるコメント）。**人は 1 文字も書いておらず、数字と診断ログしか無い。** 所見は利用者が
チャットへ直接書く。届いたときの読み方・頼んでよいこと・往復を止める合図
（`<!-- homeskz-ifc-feedback v1 control=stop -->` を**その行だけの行**として投稿する）は
`docs/DEVELOPMENT.md`「実機フィードバックの往復」の「届いたコメントの読み方」に従う。
**黙って push をやめても往復は止まらない**ので、要らなくなったら必ず合図を投稿する。

## CI の完了を待つ

**待機は必ず `scripts/ci-wait.sh`（PR・ブランチの CI）/ `scripts/ci-debug.sh wait`（`ci-debug`）を
`run_in_background: true` で投げて行う。** 完了した瞬間に exit するので、その終了通知が完了
通知になる（PR 購読では CI の成功は配信されない）。

- **`sleep` で待たない。待機ループをその場で手書きしない。** どちらも「CI は終わっているのに
  気付かない」事故を実際に起こしている。
- 最終行 `ci-wait: done (conclusion=… exit=…)` を読む。**`success` でも並んだチェック名を確かめる**
  （`debug` しか無ければ本来の CI は走っていない）。`timed-out-waiting` / `api-error` は CI の
  失敗ではない。
- **`ci-debug` の `build` / `compile-one` は本番 CI の代わりにならない**（clang-tidy を通さない）。
  SDK 依存コードの最終確認は PR の CI が緑になったことで行う。
- **`build.yml` に一時的な調査ステップを挿さない**（dev プレリリースとして公開され、キャッシュを
  汚す）。
- 使い方・`conclusion` の一覧・CI が始まらないときに疑う順序・`ci-debug` の起動手順とモードは
  `docs/DEVELOPMENT.md`「CI の完了待ち」「CI デバッグ」。

## ビルド・リント・リリース

ローカルビルド（`VW_SDK_DIR`）・dual build（`VW_DEV_BUILD`）・テストとカバレッジ・lint・CI と
リリース・自動アップデートは `docs/DEVELOPMENT.md`。プラグイン名は `min-nano_structure`。
