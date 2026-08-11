# CLAUDE.md

このファイルは Claude Code（claude.ai/code）がこのリポジトリで作業するときの指針です。
**この指示は既定の挙動より優先されます。正確に従ってください。**

## このリポジトリについて

姉妹リポジトリ **`vectorworks-plugin-script-import-ifc-homeskz`**（VectorScript /
Python プラグイン）を、**C++ SDK によるネイティブ VectorWorks 2026 プラグイン**へ
移植するプロジェクトです。

**ホームズ君構造EX** が出力する木造軸組工法建築物の IFC ファイルをパースし、
VectorWorks のネイティブオブジェクトに変換して配置するというコンセプトは
Python 版と変わりません。

このリポジトリはもともと Vectorworks ネイティブプラグインの**テンプレート**
（`vectorworks-plugin-native-template`）から出発しており、以下がすでに整っています。

- macOS / Windows 両対応の CMake ビルド
- stable / dev 2 系統の共存ビルド（`VW_DEV_BUILD` スイッチ）
- ビルド配布・自動アップデートの仕組み（`scripts/vw-update.*`）
- CI（`build.yml` / `lint.yml` / `test.yml` / `codeql.yml`）
- lint 設定（clang-format / clang-tidy / cmake-format / yamllint / editorconfig / PSScriptAnalyzer）
- **SDK 非依存ロジックを無 SDK で単体テストする**土台（`src/UpdaterParse.h` などを
  `tests/` の極小ハーネス `TestFramework.h` でテスト）

移植作業は、この土台を活かして**小さく機能を積み上げていく**（下記「移植の基本方針」）。

## 移植の基本方針

1. **概念は不変、完全一致は非目標**。ホームズ君 IFC をパースして VectorWorks
   ネイティブデータへ変換する、という目的は同じ。ただし Python API（`vs`）には制限が
   多く、Python 版には**トリッキーな実装や未実現の挙動**がある。C++ SDK は API が
   広いため、Python 版とバイト単位で同じ描画を再現することは目標にしない。
   **仕様の意図（何を・なぜ描くか）を再現し、実現手段は C++ SDK の作法に合わせる。**

2. **IFC パースの「方法」は Python 版から取り入れる**。ホームズ君が出力する IFC の
   読み方——どのエンティティ（`IfcGridAxis` / `IfcBeam` / `IfcColumn` / `IfcFooting` /
   `IfcSlab` / `IfcBuildingStorey` / `IfcMechanicalFastener` …）を、どの属性・逆参照
   （`IfcRelContainedInSpatialStructure` / `IfcRelDefinesByType` /
   `IfcRelDefinesByProperties`→`Pset`）で辿り、ホームズ君固有の命名規約
   （`木梁:{種別}:{連番}`、`STANDCOLUMN`、`基礎梁…`、`底盤…`、`地中梁…` 等）を
   どう解釈するか——は Python 版が実証済みの資産。**そのロジックを C++ へ移植する。**

3. **2 フェーズ分離を維持する**（下記「アーキテクチャ」）。これが Python 版の設計の
   核であり、テンプレートの「SDK 非依存ロジック＋無 SDK テスト」パターンと完全に噛み合う。

4. **小さく機能追加を重ねる**。プラグインの実描画は**ローカルの VectorWorks でしか
   最終確認できない**。したがって 1 マイルストーン＝1 要素（または 1 サブ機能）とし、
   各マイルストーンを「IFC パース → 命令セット → VW 描画 → ローカル目視確認」の
   1 周が回る**縦切り**で完成させる。詳細は `ROADMAP.md`。

5. **Python 版の CLAUDE.md を仕様の一次資料とする**。姉妹リポジトリの
   `CLAUDE.md`（約 186KB）に各要素の解析・描画の詳細仕様が日本語で書かれている。
   C++ で 1 要素を移植するときは、まずそのリポジトリの該当節と実装
   （`src/vectorworks_plugin_import_ifc_homeskz/ifc/<要素>.py` /
   `vw/<要素>.py`）とテスト（`tests/`）を読み、意図を写す。

## アーキテクチャ: 2 フェーズ分離

処理は **IFC 解析フェーズ** と **VectorWorks 描画フェーズ** に完全分離する。
両フェーズは**命令セット（Document）**だけで接続し、SDK との密結合を解析側に
持ち込まない。Python 版の `ifc` / `vw` 分離をそのまま C++ へ写す。

```
IFC ファイル
   │  Phase 1: parse  （SDK 非依存 = VectorWorks SDK を include しない）
   ▼
Document（命令セット。プレーンな構造体の集まり）
   │  Phase 2: draw   （VectorWorks SDK のみに依存）
   ▼
VectorWorks ネイティブオブジェクト
```

### Phase 1 — `parse/`（旧 `ifc/`）: SDK 非依存

- **VectorWorks SDK を一切 include しない。** 通常の C++ ツールチェインだけで
  コンパイル・単体実行・テストできる（テンプレートの `UpdaterParse` と同じ立ち位置）。
- **最小 STEP リーダ**（自前）で IFC を読む。ホームズ君が出す既知サブセット向けに、
  STEP トークナイザ＋エンティティグラフ（`byType(name)` / インデックスによる属性
  アクセス / 逆参照 lookup）を提供する。**幾何エンジン（OpenCASCADE 等）は使わない**
  ——Python 版も ifcopenshell を「エンティティグラフの読み取り」だけに使い、配置行列・
  断面・押し出しの幾何計算は自前で行っている。その幾何計算を C++ へ移植する。
- **読み込み時サニタイズは不要**（`ifc/loader.py` との相違点）: Python 版が
  ホームズ君 IFC2X3 に混入する `IFCFOOTINGTYPE`（IFC4 専用エンティティ）を STEP
  テキストから除去していたのは、**ifcopenshell が IFC2X3 スキーマに無い非正規
  エンティティに出会うと処理を中断してしまう**からで、除去はその回避策だった。
  自前 STEP リーダ（`parse/Step`）は**スキーマ検証をせず非正規エンティティも
  そのまま読める**ため、除去は不要。本プラグインはそれらの型を参照しないので、
  グラフ上に残っていても無害（挙動は Python 版の除去と同値）。したがって
  `parse/Loader` はファイル読み込み（テキスト→STEP グラフ）だけを担い、
  サニタイズ処理は持たない。
- 出力は **Document**（下記）。ここに `vs`/SDK ハンドルや STEP エンティティポインタ等の
  「フェーズ間で運べないもの」を入れない。

### Phase 2 — `draw/`（旧 `vw/`）: VectorWorks SDK 依存

- **VectorWorks SDK のみに依存**し、IFC / STEP の知識を持たない。
- Document を**検証**（`validateDocument` 相当）してから SDK API で描画する。
- 実描画（高さ・傾き・スタイル・PIO の挙動）は SDK 上で最終確認する（自動テスト困難）。

### 命令セット（Document）

- Python 版では JSON 直列化可能な dict。C++ では**プレーンな構造体**
  （`std::vector`・`std::string`・`double`・`enum` 等の集約）で表す。
- スキーマ（`stories` / `grids` / `members` / `columns` / `walls` / `slabs` / `floors` /
  `rafters` / `roofs` / `anchorBolts` / `floorPosts` / `fireBraces` / `joints` /
  `columnMarks` / `sheets` / `sections` / `tags` / `legends` …）は Python 版
  `document.py` の `TypedDict` 群に対応させる。**フィールド名・意味は Python 版に
  合わせる**（対応が追いやすく、仕様のブレを防ぐ）。ただし**同型の TypedDict が並ぶところは
  構造体 1 つへまとめる**——`anchorBolts` / `floorPosts` / `fireBraces` / `joints` は
  Python 版では 4 つの TypedDict だが中身が同じなので、C++ は `core::SymbolCommand` 1 つで
  受け、要素の区別は「Document のどのリストか」が担う（`core/Document.h` の doc コメント参照）。
- フェーズ間の受け渡しは**構造体のまま**行う。JSON 直列化は**予定に無い**——Python 版
  出力との突き合わせ（ゴールデンテスト）は行わない方針のため、唯一の用途が消えた
  （理由は `ROADMAP.md`「Python 版出力との比較はしない」）。デバッグでダンプが要る場面が
  実際に出てきたら、そのときに最小限を足す。
- スキーマを変えるときは、構造体定義・`validateDocument`・テストを同時に更新する。

## ディレクトリ構造（目標）

移植が進むにつれて `src/` を次の形に育てる（M0 で骨組みを用意）。

```
src/
  ModuleMain.cpp            モジュールのエントリポイント（既存）
  Extensions/
    ExtMenu.{h,cpp}         「IFC インポート」メニューコマンド（既存 Sample から改名・改修）
  BuildConfig.h             stable/dev 識別（既存）
  PluginPrefix.h            SDK を取り込む共有プレフィックス（既存。parse/ からは include しない）
  Updater*.{h,cpp}          自動アップデータ（既存・テンプレート由来。基本触らない）

  core/                     フェーズ非依存の土台（SDK も STEP も知らない純粋コード）
    Geometry.{h,cpp}        Vec2 / Vec3 / Mat4 等の自前数学型（parse が SDK 非依存のため必須）
    Document.{h,cpp}        命令セットの構造体定義と validateDocument（1 対で持つ。分割は不要）
    Region.{h,cpp}          部品が囲む平面領域の合成（ロフト床の外形。M5 で追加）
    Progress.{h,cpp}        進捗の報告先（ProgressReporter）と文言整形・バー配分（M15 の先行実装）

  parse/                    Phase 1: IFC 解析（SDK 非依存）… 旧 ifc/
    Step.{h,cpp}            最小 STEP リーダ（トークナイザ＋エンティティグラフ）
    Loader.{h,cpp}          ファイル読み込み: テキスト→STEP グラフ（旧 loader.py。サニタイズは不要）
    IfcAttr.h               IFC 属性インデックスの唯一の定義（番号を散らさない）
    IfcGeometry.{h,cpp}     配置行列・押し出しソリッド・断面・屋根面の解決（旧 footing._world_solid 等）
    Context.{h,cpp}         解析中の共有キャッシュ（下記「共有コンテキスト」）
    BuildDocument.{h,cpp}   build_document 相当のオーケストレーション
    Grid.{h,cpp}            通り芯（旧 ifc/grid.py）
    Story.{h,cpp}           ストーリ（旧 ifc/story.py）
    Member.{h,cpp}          横架材（旧 ifc/member.py）
    Footing.{h,cpp}         基礎（立上り＝壁・底盤＝スラブ・基礎ストーリ・人通口・壁結合・
                            地中梁。旧 ifc/footing.py）
    Joint.{h,cpp}           仕口（旧 ifc/joint.py。IFC ではなく member/column 命令から導出）
    …                       以降、要素ごとに 1 対 1 で追加

  draw/                     Phase 2: VW 描画（SDK 依存）… 旧 vw/
    ExecuteDocument.{h,cpp} execute_document 相当のディスパッチ
    DrawUtil.{h,cpp}        クラス分け・by-class 属性・スタイル解決・レイヤ用意・スラブの共通ヘルパー
    StructuralMember.{h,cpp} 構造材ツール（StructuralMember PIO）1 本の生成・設定（横架材／柱で共有）
    ProgressDialog.{h,cpp}  core::ProgressReporter を VW の進捗ダイアログへ橋渡し（唯一の実装）
    Grid.{h,cpp}            grid 命令 → GridAxis（旧 vw/grid.py）
    Story.{h,cpp}           story 命令 → ストーリ・レベル・レイヤ（旧 vw/story.py）
    Footing.{h,cpp}         wall/wallJoin/slab 命令 → 壁・壁結合・スラブ（地中梁の
                            台形プリズムを含む。旧 vw/footing.py）
    Symbol.{h,cpp}          シンボル配置（旧 vw/{anchor_bolt,floor_post,fire_brace,joint}.py
                            の 4 本を 1 本に。要素の区別は呼び出し側が持つ）
    …                       以降、要素ごとに 1 対 1 で追加

tests/
  TestFramework.h           既存の極小ハーネス（無 SDK）
  Fixtures.h                フィクスチャの読み込み・近似比較・フィクスチャ一覧（共通）
  RoofSample.h              試験用の屋根面と最小の屋根版 IFC（垂木/野地板/幾何で共有）
  fixtures/                 ホームズ君 IFC（Python 版 tests/fixtures から流用）
  StepTests.cpp             STEP リーダ
  GeometryTests.cpp         幾何計算（Python 版の値と突き合わせ）
  ParseGridTests.cpp        parse/Grid …（要素ごと）
```

**共有コンテキスト（`parse/Context`）**: 各要素の解析は「ストーリ一覧」「通り芯の
センタリング中心」「階に属する要素」「屋根面」を共通して必要とする（加えて横架材・柱・
立上りの命令そのものも複数の要素が参照するのでキャッシュする）。`buildDocument` は
`Context` を **1 つだけ**作って全要素へ渡し、これらの前処理を 1 回で済ませる（要素ごとに
求め直すと同じ走査が要素の数だけ走る）。単体テストの都合で `const Model&` を直接取る
オーバーロードも各 `build*Commands` に残してあり、そちらは内部でコンテキストを作って捨てる。

**重複を作らない置き場所**: 同じ定数・述語・ヘルパーを 2 か所に書かない。IFC の属性
インデックスは `parse/IfcAttr.h`、レベル種別名は `parse/Story.h`（屋根組は
`parse/Rafter.h` / `parse/Roof.h`。基礎ストーリの名前・接尾辞・レベル・レイヤ名は
`parse/Footing.h`）、レイヤ名の組み立ては `storyLayerName`、要素の判別
述語（`isFloorSlab` / `isRoofSlab` / `isFireBrace`、基礎の `isBaseSlab` 等）はその要素の
ヘッダ、金物（`IfcMechanicalFastener`）の型名取得は `parse/Column` の `fastenerTypeName`
（柱頭・柱脚金物とアンカーボルトが共有）、平面座標の同一判定と許容
（`samePoint` / `kPointEps`）は `core/Geometry.h`、屋根面の勾配座標系と退化の閾値・**押し出しを
鉛直とみなす閾値**（`kVerticalExtrudeTol`。平面外形の求め方と、人通口・地中梁の「水平押し出しか」
判定が共有する）は `parse/IfcGeometry.h`、基礎のレイヤ名・許容値（統合・自由端・**人通口・
壁結合・地中梁**）は `parse/Footing.h`、`draw/` の SDK 呼び出しの定型（クラス分け・レイヤ用意・
スタイル解決・**構成層／基準面／スタイルの新規作成**——床板・底盤・立上りが共有する）は
`draw/DrawUtil`、構造材ツール（StructuralMember PIO）のフィールド名・値・生成手順は
`draw/StructuralMember`、ハイブリッドシンボルの配置は `draw/Symbol`（4 要素で共有する唯一の
実装）、**描画側から切り離せる純計算**（レイヤの希望スタック順
`desiredStoryLayerOrder`・地中梁の可視ソリッドの呑み込み `raiseModifierTop`）は `core/Document`、
進捗の見出し・バー配分は `draw/ExecuteDocument`（要素ごとのフェーズ）と `core/Progress`
（整形と配分の計算）に**それぞれ 1 つだけ**置く。テスト側も同じで、フィクスチャ一覧・近似比較は `tests/Fixtures.h`、
共有する試験用屋根面と最小 IFC は `tests/RoofSample.h` が唯一の定義。

**依存の向きは厳守する:** `parse/` と `core/` は VectorWorks SDK を include しない。
`draw/` は STEP / IFC を include しない。両者をつなぐのは `core/Document.h` だけ。
この規律は Python 版の「`ifc` に `vs` を持ち込まない」規約の C++ 版であり、CI で
**parse/core を無 SDK でコンパイル・テストできること**によって担保する。

## C++ コード規約

### フォーマット（既存 lint に従う）

テンプレートの設定をそのまま使う。**これらが単一の真実**で、手で例外を作らない。

- `.clang-format`: **タブインデント（幅 4）**・**Allman ブレース**・**名前空間の本体を
  インデント**・**ポインタ/参照は型側に寄せる**（`int* p`）・**100 桁で折り返し**・
  **コメントは自動再整形しない**（`ReflowComments: false`。日本語の手折りコメントを守る）・
  **include は自動並べ替えしない**（`PluginPrefix` / `BuildConfig` を先頭に保つ）。
- `.clang-tidy`: **警告をエラー扱い**（`WarningsAsErrors`）。SDK 非依存コード
  （`core/` `parse/`）は CI のランナー上でも clang-tidy にかける。SDK 依存コード
  （`draw/`）は `build.yml` の `tidy-mac` / `tidy-windows` ジョブが同じルールで
  チェックする（ビルドと並走。対象一覧と並列実行は `scripts/clang-tidy-sdk.sh`）。
  この 2 ジョブは `release` の `needs` に入っているので、**ビルドが通っていても
  clang-tidy が通らなければリリースは公開されない**。
- コミット前に `scripts/lint.sh`（必要なら `--fix`）で C++ / CMake / YAML / shell の
  全 lint を通す（CI と同一チェック）。

### 命名

- **名前空間**: プラグイン固有コードは単一のトップ名前空間にまとめる
  （M0 でテンプレートの `SamplePlugin` を `HomeskzIfcImport` へ置換済み）。
  フェーズは入れ子名前空間（`::parse` / `::draw` / `::core`）で分ける。
- **SDK 拡張クラス**は SDK の作法（`CExt…` / `…_EventSink`）に従う（既存 `ExtMenu` に倣う）。
- **フェーズ非依存コード**（`core/` `parse/`）は素直な C++ 命名でよい:
  型は `PascalCase`、関数・変数は `camelCase`、定数は `kPascalCase` または
  `UPPER_SNAKE`（ファイル内で統一）。Python 版の関数名（`build_grid_commands` 等）は
  `buildGridCommands` のように機械的に対応させ、**追跡しやすさを優先**する。
- **Document のフィールド名**は Python 版のキー（`class` は予約語なので `drawClass` /
  `className` 等へ機械的に置換）に対応させ、対応表を各構造体の doc コメントに書く。

### 幾何の型

- `parse/` は SDK 非依存なので、**SDK の幾何型（`WorldPt3` 等）を使えない**。
  `core/Geometry.h` に自前の `Vec2` / `Vec3` / `Mat4`（配置行列合成用）を定義し、
  Python 版が手計算している行列・断面・押し出しの数式をここへ移植する。
- `draw/` では SDK の幾何型と自前 `Vec*` を相互変換する薄いヘルパーを 1 か所に置く
  （変換規約を分散させない）。

### エラーハンドリング・所有権

- **STEP パースの失敗・想定外エンティティ**は Python 版の寛容さ（`continue` して
  スキップ、フォールバック描画）を踏襲する。1 要素の欠損で全体を止めない。
- **例外は parse 内部の局所処理に留める**。フェーズ境界（`buildDocument` の戻り）は
  値で返す。SDK コールバック（`plugin_module_main` / `DoInterface`）へ例外を漏らさない。
- **RAII で所有権を明示**。生ポインタの所有は避け、SDK ハンドルは Document に載せない
  （フェーズ間で運べない）。描画で必要なハンドルの受け渡しは Python 版と同じく
  「命令インデックス → ハンドル」の対応（`std::unordered_map<size_t, Handle>` 等）で行う
  （横架材ハンドル→タグ、壁ハンドル→壁結合など）。
- **決定性を守る**。エンティティ列挙順に依存しない結果を出す（Python 版が随所で
  「入力順に依存しない決定的な結果」を保証しているのと同じ）。ソート・集約は明示的に。

### コメント・言語

- **日本語コメントを基本**とする（Python 版・テンプレート README と揃える）。
  既存ソースの重厚な手折りコメントの density に合わせる。`ReflowComments: false` の
  ため折り返しは著者責任。
- **なぜ（意図・仕様の根拠）を書く**。ホームズ君 IFC の癖・VW SDK の落とし穴・
  Python 版で判明済みの不具合（#番号）への対処は、対応する Python 版 CLAUDE.md の
  節を参照する形で残す。

## テスト方針

Python 版の「`ifc`/`document` テストは vs モック不要、`vw` テストは vs モックで実行」
という分離を C++ でも守る。

- **`core/` `parse/`**: **無 SDK で単体テスト**（既存 `TestFramework.h` を使う）。
  - STEP リーダ・幾何計算・各 parse モジュールを、Python 版と同じ**実 IFC フィクスチャ**
    （`tests/fixtures/` へ流用）に対してテストする。
  - **期待値は手書きする。** Python 版 `build_document` の出力と機械的に突き合わせる
    （ゴールデンテスト）ことはしない——Python 版は仕様の一次資料であって出力の契約では
    なく、意図的な差が積み上がるほど除外リストが育つだけになる（`ROADMAP.md`
    「Python 版出力との比較はしない」）。移植ズレは Python 版の該当節・実装・テストを
    読んで期待値へ写すことで防ぐ。数値は許容誤差付きで比較する。
  - CI（`lint.yml` / ランナー）で無 SDK ビルドとして常時回す。
- **`draw/`**: SDK 依存のため CI での完全な実行は難しい。
  - ロジック（レイヤ順の並べ替え計算、命令→SDK 呼び出し列の組み立て等）で
    SDK から切り離せる部分は `core/` 側へ寄せて無 SDK テストする。
  - 実描画は**ローカルの VectorWorks で目視確認**する（各マイルストーンの
    「ローカル確認チェックリスト」に沿う。`ROADMAP.md`）。SDK 呼び出しの薄い
    ラッパーは、必要なら SDK モックで「正しい引数で呼んだか」を検証する程度に留める。

## Python 版との対応表

| Python | C++（目標） | 役割 |
| --- | --- | --- |
| `ifc/loader.py` | `parse/Loader` + `parse/Step` | 読み込み・STEP グラフ（サニタイズは不要） |
| `ifc/__init__.py` `build_document` | `parse/BuildDocument` | 解析オーケストレーション |
| `ifc/grid.py` … `ifc/section.py` | `parse/Grid` … `parse/Section` | 要素ごとの解析 |
| `ifc/footing.py` | `parse/Footing` | 基礎（立上り・底盤・基礎ストーリ） |
| `ifc/structural_class.py` | `parse/StructuralClass` | 構造クラス判定（純ロジック） |
| （ifcopenshell の行列・幾何） | `parse/IfcGeometry` + `core/Geometry` | 配置行列・押し出し・断面 |
| `document.py` | `core/Document`（検証も同ファイル） | 命令セット・検証 |
| `tracing.py` | `core/Trace`（任意） | クラッシュ診断ログ |
| `vw/__init__.py` `execute_document` | `draw/ExecuteDocument` | 描画ディスパッチ |
| `vw/grid.py` … `vw/section.py` | `draw/Grid` … `draw/Section` | 要素ごとの描画 |
| `vw/footing.py` | `draw/Footing` | 基礎の描画（壁・スラブ） |
| `main.py` / `__init__.py` `run()` | `Extensions/ExtMenu`（コマンド本体） | ファイル選択→解析→描画→完了ダイアログ |

## 移植上の既知の制限・非目標

- Python 版の**バイト単位の再現は非目標**。VW ビューポート再描画の手動反映依存など、
  Python 版が「VW の制約として許容」した挙動は、C++ SDK でより良く扱えるなら差し替えてよい。
- **ホームズ君 IFC 以外の汎用 IFC 対応は非目標**。既知サブセット前提で最小 STEP リーダを組む。
- 1 マイルストーンで**全要素を一度に移植しない**。`ROADMAP.md` の順序で 1 つずつ。

## ビルド・リント・リリース

テンプレートの `README.md` に、ローカルビルド（`VW_SDK_DIR` 指定）・dual build
（`VW_DEV_BUILD`）・lint（`scripts/lint.sh`）・自動アップデート・CI の詳細がある。
移植でこれらの仕組みは基本そのまま使う。**プレースホルダー識別子
（`SamplePlugin` / `com.example…` / UUID 等）の置換**は M0 で完了済み（現在の識別子は
README「プラグイン識別子」節を参照。プラグイン名は `HomeskzIfcImport`）。

## CI の完了を待つ（待機は必ず `ci-wait` / `ci-debug` で行う）

リモートセッションから「CI が終わった」ことを知る手段は、**完了した瞬間に exit する
プロセスをバックグラウンドで走らせる**ことだけである（PR 購読で配信されるのは CI の
**失敗**とコメントだけで、**成功は配信されない**）。バックグラウンドコマンドの終了は
ハーネスが通知するので、**exit がそのまま完了通知になる**。

したがって次の 2 つは**禁止**する。どちらも「CI は終わっているのにセッションが
気付かない」という事故に直結し、実際に 2 度起きている。

- **`sleep` で待つ**（完了時刻の予測が要るうえ、外れれば無駄待ちか取りこぼし）。
- **待機ループをその場で手書きする**（`while : ; do gh/curl …; sleep 30; done`）。
  締切もウォッチドッグも HTTP の時間上限も無いので、API が固まればぶら下がる。

### 使い方

```
Bash(run_in_background: true):
  scripts/ci-wait.sh --pr 34        # PR の head（新しい push が入ったら追随する）
  scripts/ci-wait.sh --ref main     # ブランチ / タグ
  scripts/ci-wait.sh                # いま checkout しているブランチ
  scripts/ci-wait.sh --sha <sha>    # 固定のコミット（追随しない）
```

投げたら別作業を続け、終了通知が来たら出力ファイルを `Read` するだけでよい。
出力の最終行は必ず

```
ci-wait: done (conclusion=<結果> exit=<終了コード>)
```

で、この行が無ければ「まだ動いている」か「外から殺された」かのどちらか。
**`conclusion=success` 以外は exit 1** で、内訳は次のとおり。

| conclusion | 意味 |
| --- | --- |
| `success` | 全チェックが成功（skipped / neutral を含む） |
| `failure` | 1 つ以上が失敗・キャンセル・timed_out。**cancelled も失敗扱い**（新しい push で古い run が消えたものを green と取り違えないため） |
| `no-checks` | 猶予（既定 180 秒）を過ぎてもチェックが 1 件も登録されなかった。**「CI が始まってすらいない」を成功と読まない**ための結果 |
| `head-moved` | `--no-follow` 指定時に、待っている間に head が動いた（古い結果は返さない） |
| `timed-out-waiting` / `api-error` | **CI の失敗ではなく待機側が見届けられなかった**。CI 自体はまだ動いているかもしれない（同じ行に合流用のコマンドが出る） |

`git push` の直後に投げてよい（チェックの登録待ちは `--grace` が吸収する）。CI が長い
間も、状態が変わらなければ 5 分ごとに生存行が stderr に出るので、固まっているのか
単に長いのかは出力を見れば分かる。

### CI が始まらないときに疑うこと（原因を何度も見誤っている）

`no-checks` が返る・PR の Checks が 0 のままというのは、**CI が失敗したのではなく
「チェックが登録されていない」**という意味。原因の切り分けを毎回やり直さないよう、
実際に起きたものを可能性の高い順に置く。**どれも「必ずそうなる」規則ではない**ので、
断定して報告せず、疑う順序として使うこと。

1. **PR にコンフリクトがある。** `build.yml` / `lint.yml` / `test.yml` は `pull_request`
   トリガなので PR が対象だが、**コンフリクトを抱えた PR ではチェックが 1 件も登録されない
   ことがある**（実測: main を取り込んで解消したら、こちらが何も操作しなくても CI が
   起動した）。ただし**コンフリクトがあれば毎回走らない、という挙動でもない**らしい
   （GitHub 側で無駄な実行を削っている可能性がある）。**「走らないならまずここを見る」**
   という位置づけで、「走らせるために必ず解消しろ」という意味ではない。
2. **PR がまだ無い／その head に PR が向いていない。** 作業ブランチへの push は
   `push: branches: [main]` に当たらないので何も走らない。PR を作る前に push しただけの
   コミットにチェックが付かないのは正常。
3. どれでもなければ、GitHub MCP で run（`actions_list` の `list_workflow_runs`）と
   check-run（`pull_request_read` の `get_check_runs`）を直接数えて、登録の有無を確かめる。

**`ci-wait` の `success` を鵜呑みにしない。** `ci-wait --pr` は「その sha に登録されている
チェック」を見るので、`ci-debug`（`workflow_dispatch`）の `debug` チェックしか無い状態でも
`conclusion=success` を返す。出力に並ぶチェック名を必ず読み、`build-mac` /
`build-windows` / `clang-tidy` / `test` … が並んでいることを確認する（**`debug` だけなら
本来の CI は走っていない**）。

### 仕組みと不変条件

待機の土台は `scripts/ci-common.sh` にあり、`ci-wait.sh` と `ci-debug.sh` が共有する。
**どんな異常でも必ず有限時間で exit する**ことが唯一にして最大の要件で、そのために
HTTP 呼び出しの時間上限・締切判定・ウォッチドッグの三重の歯止めを持つ（詳細は同
ファイルのヘッダ）。この性質は `tests/ci-wait.test.sh`（ctest の `CiWaitScriptTests`）
で回帰テストしてある。

**新しく「何かの完了を待つ」道具が要るときは、`poll_until` の上に probe を 1 つ書く。**
待機ループを増やさない。

## CI デバッグ（SDK 依存の調査は `ci-debug` を使う）

リモートセッション（クラウド上のコンテナ）には **Vectorworks SDK が無い**。したがって
SDK 依存のビルドエラーの再現や「この API は SDK にあるか」という設計調査は、**CI 上で
しか答えが出ない**。そのための専用ワークフローが `.github/workflows/ci-debug.yml` で、
`workflow_dispatch` でしか起動しない（push / PR では**決して**走らない）。

**`build.yml` に一時的な調査ステップを挿してはならない。** 戻し忘れる・その commit が
dev プレリリースとして公開される・ccache / SDK キャッシュを汚す、と副作用が大きい。
調査は必ず下記の経路で行う。

### 使い方（リモートセッションの AI はこの 2 手順）

リモートセッションのコンテナに入っている `GITHUB_TOKEN` は**読み取り専用**で
`actions: write` を持たない（REST でのディスパッチは 403 になる）。したがって
**起動は GitHub MCP、待機はスクリプト**という 2 手順になる。

```
1. mcp__github__actions_run_trigger
     method: run_workflow, workflow_id: "ci-debug.yml", ref: <調査したいブランチ>,
     inputs: {mode, platform, label, args, script, notify_pr}
     ※ label は一意な文字列にする（これで run を特定する）

2. Bash(run_in_background: true):
     scripts/ci-debug.sh wait --label <label>
```

**手順 2 は必ず `run_in_background: true` で投げる。** このスクリプトは「run の特定 →
完了待ち → ペイロード抽出」を行って**完了した瞬間に exit する**ので、待機時間ゼロ・
タイマー不要で結果を受け取れる（バックグラウンドコマンドの終了はハーネスが通知する）。
投げたら別作業を続け、終了通知が来たら出力ファイルを `Read` するだけでよい。
**`sleep` で待ってはいけない。**

**待機は必ず有限時間で終わる。** この仕組みは「exit がそのまま完了通知になる」ことに
全面的に依存しているので、`wait` はどんな異常でも必ず exit するように作ってある
（HTTP 呼び出しの時間上限・締切判定・ウォッチドッグの三重。実体は `ci-wait.sh` と
共有の `scripts/ci-common.sh`）。既定の上限は 45 分（ジョブ側の
`timeout-minutes` と同じ）で、`--timeout` / `--poll` で変えられる。出力の最終行は必ず

```
ci-debug: done (conclusion=<結果> exit=<終了コード>)
```

で、この行が無ければ「まだ動いている」か「外から殺された」かのどちらか。
`conclusion=timed-out-waiting` / `api-error` は**CI の失敗ではなく待機側が見届けられ
なかった**という意味で、run 自体はまだ動いているかもしれない（同じ行に合流用の
コマンドが出る）。長い CI を待つ間、状態が変わらなくても 5 分ごとに生存行が
stderr に出るので、固まっているのか単に長いのかは出力を見れば分かる。

書き込み権限のあるトークン（PAT など）が使える環境では、起動と待機をまとめた

```bash
scripts/ci-debug.sh run --mode sdk-grep --args 'GetLayerByName'
```

が使える（`run` は内部で 1 と 2 を続けて行う。403 が返る環境では上の 2 手順に切り替える）。

| mode | 用途 | `--args` |
| --- | --- | --- |
| `sdk-grep` | SDK ヘッダを拡張正規表現で検索（**設計調査の主力**） | 検索パターン |
| `sdk-ls` | ヘッダの全文表示 / パス部分一致の一覧 | ヘッダのパスまたは部分文字列 |
| `build` | configure してビルド（リリース公開はしない） | 単一ターゲット名（省略可） |
| `compile-one` | 1 翻訳単位だけコンパイル（数十秒。Windows 不可） | ソースのパス |
| `shell` | 任意の bash（`--script`）。逃げ道 | — |

`--platform` は `mac`（既定）/ `windows` / `linux`。**`linux` は SDK を用意しない**ので
SDK 非依存コード（`core/` `parse/` とテスト）専用。SDK が要らない調査は `linux` を選ぶと
速い。`--ref` は既定で現在のブランチ。

**`ci-debug` の `build` / `compile-one` は本番 CI の代わりにならない。** どちらも
clang-tidy を通さない構成でコンパイルするだけなので、`build.yml` の `build-mac` /
`build-windows` が落とす lint（例: `readability-uppercase-literal-suffix`）はここでは
素通りする。**「ci-debug の build が通ったから CI も通る」と報告しない**——SDK 依存
コードの最終確認は PR の CI が緑になったことで行う（`ci-debug` は「SDK でコンパイルが
成立するか」を早く知るための道具）。

### 結果の読み方

出力は必ず次のマーカーで挟まれている。`truncated=yes` のときは**全部は見えていない**
ので、`--args` を絞るか `mode=shell` で件数を数える。

```
===== BEGIN PAYLOAD (mode=... platform=...) =====
...
===== END PAYLOAD (exit=N lines_total=N truncated=yes|no) =====
```

`... (annotation truncated by GitHub's 4096-char limit …)` が出ていたら、それは**注釈経路の
上限**で切られたということ（下記）。この行は必ず END マーカーの直前に入るので、END の
`lines_total` を見れば本当の行数が分かる。全文はジョブログとアーティファクトにある。

失敗して調査コマンドに到達しなかった場合はマーカーが無く、代わりに理由が出る。
全文ログは run のアーティファクト（`ci-debug-<label>`）に残るが、**AI はアーティファクトを
取得できない**ので、必要な情報は必ずログ側に出すこと（モードを追加するときの原則）。

ペイロードの取得経路は 2 つあり、`ci-debug.sh` はこの順に試す。

1. **チェックラン注釈**（`GET /repos/{owner}/{repo}/check-runs/{id}/annotations`）。
   `ci-debug-job.sh` がペイロードを `::notice::` としても出しているので、ここから読める。
   `api.github.com` だけで完結し、ログのノイズ（セットアップ・アーティファクト
   アップロード・ポストジョブ後始末）も混ざらない。**通常はこちらで取れる。**
2. **ジョブログ**。ログ API は署名付きの Azure Blob Storage へ 302 で飛ぶが、**その
   ホストは組織の egress ポリシーで拒否されている**ため、リモートセッションの
   コンテナからは取得できない（`curl: (56) CONNECT tunnel failed, response 403`）。
   これは迂回してはならない制約なので、必要なときは GitHub MCP の `get_job_logs`
   （`job_id` 指定・`return_content: true`）を使う。ただし全ログが文脈に入るので、
   注釈で足りるならそちらで済ませる。

### 通知について

PR 購読（`subscribe_pr_activity`）で配信されるのは CI の**失敗**とコメントだけで、
**成功は配信されない**。上記のバックグラウンド待機が主たる通知経路であり、これが
あるので完了時刻を予測する必要はない。待機プロセスを失った場合（セッション再開など）は

```bash
scripts/ci-debug.sh wait --label <label>   # label は run の出力冒頭に出ている
```

で合流できる。長時間の調査で確実に結果へ追いつきたいときは `--notify-pr <番号>` を付け
ると、完了時に結果が PR コメントとして投稿される（コメントは購読中セッションへ配信
されるため、これが冗長な通知経路になる）。

### 制約

- `workflow_dispatch` は**デフォルトブランチに存在するワークフロー**しか起動できない。
  `ci-debug.yml` が `main` にマージされて初めて、作業ブランチを `--ref` に指定して使える。
- **モードの追加・修正は `scripts/ci-debug-job.sh`（ランナー側）で行う。** ワークフロー
  本体は薄く保ってあるので、作業ブランチに push するだけで新しいモードを試せる
  （`--ref` がそのブランチのため）。ワークフロー本体を変えると main へのマージが要る。
