# ROADMAP — ホームズ君 IFC インポート C++ 移植

Python プラグイン（`vectorworks-plugin-script-import-ifc-homeskz`）を C++ SDK ネイティブ
プラグインへ移植するための計画。設計原則・規約は `CLAUDE.md` を参照。

## 進め方の原則

- **1 マイルストーン = 1 縦切り**。各マイルストーンは
  「IFC パース → 命令セット（Document）→ VW 描画 → **ローカル VectorWorks で目視確認**」
  の 1 周が回る、単体で価値のある増分にする。
- **小さく積む**。大きな要素（基礎・横架材）はサブマイルストーンに割る。
- **依存順に進む**。土台（幾何・ストーリ）を先に作り、それに乗る要素を後に。
- 各マイルストーンの完了条件（Definition of Done）:
  1. `parse/` の解析＋`core/` の型が**無 SDK で単体テスト**を通る（実 IFC フィクスチャ）。
  2. 可能なら Python 版 `build_document` 出力との**ゴールデン比較**が通る。
  3. `draw/` が SDK ビルドでコンパイルできる。
  4. **ローカル確認チェックリスト**をユーザーが VectorWorks 上で確認済み。
  5. lint（`scripts/lint.sh`）通過・PR 作成・CI green。
- 実描画の正否は**ローカルでしか確定できない**。仕様に迷いが出たら、次へ進む前に
  Python 版 CLAUDE.md の該当節と実装・テストを読み、必要ならユーザーに確認する。

進捗記号: ⬜ 未着手 / 🟨 進行中 / ✅ 完了

---

## M0 — 基盤整備（骨組み・STEP リーダ・Document 土台）

**目的:** テンプレートを本プラグインへ改名し、2 フェーズの骨組みと無 SDK テスト土台を敷く。
描画対象はまだ無し（土台のみ）。

- ✅ プレースホルダー識別子の置換（`SamplePlugin` → `HomeskzIfcImport`。バンドル名・`.vwr`・
  VCOM ユニバーサル名・**UUID を再生成**・namespace（`CExtMenuImportIfc` /
  `CImportIfcMenu_EventSink`）・`VW_REPO`・メニュー `ファイル` ▸ `ホームズ君IFCをインポート…`）。
  現在の識別子は README「プラグイン識別子」節を参照。`.vwstrings` は UTF-16LE/BOM/CRLF を保持。
- ⬜ ディレクトリ骨組み: `src/core/` `src/parse/` `src/draw/` と CMake ターゲット分割。
  **`parse/`・`core/` は VectorWorks SDK を include しない**ビルド構成にする（無 SDK で
  コンパイルできることを CI で担保）。
- ⬜ `core/Geometry.h`: `Vec2` / `Vec3` / `Mat4`（配置行列合成に足りる最小限）。
- ⬜ `core/Document.h`: 空の Document 構造体（バージョン＋各命令リストの器）と
  `validateDocument` の骨組み。任意で `core/DocumentJson`（デバッグ／ゴールデン用）。
- ⬜ `parse/Step`: **最小 STEP リーダ**。トークナイザ＋エンティティグラフ
  （`byType(name)` / インデックス属性アクセス / 逆参照 lookup）。ホームズ君サブセット前提。
- ⬜ `parse/Loader`: サニタイズ（`IFCFOOTINGTYPE` 除去、または未知エンティティ読み飛ばし）。
- ⬜ `tests/`: フィクスチャを Python 版 `tests/fixtures/` から流用。`StepTests` で
  リーダを実 IFC に対してテスト（`by_type` の件数など）。
- ⬜ メニューコマンドを「IFC をインポート」の器へ改修（ファイル選択ダイアログ →
  `openIfc` → まだ描画せず件数をダイアログ表示、程度）。

**ローカル確認:** メニューからコマンド実行 → IFC を選ぶ → 「グリッド軸 N 本を検出」等の
件数がダイアログに出る（パースが動いている確証）。

---

## M1 — 通り芯（グリッド）★最初の縦切り

**目的:** 2 フェーズが端から端まで通ることを、最も単純な要素で実証する。
grids は配置行列・断面・ストーリを必要としない（`IfcGridAxis` → ポリライン → 中心
オフセット → `GridAxis` オブジェクト）。**Python: `ifc/grid.py` / `vw/grid.py`。**

- ⬜ `parse/Grid`: `IfcGridAxis` の `AxisCurve`(`IfcPolyline`) 端点を取得、重複線除去、
  bbox 中心でセンタリング、X/Y 通り判定（名前 `X`/`Y` 始まり、無ければ `|Δx|<|Δy|`）、
  クラス名付与 → `GridCommand`。
- ⬜ `core/Document`: `GridCommand`（`label` / `layer='共通'` / `drawClass` / `start` / `end`）。
- ⬜ `draw/Grid`: `共通` レイヤを（無ければ）作成し、`CreateCustomObjectPath('GridAxis', …)`
  で生成。失敗時は通常線にフォールバック。`Label` / `ShowBubbleAt='Start Point'` を設定。
- ⬜ `draw/ExecuteDocument`: grid 命令だけをディスパッチする最小版。
- ⬜ テスト: `ParseGridTests`（フィクスチャで線の本数・センタリング・クラス判定を検証。
  Python 版 `test_ifc_grid.py` を写す）。

**ローカル確認:** IFC を選ぶと `共通` レイヤに通り芯が描かれ、X/Y でクラス分けされ、
軸名ラベルと基点バブルが出る。位置が原点付近にセンタリングされている。

---

## M2 — 幾何の土台（配置行列・押し出しソリッド・断面）

**目的:** M3 以降のほぼ全要素が使う共有の幾何計算を、描画なしで先に固めて de-risk する。
**Python: ifcopenshell 依存部の自前計算（`ifc/footing.py` の `_world_solid` 等）＋
`ifc/member.py` の `_get_placement_3d` / `_get_profile_dims`。**

- ⬜ `parse/IfcGeometry`: `IfcLocalPlacement` / `IfcAxis2Placement3D` の合成 → ワールド行列。
- ⬜ 押し出しソリッド（`IfcExtrudedAreaSolid`）のワールド変換。押し出し方向が鉛直／水平で
  平面外形の求め方を分ける（Python 版 `_world_solid` に準拠）。
- ⬜ 断面プロファイル: `IfcRectangleProfileDef`（`XDim`/`YDim`）と、任意断面
  `IfcArbitraryClosedProfileDef`（登り梁・火打の外形）。
- ⬜ 差演算 `IfcBooleanResult` の第 1 オペランド（素のソリッド）を辿る。
- ⬜ テスト: `GeometryTests`。**Python 版が同フィクスチャで算出する座標・寸法と許容誤差で
  突き合わせる**（移植の数値ズレを機械的に検出）。

**ローカル確認:** （描画なし。テストのみ。）

---

## M3 — ストーリ（階・レベル・レイヤ）

**目的:** 以降の要素の配置先となるストーリ・ストーリレベル・デザインレイヤを生成する。
**Python: `ifc/story.py` / `vw/story.py`。**

- ⬜ `parse/Story`: `…FL` で終わる `IfcBuildingStorey` を対象に、`Elevation`＝ストーリ高さ、
  `IfcColumn`/`IfcSlab` のローカル Z 最大値（≤0）＝横架材天端オフセットを算出。
  階名・suffix・レイヤ名・レベル（`FL`/`横架材天端`、最上階 `軒高`）を組み立て → `StoryCommand`。
  （屋根組・登り梁・span 柱レベルは各要素の導入時に追加。まずは基本レベルのみ。）
- ⬜ `draw/Story`: `CreateStory`→`SetStoryElevationN` を 1 階ずつ。レベルは
  `CreateLevelTemplateN`＋`AddLevelFromTemplate`＋`GetLayerForStory`＋`SetName`。
- ⬜ レイヤのスタック順並べ替え（`reorder_story_layers` 相当）。まずは基本レイヤの希望順。
  床レイヤを最背面へ回す方針は Floor 導入時に効いてくる（枠だけ用意）。
- ⬜ テスト: `ParseStoryTests`（`test_ifc_story.py` を写す。階数・レベル・オフセット）。

**ローカル確認:** ストーリと `n-FL`/`n-横架材天端`/`R-軒高` 等のレイヤが階数分でき、
ナビゲーションのレベル高さが正しく、レイヤのスタック順が希望どおり。

---

## M4 — 構造クラス判定（純ロジック）

**目的:** 柱・横架材の `04構造-02木造-…` クラス割り当てを純ロジックとして移植。
描画は無し（M5/M6 が使う）。**Python: `ifc/structural_class.py`。**

- ⬜ `parse/StructuralClass`: 種別トークン→クラスの対応表、名前で判別できないときの
  状況推定（階・軒高・貫通判定）。基礎クラス（立上り／底盤）も定義。
- ⬜ テスト: `test_ifc_structural_class.py` を写す。

**ローカル確認:** （描画なし。M5/M6 の描画時にクラス分けで確認。）

---

## M5 — 横架材（梁）

**目的:** 中核要素。土台・梁・桁をストーリレベルにバインドして描く。傾斜梁・食い込み
調整は基本形のみ先に、登り梁の任意断面と屋根スナップは M11 へ回す。
**Python: `ifc/member.py` / `vw/member.py`。**

- ⬜ `parse/Member`: 配置・断面・材種 → `MemberCommand`。天端中央線への基準点補正、
  高さバインド（`start_bound`/`end_bound`）、傾斜梁（`elevation≠end_elevation`）、
  食い込み調整（`resolve_member_interferences`）。母屋・棟木の `n-母屋` 分離は
  M11 で（まずは横架材天端／軒高レイヤ）。
- ⬜ `core/Document`: `MemberCommand` ＋ `StoryBoundCommand`。
- ⬜ `draw/Member`: `CreateCustomObjectPath('StructuralMember', …)`＋`Move3D`＋
  `SetObjectStoryBound`（上下端）＋`SetPluginStyle('木質構造材_横架材')`。全配置後に
  `UpdateStyledObjects` を 1 回。**パスに Z 成分を持たせない**（傾斜は bound の offset 差）。
- ⬜ テスト: `ParseMemberTests`（`test_ifc_member.py`）。

**ローカル確認:** 各階に土台・梁・桁が正しい高さ・断面・クラスで並ぶ。段差梁・傾斜梁の
高さが二重加算されない。オブジェクト編集で高さがリセットされない。

---

## M6 — 柱（管柱・通し柱・小屋束）

**目的:** 柱を構造材ツールで鉛直材として描き、span レイヤに分けて配置する。
**Python: `ifc/column.py` / `vw/column.py`。**

- ⬜ `parse/Column`: 断面・柱高さ・種別・柱頭/柱脚金物・**span（from/to レベル）判定**
  （`resolve_column_to_level`。上階梁下端＝M5 の members を参照）→ `ColumnCommand`。
  `{from}to{to}-柱` レイヤ、`structural_use`（柱=4／小屋束=5）、金物を含む `member_id`。
- ⬜ `parse/Story` 拡張: span 柱レベル（`column_layers_by_story`）をストーリに追加。
- ⬜ `draw/Column`: 構造材ツールで鉛直パス＋`Move3D`＋上下端バインド。小屋束は上端
  offset を下端と同値にして二重加算回避。`SetPluginStyle('木質構造材_柱・束')`＋
  全配置後 `UpdateStyledObjects`。
- ⬜ テスト: `test_ifc_column.py` / `test_ifc_column_span.py` を写す。

**ローカル確認:** 柱・小屋束が span レイヤ（`1to2-柱` 等）に分かれ、通し柱が複数階を
跨ぎ、下屋小屋束が上階に写り込まない。上端高さが正しい。

---

## M7 — 基礎（立上り＝壁・底盤＝スラブ）＋基礎ストーリ

**目的:** 基礎の主要 2 オブジェクトと基礎ストーリを描く。地中梁・人通口・壁結合・配筋は
M8 に分割。**Python: `ifc/footing.py`（一部）/ `vw/footing.py`。**

- ⬜ `parse/Footing`: 基礎ストーリ命令（`基礎`/suffix `F`/GL=0、レベル 4 種）。
  立上り（`基礎梁…`）→ `WallCommand`（マージ `merge_wall_commands`・自由端半壁厚延長）。
  底盤（`底盤`）→ `SlabCommand`（マージ `merge_slab_commands`・外面合わせ
  `align_slabs_to_wall_faces`・`thickness`）。
- ⬜ `draw/Footing`: 壁（`DoubLines`→`Wall`→`SetWallOverallHeights`→`SetWallStyle`）、
  スラブ（`CreateSlab`→スラブスタイル→`SetSlabHeight`→`SetObjectStoryBound`）。
  スラブスタイルは `BuildResourceList` で列挙・厚み別に複製。
- ⬜ テスト: `test_ifc_footing.py` の該当部を写す。

**ローカル確認:** 基礎ストーリができ、立上りが壁・底盤がスラブで正しい高さ・厚み・
クラスで描かれ、連続する立上り／底盤が 1 本／1 枚に統合される。

---

## M8 — 基礎の高度化（地中梁・人通口・壁結合・配筋）

**目的:** 基礎の残り機能。M7 の上に積む。**Python: `ifc/footing.py` / `ifc/reinforcement.py`。**

- ⬜ 地中梁: 台形プリズムのモディファイア（`SetCustomObjectProfileGroup` で clip）＋
  可視 3D ソリッド（`BeginXtrd`＋回転規約＋オブジェクト変数 1160/702）。マージ・底盤振り分け。
- ⬜ 人通口: 立上りソリッドの天端下方削り取りを抽出し、区間を分割／切り下げ。
- ⬜ 壁結合: `JoinWalls`（L/T/X・`capped`・ピック点）。壁ハンドルを命令インデックスで受け渡し。
- ⬜ 配筋: レコード `配筋` を立上り・底盤に設定（`Pset_Reinforcement`、鋼種接頭辞除去、
  短辺/長辺→X/Y）。
- ⬜ テスト: `test_ifc_reinforcement.py` ほか。

**ローカル確認:** 地中梁が底盤に噛み合い、人通口で立上りが切り下がり、コーナー／T／十字が
きれいに結合し、立上り・底盤に配筋レコードが載る。

---

## M9 — 床板

**目的:** 各階 `n-FL` に床ツールで床を描き、段差（スキップフロア）を表現。
床レイヤを最背面へ回す並べ替えを効かせる。**Python: `ifc/floor.py` / `vw/floor.py`。**

- ⬜ `parse/Floor`: `床版`(`IfcSlab`) → `FloorCommand`（厚み 24mm 固定、床下端絶対 Z、
  横架材天端バインド＋段差 offset）。
- ⬜ `draw/Floor`: `BeginFloor`→外形ポリゴン→`EndGroup`→`Move3D`→`SetObjectStoryBound`。
- ⬜ `draw/Story` の並べ替えで床レイヤを最背面へ（枠は M3 で用意済み）。
- ⬜ テスト: `test_ifc_floor.py`。

**ローカル確認:** 各階に床が描かれ、段差床の高さが IFC どおりずれ、床が柱梁を覆い隠さない。

---

## M10 — シンボル置換系（アンカーボルト・床束・火打・仕口）

**目的:** ハイブリッドシンボルへの置換 4 種。互いに独立で小さく、まとめて 1 マイルストーン。
**Python: `ifc/{anchor_bolt,floor_post,fire_brace,joint}.py` / 対応 `vw/`。**

- ⬜ アンカーボルト（`IfcMechanicalFastener`→`アンカーボルト_M12/M16`、`F-アンカーボルト`）。
- ⬜ 床束（大引の下に 910mm 間隔で決め打ち、継手統合・支持材芯、`床束`、`F-床束`）。
- ⬜ 火打（`火打:…`→`鋼製火打`、端面交点＋回転角、横架材レイヤ）。
- ⬜ 仕口（受ける材／柱のある横架材端部→`仕口`、member/column 命令から判定、横架材レイヤ）。
- ⬜ `draw/`: 各 `Symbol(名, (x,y), angle)`。テスト: 対応する `test_ifc_*.py`。

**ローカル確認:** 各シンボルが正しい位置・角度・レイヤに置かれる。

---

## M11 — 屋根組（垂木・野地板・登り梁）

**目的:** 屋根版から導出する屋根組と、母屋・棟木・登り梁の専用レイヤ分離。
**Python: `ifc/{rafter,roof,noboribari,member}.py`。**

- ⬜ 母屋・棟木の `n-母屋` 分離、登り梁の任意断面抽出と `n-登り梁` 分離（M5 の拡張）。
- ⬜ 垂木（屋根版から導出、`FramingMember` type=rafter、`n-垂木`、45×45@455）。
- ⬜ 野地板（屋根版 1 面＝1 枚、`BeginRoof`、`n-野地板`、12mm）。
- ⬜ 登り梁の位置補正（`correct_noboribari`: 端部詰め＋屋根面スナップ）。
- ⬜ ストーリに `母屋`/`垂木`/`野地板`/`登り梁` レベルを追加（屋根版・母屋・登り梁の有無で判定）。
- ⬜ テスト: `test_ifc_{rafter,roof,noboribari}.py`。

**ローカル確認:** 屋根面に沿って垂木・野地板が並び、母屋・棟木・登り梁が専用レイヤに分離される。

---

## M12 — 断面記号・伏図記号

**目的:** span 柱レイヤごとに `柱束伏図記号` PIO を配置（断面記号＝×/／、伏図記号＝シンボル）。
**Python: `ifc/column_mark.py` / `vw/column_mark.py`。**

- ⬜ `parse/ColumnMark`: span ごとに断面記号（`断面`）＋伏図記号（`平面`、`{to}-柱伏図記号`
  レイヤ、種別でシンボル選択）→ `ColumnMarkCommand`。
- ⬜ `draw/ColumnMark`: `柱束伏図記号` PIO、伏図記号レイヤの生成・`共通` 直下への並べ替え。
- ⬜ テスト: `test_ifc_column_mark.py`。

**ローカル確認:** 各 span レイヤに断面記号が載り、伏図記号レイヤに平面記号が描かれる。

---

## M13 — シート・伏図・データタグ・凡例

**目的:** シートレイヤとビューポート（基礎伏図／各階柱梁伏図／母屋伏図）、断面寸法データタグ、
グラフィック凡例。**Python: `ifc/sheet.py` / `ifc/tag.py` / `vw/sheet.py`。**

- ⬜ `parse/Sheet`: 基礎伏図・柱梁伏図（切断レベルで span 柱・伏図記号を絞る）・母屋伏図の
  `SheetCommand`/`ViewportCommand`。凡例 `LegendCommand`、タグ `TagCommand`。
- ⬜ `draw/Sheet`: シートレイヤ／ビューポート生成、表示レイヤ絞り込み・クラス・縮尺、
  データタグ（`Data Tag`＋関連付け＋ビューポート注釈）、凡例（`GraphicLegend`＋スタイル＋
  `UpdateStyledObjects`）。横架材ハンドルを命令インデックスで受け渡し。
- ⬜ テスト: `test_ifc_sheet.py` / `test_ifc_tag.py`。

**ローカル確認:** 各伏図がシートに並び、表示レイヤ・縮尺が正しく、断面寸法タグと凡例が載る。

---

## M14 — 断面ビューポート（軸組図）

**目的:** 既製の 40 枚（`X1`〜`X20`/`Y1`〜`Y20`）の断面指示線・ビューポートを、柱梁の芯を通る
通りへ移動・改名・削除・整列。**Python: `ifc/section.py` / `vw/section.py`。**

- ⬜ `parse/Section`: 柱梁の芯を X/Y 方向に検出（大引・母屋は梁とみなさない）、命名（名前付き
  通り芯・中間通りの `'`/`又`）、既製図番へ割り当て → `SectionCommand`。
- ⬜ `draw/Section`: 既製指示線を辞書化し、移動・改名・リンク先ビューポート更新・未使用削除・整列。
- ⬜ テスト: `test_ifc_section.py`。

**ローカル確認:** 軸組図シートに、柱梁が通る各通りの断面ビューポートが図番・タイトル付きで並ぶ。

---

## 完了後

- Python 版と C++ 版の**ゴールデン比較**を全フィクスチャで回し、意図的な差分以外がないことを確認。
- README を移植版の内容へ更新（ビルド・使い方・Python 版との差異）。
- Python 版で未実現／トリッキーだった箇所のうち、C++ SDK でより良く実装できたものを記録。

## 順序の要点（依存関係）

```
M0 基盤 ─┬─ M1 通り芯（縦切り実証）
         └─ M2 幾何土台 ── M3 ストーリ ── M4 構造クラス
                                │
                    ┌───────────┼───────────────┐
                 M5 横架材    M7 基礎(壁/スラブ) M9 床板
                    │           │
                 M6 柱      M8 基礎(高度)
                    │
      ┌─────────────┼───────────────┐
   M10 シンボル   M11 屋根組     M12 記号
                                   │
                              M13 伏図/タグ/凡例 ── M14 軸組図
```

各要素の詳細仕様は姉妹リポジトリ `vectorworks-plugin-script-import-ifc-homeskz` の
`CLAUDE.md` および該当ソース・テストを一次資料とする。
