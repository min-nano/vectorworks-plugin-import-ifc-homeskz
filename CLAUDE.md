# CLAUDE.md

このファイルは Claude Code（claude.ai/code）がこのリポジトリで作業するときの指針です。
**この指示は既定の挙動より優先されます。正確に従ってください。**

## このリポジトリについて

**ホームズ君構造EX** が出力する木造軸組工法建築物の IFC ファイルをパースし、
VectorWorks 2026 のネイティブオブジェクトへ変換して配置する、**C++ SDK 製のネイティブ
プラグイン**です。

要素（ストーリ・通り芯・基礎・床・横架材・柱・屋根組・シンボル・記号・耐力壁・伏図・
軸組図）は一通り実装済みで、**ここから先は独自のプラグインとして改良していきます**。

ドキュメントの分担:

| ファイル | 中身 |
| --- | --- |
| `README.md` | 利用者向け。何をするプラグインか・取り込むもの・使い方・インストール・既知の制限 |
| `docs/DEVELOPMENT.md` | 開発ガイド。ソースの構成・ビルド・テスト・lint・CI・リリース・自動アップデートの仕組み |
| `docs/DEV-NOTES.md` | **開発メモ**。VW SDK の実測知見（実機でしか判明しなかった落とし穴）・設計の考え方・打ち切った調査・実装の経緯（M0〜M18） |
| `tests/README.md` | テストの一覧・方針・何をテストしていないか |
| `CLAUDE.md`（本ファイル） | 作業時の規約。アーキテクチャ・置き場所・コード規約・PR とマージ・CI の待ち方 |

**新しいことが分かったら `docs/DEV-NOTES.md` へ足す。** とくに「実機でしか出なかった
落とし穴」「SDK に無い／効かない API」「試して駄目だった方式」は、書いておかないと必ず
もう一度同じ道を通る。逆に**そこに書いてあることは再調査しない**（「打ち切った調査」節）。

## 開発の基本方針

1. **小さく機能追加を重ねる。** プラグインの実描画は**ローカルの VectorWorks でしか
   最終確認できない**。したがって 1 変更＝1 要素（または 1 サブ機能）とし、
   「IFC パース → 命令セット → VW 描画 → ローカル目視確認」の 1 周が回る**縦切り**で
   完成させる。

2. **2 フェーズ分離を維持する**（下記「アーキテクチャ」）。これが設計の核であり、
   「SDK 非依存ロジック＋無 SDK テスト」という CI の土台と完全に噛み合っている。
   ここを崩す変更は、他がどれだけ良く見えても採らない。

3. **仕様の根拠はコードとメモに書く。** ホームズ君 IFC の癖・VW SDK の落とし穴・
   なぜその値なのかは、対応するコメント（`なぜ` を書く）と `docs/DEV-NOTES.md` に残す。

4. **既存の図面リソースを作らない・書き換えない。** ユーザーの図面に名前付きリソースを
   増やさない。スラブ／ウォールスタイルもデータタグスタイルも作らず、構成層・基準面・
   タグレイアウトは**各オブジェクトへ直接**与える。

   **例外は「耐力壁の伏図記号のシンボル」3 つだけ**（M19）。記号は**利用者が描き直せる形**
   で図面に持たせたい、というご要望による——シンボルなら 1 か所直せば全部の耐力壁に効き、
   用紙基準（`ovSymDefPageBased`）にできるので**縮尺非追従**にもなる。例外は次の 2 つの
   縛りとセットで、**無いときだけ登録し、在れば触らない**（利用者が描き直したものを
   上書きしない）・**登録するのはその 3 つだけ**。増やすときはここへ足すこと。

5. **空のもの（レイヤ・レベル・凡例）を先に作らない。** 描画対象がある要素にだけ作る。

6. **決定性を守る。** エンティティ列挙順に依存しない結果を出す。ソート・集約は明示的に。

## アーキテクチャ: 2 フェーズ分離

処理は **IFC 解析フェーズ** と **VectorWorks 描画フェーズ** に完全分離する。
両フェーズは**命令セット（Document）**だけで接続し、SDK との密結合を解析側に
持ち込まない。

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

- **VectorWorks SDK を一切 include しない。** 通常の C++ ツールチェインだけで
  コンパイル・単体実行・テストできる（テンプレートの `UpdaterParse` と同じ立ち位置）。
- **最小 STEP リーダ**（自前）で IFC を読む。ホームズ君が出す既知サブセット向けに、
  STEP トークナイザ＋エンティティグラフ（`byType(name)` / インデックスによる属性
  アクセス / 逆参照 lookup）を提供する。**幾何エンジン（OpenCASCADE 等）は使わない**
  ——配置行列・断面・押し出しの幾何計算は `core/Geometry` ＋ `parse/IfcGeometry` の
  自前計算で行う。
- **読み込み時サニタイズは不要**: ホームズ君の IFC2X3 には IFC4 専用エンティティ
  （`IFCFOOTINGTYPE`）が混入するが、自前 STEP リーダ（`parse/Step`）は**スキーマ検証を
  せず非正規エンティティもそのまま読める**。本プラグインはそれらの型を参照しないので、
  グラフ上に残っていても無害。したがって `parse/Loader` はファイル読み込み
  （テキスト→STEP グラフ）だけを担い、サニタイズ処理は持たない。
- 出力は **Document**（下記）。ここに SDK ハンドルや STEP エンティティポインタ等の
  「フェーズ間で運べないもの」を入れない。

### Phase 2 — `draw/`: VectorWorks SDK 依存

- **VectorWorks SDK のみに依存**し、IFC / STEP の知識を持たない。
- Document を**検証**（`validateDocument` 相当）してから SDK API で描画する。
- 実描画（高さ・傾き・スタイル・PIO の挙動）は SDK 上で最終確認する（自動テスト困難）。

### 命令セット（Document）

- **プレーンな構造体**（`std::vector`・`std::string`・`double`・`enum` 等の集約）で表す。
- スキーマは `stories` / `grids` / `members` / `columns` / `walls` / `wallJoins` / `slabs` /
  `floors` / `rafters` / `roofs` / `anchorBolts` / `floorPosts` / `fireBraces` / `joints` /
  `columnMarks` / `shearWalls` / `sheets` / `sections` / `sectionSheet`。**同型が並ぶところは構造体 1 つへまとめる**
  ——`anchorBolts` / `floorPosts` / `fireBraces` / `joints` は中身が同じなので
  `core::SymbolCommand` 1 つで受け、要素の区別は「Document のどのリストか」が担う
  （`core/Document.h` の doc コメント参照）。
- **突き合わせが要る関係は入れ子で持つ**（データタグは `ViewportCommand::tags`、
  グラフィック凡例は `SheetCommand` の中）。平らに並べて番号で突き合わせる形にしない。
- **描くときにしか決まらないものは命令に持たせない。** 用紙の大きさはシートレイヤから
  SDK で読むものなので、それに依る値——縮尺・用紙上の位置・軸組図が何枚の用紙に分かれるか
  ——は解析側では決められない。命令が持つのは**枚数や用紙に依らない指示**だけにする
  （軸組図なら `sectionSheet` の「番号の始まり（伏図の続き）とタイトルの基」）。
  決め方そのものは SDK と無関係な算数なので `core/Layout` に置き、無 SDK でテストする。
- フェーズ間の受け渡しは**構造体のまま**行う。JSON 直列化は**予定に無い**。デバッグで
  ダンプが要る場面が実際に出てきたら、そのときに最小限を足す。
- スキーマを変えるときは、構造体定義・`validateDocument`・テストを同時に更新する。

## ディレクトリ構造

`src/` の全体像（どのファイルが何を担うか）は
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)「ソースの構成」にある。ここには**作業する
ときに守る置き場所の規約**だけを書く。

要素を 1 つ足すときの型は決まっている: `parse/<要素>.{h,cpp}`（解析）＋
`core/Document.h` の命令構造体と `validateDocument` の検証＋`draw/<要素>.{h,cpp}`（描画）＋
`tests/Parse<要素>Tests.cpp`（無 SDK テスト）＋`parse/Summary.cpp` の `kElements` に 1 行。

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
壁結合・地中梁・床付け**）は `parse/Footing.h`、`draw/` の SDK 呼び出しの定型（クラス分け・レイヤ用意・
プラグインスタイル解決・**構成層／基準面を各オブジェクトへ直接与える手順**——床板・底盤・
立上りが共有する。スラブ・壁は**スタイルを作らない・当てない**）は
`draw/DrawUtil`、**シートレイヤの用意とビューポートの仕上げ**（表示レイヤの絞り込み・クラス表示・
縮尺・図番／図面タイトル・更新に加え、**用紙と印刷可能領域の読み取り**（`SheetPaperArea`。用紙は 167/168・余白は `GetPageMargins`・インチ→mm と
「用紙は原点中心」の規約）と**測って動かす位置合わせ**（`PlaceViewport`）——伏図と軸組図が
共有する唯一の実装）も `draw/DrawUtil`、**用紙の割り付けの決め方**（縮尺の階梯と選び方・伏図の
縮尺と位置（**縮尺は凡例の幅を引いてから**決める。引く幅は**実測した凡例の幅**）・軸組図の上下 2 段とシートの分割・
タイトルの連番）は `core/Layout`、
**断面寸法データタグ**（断面の注釈空間への投影は `parse/Tag` の `sectionAnnotationPoint`、
`Data Tag` PIO の登録名・引出線パラメータと配置手順、そして**タグレイアウト（＝タグ 1 本の
中身）の組み方**——式・フィールドラベル・文字スタイル名・**クラス名（"寸法"。タグ本体と中の
文字の両方に与える）**——は `draw/Tag`。伏図と軸組図が共有する
唯一の実装で、**スタイルは作らないし当てない**（スラブ・壁と同じく各オブジェクトへ直接設定
する））、**グラフィック凡例**
（スタイル名は `parse/Sheet` の `kFoundationLegendStyle` / `kFloorLegendStyle`、
`GraphicLegend` PIO の登録名・箱幅／線の太さ／塗りと配置手順は `draw/Legend`。**用紙を
どれだけ空けるかは定数ではなく実測**——`measureLegendWidth` で測った幅を
`core::planLayout` へ渡し、置き場所はその `legendTopRight`）、
構造材ツール（StructuralMember PIO）のフィールド名・値・生成手順は
`draw/StructuralMember`、ハイブリッドシンボルの配置は `draw/Symbol`（4 要素で共有する唯一の
実装）、伏図記号レイヤ名（`{to}-柱伏図記号`）と記号の作図クラス・シンボル名は
`parse/ColumnMark`、記号 PIO の登録名・パラメータ名は `Extensions/ExtColumnMark.h`、**耐力壁**の要素判別
（`isShearBrace` / `isShearPanel`）・レイヤレベル名・柱を探す許容は `parse/ShearWall.h`、
耐力壁 PIO の登録名・パラメータ名・**伏図記号のシンボル名**・**PIO が自分の絵へ与えるクラス**
（伏図記号／面材の表・裏。ハッチングの向きで表裏を分ける 2 クラス）は
`Extensions/ExtShearWall.h`（**シンボル定義そのものを登録するのは `draw/ShearWall`** の
`EnsureMarkSymbols` ただ 1 か所。記号の寸法＝用紙 mm もそこ）、
**PIO のパラメータを読む口**（`PioParamString`）と**構造材の構造用途を読む述語**
（`StructuralUseOf`。柱記号 PIO と耐力壁 PIO が共有）は `draw/DrawUtil`、
**凸多角形の矩形クリップ**は `core/Geometry` の `clipPolygonToRect`（耐力壁の筋かいの形
`core::shearWallBracePolygon` が唯一の利用者）、span レベルの表記（`1` / `2.5`）は `parse/Story` の `formatSpanLevel`
（span 柱レイヤと伏図記号レイヤが共有）、「命令インデックス → ハンドル」の対応表は
`draw/ObjectHandles`（宣言）＋ `draw/DrawUtil`（SDK 型を持つ実体）、**描画側から切り離せる純計算**（レイヤの希望スタック順
`desiredStoryLayerOrder`・地中梁の可視ソリッドの呑み込み `raiseModifierTop`・図に映るものの
広がり `planContentBounds` / `sectionContentSize`）は `core/Document`、
進捗の見出し・バー配分は `draw/ExecuteDocument`（要素ごとのフェーズ）と `core/Progress`
（整形と配分の計算）に**それぞれ 1 つだけ**置く。**完了ダイアログに並ぶ要素の一覧**
（表示名・助数詞・命令数の取り出し・描けた数）は `parse/Summary` の `kElements` ただ 1 つの表で、
要素を足すときに触るのはその 1 行だけ（SDK 側は組み上がった本文を出すだけ）。**診断ログへの
書き出し口**は `core/Progress` の `beginPhase` 1 か所（各要素へ `trace::log` を撒かない。
開始・終了・例外だけを `Extensions/ExtMenu` が書く）。テスト側も同じで、フィクスチャ一覧・近似比較・**実 IFC の読み込みと命令セットの組み立てを
1 プロセス 1 回に畳むキャッシュ**（`fixture` / `fixtureDocument`）は `tests/Fixtures.h`、
共有する試験用屋根面と最小 IFC は `tests/RoofSample.h` が唯一の定義。

**依存の向きは厳守する:** `parse/` と `core/` は VectorWorks SDK を include しない。
`draw/` は STEP / IFC を include しない。両者をつなぐのは `core/Document.h` だけ。
この規律は CI で**parse/core を無 SDK でコンパイル・テストできること**によって担保する。

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
  `UPPER_SNAKE`（ファイル内で統一）。要素ごとの組み立て関数は `buildGridCommands` /
  `buildFootingCommands` のように `build<要素>Commands` で揃える。
- **Document のフィールド名**は図面側の語彙に合わせる（`class` は予約語なので `drawClass` /
  `className`）。意味は各構造体の doc コメントに書く。

### 幾何の型

- `parse/` は SDK 非依存なので、**SDK の幾何型（`WorldPt3` 等）を使えない**。
  `core/Geometry.h` の自前 `Vec2` / `Vec3` / `Mat4`（配置行列合成用）を使い、行列・断面・
  押し出しの数式はここに置く。
- `draw/` では SDK の幾何型と自前 `Vec*` を相互変換する薄いヘルパーを 1 か所に置く
  （変換規約を分散させない）。

### エラーハンドリング・所有権

- **STEP パースの失敗・想定外エンティティ**には寛容にふるまう（`continue` してスキップ、
  フォールバック描画）。1 要素の欠損で全体を止めない。
- **例外は parse 内部の局所処理に留める**。フェーズ境界（`buildDocument` の戻り）は
  値で返す。SDK コールバック（`plugin_module_main` / `DoInterface`）へ例外を漏らさない。
- **RAII で所有権を明示**。生ポインタの所有は避け、SDK ハンドルは Document に載せない
  （フェーズ間で運べない）。描画で必要なハンドルの受け渡しは「命令インデックス →
  ハンドル」の対応（`draw/ObjectHandles`）で行う（横架材ハンドル→タグ、壁ハンドル→壁結合
  など）。
- **決定性を守る**。エンティティ列挙順に依存しない結果を出す。ソート・集約は明示的に。

### コメント・言語

- **日本語コメントを基本**とする。既存ソースの重厚な手折りコメントの density に合わせる。
  `ReflowComments: false` のため折り返しは著者責任。
- **なぜ（意図・仕様の根拠）を書く**。ホームズ君 IFC の癖・VW SDK の落とし穴・実機で
  切り分けた経緯は、そのコードの近くに残す（大きな知見は `docs/DEV-NOTES.md` にも足し、
  コメントからはその節を指す）。

## テスト方針

テストの一覧・何をテストしていないかは `tests/README.md`。ここは方針だけ。

- **`core/` `parse/`**: **無 SDK で単体テスト**（`tests/TestFramework.h` を使う）。
  - STEP リーダ・幾何計算・各 parse モジュールを、**実 IFC フィクスチャ**
    （`tests/fixtures/`）に対してテストする。全フィクスチャで `buildDocument` が例外なく
    通ること（＋決定性）は、要素を足すたびに確認する。
  - **期待値は手書きする。** 数値は許容誤差付きで比較する。
  - CI（`test.yml`）で ASan + UBSan 付き・カバレッジ付きで常時回す。
- **`draw/`**: SDK 依存のため CI での完全な実行は難しい。
  - ロジック（レイヤ順の並べ替え計算、地中梁の呑み込み等）で SDK から切り離せる部分は
    `core/` 側へ寄せて無 SDK テストする。
  - 実描画は**ローカルの VectorWorks で目視確認**する（作法は
    `docs/DEV-NOTES.md`「実機確認の作法」）。SDK 呼び出しの薄いラッパーは、必要なら
    SDK モックで「正しい引数で呼んだか」を検証する程度に留める。

## 既知の制限・非目標

- **ホームズ君 IFC 以外の汎用 IFC 対応は非目標**。既知サブセット前提で最小 STEP リーダを組む。
- **一度に全部を変えない。** 1 変更＝1 要素（または 1 サブ機能）で、実機確認まで含めて閉じる。
- そのほかの制限（床版の開口・配筋・ロフト床の近似・柱記号の追随・断面の範囲）は
  `README.md`「既知の制限」と `docs/DEV-NOTES.md`「残っている宿題」にある。

## 開発プロセス: PR とマージ

1. **PR は自動で作ってよい。** コード編集後、ユーザーに確認すべき疑義が特に無ければ PR を
   作成し、`subscribe_pr_activity` で CI 結果とレビューコメントを監視する。CI の失敗は原因を
   診断して修正コミットを push する（待機は必ず `ci-wait`。上記「CI の完了を待つ」）。

2. **マージは「ローカル目視確認が済んでから」。** CI が全て green でも、**実描画が変わる変更
   （`draw/` を含む PR）は勝手にマージしない**——ユーザーが VectorWorks 実機で確認し、
   「確認できた」と伝えるまで PR を open のまま待つ。理由は本リポジトリの前提そのもので、
   **実描画（高さ・傾き・スタイル・PIO の挙動）は CI では検証できず、ローカルの VW でしか
   最終確認できない**（「テスト方針」「開発の基本方針」1）。確認前にマージすると、
   **未確認の描画が main に積み上がり、後で不具合が出たときにどの変更が原因か切り分けられなく
   なる**。「コード実装済み・CI green」は目視確認の代わりにならない。

3. **実機確認の要らない変更は CI green で自動マージしてよい**（`core/` `parse/` だけの変更・
   テスト・ドキュメント・CI 設定など、描画に触れないもの）。判断に迷うなら 2 に倒す。

4. **「CI が green なら自動マージ」は本リポジトリの規則ではない。** ネイティブプラグインで
   あり、リリースが stable / dev のビルドとして配布されるので、描画に触れる変更は必ず
   実機確認を挟む（上記 2）。

5. **コミットメッセージ**には Claude セッション URL を入れる
   （`https://claude.ai/code/session_<SESSION_ID>` の形式）。

## ビルド・リント・リリース

`docs/DEVELOPMENT.md` に、ローカルビルド（`VW_SDK_DIR` 指定）・dual build
（`VW_DEV_BUILD`）・テストとカバレッジ・lint（`scripts/lint.sh`）・CI とリリース・
自動アップデートの詳細がある。プラグイン識別子（バンドル名・VCOM ユニバーサル名・
拡張機能 UUID など）の一覧も同ファイルの「プラグイン識別子」節にある
（プラグイン名は `HomeskzIfcImport`）。

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
