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
- **読み込み時サニタイズ**（`ifc/loader.py` 相当）: ホームズ君 IFC2X3 に混入する
  `IFCFOOTINGTYPE`（IFC4 専用エンティティ）を、STEP テキストから除去してから解析する
  （本プラグインは基礎の型を参照しないため除去して問題ない）。自前リーダなら
  「未知エンティティを黙って読み飛ばす」実装にしてもよい（挙動は同値）。方針は
  Python 版 `loader.py` の docstring を参照。
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
  合わせる**（対応が追いやすく、仕様のブレを防ぐ）。
- Document は**任意で JSON へダンプできる**ようにしておく（デバッグと、Python 版の
  期待値と突き合わせる**ゴールデンテスト**のため）。ただしフェーズ間の受け渡しは
  構造体のまま行い、JSON 往復は必須にしない。
- スキーマを変えるときは、構造体定義・`validateDocument`・テスト・（あれば）JSON
  ダンプを同時に更新する。

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
    Geometry.h              Vec2 / Vec3 / Mat4 等の自前数学型（parse が SDK 非依存のため必須）
    Document.h              命令セットの構造体定義
    DocumentValidate.{h,cpp} validateDocument
    DocumentJson.{h,cpp}    Document ⇔ JSON（デバッグ / ゴールデンテスト用・任意）

  parse/                    Phase 1: IFC 解析（SDK 非依存）… 旧 ifc/
    Step.{h,cpp}            最小 STEP リーダ（トークナイザ＋エンティティグラフ）
    Loader.{h,cpp}          サニタイズを含む読み込み（旧 loader.py）
    IfcGeometry.{h,cpp}     配置行列・押し出しソリッド・断面の解決（旧 footing._world_solid 等）
    BuildDocument.{h,cpp}   build_document 相当のオーケストレーション
    Grid.{h,cpp}            通り芯（旧 ifc/grid.py）
    Story.{h,cpp}           ストーリ（旧 ifc/story.py）
    Member.{h,cpp}          横架材（旧 ifc/member.py）
    …                       以降、要素ごとに 1 対 1 で追加

  draw/                     Phase 2: VW 描画（SDK 依存）… 旧 vw/
    ExecuteDocument.{h,cpp} execute_document 相当のディスパッチ
    Grid.{h,cpp}            grid 命令 → GridAxis（旧 vw/grid.py）
    Story.{h,cpp}           story 命令 → ストーリ・レベル・レイヤ（旧 vw/story.py）
    …                       以降、要素ごとに 1 対 1 で追加

tests/
  TestFramework.h           既存の極小ハーネス（無 SDK）
  fixtures/                 ホームズ君 IFC（Python 版 tests/fixtures から流用）
  StepTests.cpp             STEP リーダ
  GeometryTests.cpp         幾何計算（Python 版の値と突き合わせ）
  ParseGridTests.cpp        parse/Grid …（要素ごと）
```

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
  （`draw/`）は `build.yml` の SDK ビルド中に同じルールでチェックする。
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
  - 可能なら**ゴールデンテスト**: Document を JSON ダンプし、Python 版の
    `build_document` 出力（同じフィクスチャ）と主要フィールドを突き合わせて、
    移植のズレを機械的に検出する。数値は許容誤差付きで比較する。
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
| `ifc/loader.py` | `parse/Loader` + `parse/Step` | 読み込み・サニタイズ・STEP グラフ |
| `ifc/__init__.py` `build_document` | `parse/BuildDocument` | 解析オーケストレーション |
| `ifc/grid.py` … `ifc/section.py` | `parse/Grid` … `parse/Section` | 要素ごとの解析 |
| `ifc/structural_class.py` | `parse/StructuralClass` | 構造クラス判定（純ロジック） |
| （ifcopenshell の行列・幾何） | `parse/IfcGeometry` + `core/Geometry` | 配置行列・押し出し・断面 |
| `document.py` | `core/Document` (+ `DocumentValidate` / `DocumentJson`) | 命令セット・検証 |
| `tracing.py` | `core/Trace`（任意） | クラッシュ診断ログ |
| `vw/__init__.py` `execute_document` | `draw/ExecuteDocument` | 描画ディスパッチ |
| `vw/grid.py` … `vw/section.py` | `draw/Grid` … `draw/Section` | 要素ごとの描画 |
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

## 開発プロセス: PR 作成と監視

姉妹リポジトリと同じ運用に従う。

1. コード編集後、確認すべき疑義がなければ**自動的に PR を作成**する。方針をユーザーに
   確認中など未確定事項がある場合は PR を保留して先に確認する。
2. PR 作成後は `subscribe_pr_activity` で CI・レビューを監視し、CI 失敗は原因を診断して
   修正コミットを push、軽微なレビュー指摘は自動対応、大きな設計判断はユーザーに確認する。
3. コミットメッセージに Claude セッション URL を付す
   （`https://claude.ai/code/session_<SESSION_ID>`）。
