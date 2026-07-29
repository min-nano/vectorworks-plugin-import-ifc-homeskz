# ROADMAP — ホームズ君 IFC インポート C++ 移植

Python プラグイン（`vectorworks-plugin-script-import-ifc-homeskz`）を C++ SDK ネイティブ
プラグインへ移植するための計画。設計原則・規約は `CLAUDE.md` を参照。

## 進め方の原則

- **1 マイルストーン = 1 縦切り**。各マイルストーンは
  「IFC パース → 命令セット（Document）→ VW 描画 → **ローカル VectorWorks で目視確認**」
  の 1 周が回る、単体で価値のある増分にする。
- **小さく積む**。大きな要素（基礎・横架材）はサブマイルストーンに割る。
- **依存順に進む**。土台（幾何・ストーリ）を先に作り、それに乗る要素を後に。
- **形状を先に確定し、支持部材を形状へ合わせる**（下記「実装順序の方針」）。
  床・屋根・屋根組は建物の形状そのものなので先に位置決めを確定し、横架材・柱等の
  支持部材はその形状に合わせて補正する。
- 各マイルストーンの完了条件（Definition of Done）:
  1. `parse/` の解析＋`core/` の型が**無 SDK で単体テスト**を通る（実 IFC フィクスチャ）。
  2. 可能なら Python 版 `build_document` 出力との**ゴールデン比較**が通る。
  3. `draw/` が SDK ビルドでコンパイルできる。
  4. **ローカル確認チェックリスト**をユーザーが VectorWorks 上で確認済み。
  5. lint（`scripts/lint.sh`）通過・PR 作成・CI green。
- 実描画の正否は**ローカルでしか確定できない**。仕様に迷いが出たら、次へ進む前に
  Python 版 CLAUDE.md の該当節と実装・テストを読み、必要ならユーザーに確認する。

進捗記号: ⬜ 未着手 / 🟨 進行中（コード実装済み・ローカル目視確認待ち） / ✅ 完了

現状 M0〜M4 は**すべて ✅ 完了**（コードはマージ済み・ローカル目視確認も済み）。
**M5 床板は実装済み（🟨 ローカル目視確認待ち）**。次は M6 屋根面へ進む。

## 実装順序の方針（形状先行）

**建物の形状を先に確定し、それに支持部材を合わせる。** ホームズ君 IFC のうち
**床（床版）・屋根（屋根版）・屋根組**は建物の外形・面そのものを表す一次情報で、
最終的な位置決めの基準になる。そこで、これらを**先に**取り込んで位置を確定してから、
**横架材（土台・梁・桁・母屋・棟木・登り梁）・柱（管柱・通し柱・小屋束）**という
支持部材を、確定した床・屋根面に合わせて補正しながら載せていく。

この順序は Python 版の実装順（部材が先・屋根は後）とは逆だが、意図に合致する。
Python 版でも実際には**屋根の形状が基準**になっている:

- **垂木・野地板は屋根版（`IfcSlab` の屋根面）から導出**される（IFC に垂木は出力されない）。
  屋根面の勾配・外形が一次情報で、部材ではなく面が形を決める。
- **登り梁はホームズ君の入力位置が不正確**（軸勾配が屋根版より急・端部が受け材へ食い込む）
  なため、`correct_noboribari` で**天端中央線を屋根面（垂木下面）へスナップ**して勾配・高さを
  屋根に合わせ、端部を受け材／柱の面まで詰める。つまり登り梁は「屋根面へ補正される支持部材」。
- **床（床版）は IFC の床位置を尊重**してスキップフロアの段差を絶対 Z で表す。床は基準高さ
  （横架材天端）にバインドしつつ段差を offset で持つので、床の位置自体が一次情報。

したがって本移植では、**先に床・屋根面（垂木・野地板）を据えて位置決めの基準を作り**、
続く横架材・柱でその基準へ合わせる（登り梁の屋根スナップ・食い込み調整など）ことで、
「形状 → 支持部材」の依存が自然に流れる。屋根面より先に部材を置いてから屋根に合わせ直す
Python 版の後処理（`correct_noboribari`）を、C++ では**屋根面を先に確定した上で**素直に適用できる。

**依存メモ:**
- 垂木・野地板は屋根版と**ストーリ（M3）の横架材天端レベル**だけに依存し、横架材（梁）には
  依存しない。垂木の差し込み（`embedment`）は真下の軒桁幅を参照するが、桁が未導入の段階では
  既定桁幅（`DEFAULT_GIRDER_WIDTH`）にフォールバックできる（後続の横架材導入後に精緻化）。
- 登り梁は横架材（`IfcBeam`）の一種なので**パーサは横架材（M7）と一体**。M7 で任意断面を抽出し、
  M6 で確定済みの屋根面へスナップ補正する（端部詰めは受け材＝M7 横架材・柱＝M8 を参照するため、
  柱導入後に最終化）。母屋・棟木も横架材の一種で M7 で `n-母屋` へ分離する。

## 現在の進捗（サマリ）

| マイルストーン | 状態 | 備考 |
| --- | --- | --- |
| M0 基盤整備 | ✅ 完了 | 骨組み・CMake 分割・STEP リーダ・Loader・無 SDK テスト、メニューの器改修（ファイル選択→parse→件数ダイアログ）まで完了（PR #4〜#10）。|
| M1 通り芯 | ✅ 完了 | 最初の縦切り。parse/draw/テスト実装・**マージ済み（PR #11）**・無 SDK テスト green・Python 版と整合。**ローカル目視確認も済み**。|
| M2 幾何の土台 | ✅ 完了 | 配置行列・押し出し・断面（`Mat4` 含む）。`core/Geometry`＋`parse/IfcGeometry` 実装・**マージ済み（PR #13）**・無 SDK テスト green。描画なし＝テストのみで完了。|
| M3 ストーリ | ✅ 完了 | 階・レベル・レイヤ（基本レベルのみ）。`parse/Story`＋`core`＋`draw/Story` 実装・**マージ済み（PR #14）**・無 SDK テスト green・**ローカル目視確認も済み**。屋根組・登り梁・span 柱レベルは後続 M で追加。|
| M4 構造クラス判定 | ✅ 完了 | `parse/StructuralClass`＋`ParseStructuralClassTests`（Python 版 test 移植）実装・**マージ済み（PR #16）**・無 SDK テスト green。基礎クラス（立上り／底盤）は M9 基礎で定義する。|
| **M5 床板（形状先行）** | 🟨 進行中 | 建物形状①。各階 `n-FL` にスラブで床を描き段差を表現。位置の一次情報。parse/draw/テスト実装済み・無 SDK テスト green（Python 版の期待値と一致）。**ローカル目視確認待ち**。|
| **M6 屋根面・屋根組（形状先行）** | ⬜ 未着手 | 建物形状②。屋根版から垂木・野地板を導出し屋根面を確定。以降の登り梁スナップの基準。|
| **M7 横架材（支持部材）** | ⬜ 未着手 | 土台・梁・桁＋母屋・棟木・登り梁。M5/M6 の床・屋根面へ合わせて補正（登り梁の屋根スナップ）。|
| **M8 柱（支持部材）** | ⬜ 未着手 | 管柱・通し柱・小屋束。span レイヤ分割（上階梁下端＝M7 参照）。|
| M9 基礎（壁・スラブ）＋基礎ストーリ | ⬜ 未着手 | 立上り＝壁・底盤＝スラブ・基礎ストーリ。上部形状とは独立。|
| M10 基礎の高度化 | ⬜ 未着手 | 地中梁・人通口・壁結合・配筋。|
| M11 シンボル置換系 | ⬜ 未着手 | アンカーボルト・床束・火打・仕口。|
| M12 断面記号・伏図記号 | ⬜ 未着手 | span 柱レイヤごとの `柱束伏図記号` PIO。|
| M13 シート・伏図・タグ・凡例 | ⬜ 未着手 | シートレイヤ・ビューポート・データタグ・グラフィック凡例。|
| M14 断面ビューポート | ⬜ 未着手 | 軸組図（既製 40 枚の指示線・ビューポートの移動／改名／整列）。|

直近の到達点（PR #4〜#16）: プレースホルダー識別子の置換と 2 フェーズ骨組み・CMake
ターゲット分割（無 SDK `HomeskzIfcCore`＋SDK 依存 `draw/`）、最小 STEP リーダ・ローダ、
IFC インポートコマンドの器（ファイル選択→summarize→件数ダイアログ）、**M1 通り芯・
M2 幾何土台・M3 ストーリ・M4 構造クラス判定を実装しマージ**、CI の SDK ビルド高速化（PR #15）。
M0〜M4 は**ローカル目視確認も完了**し、全て ✅。続けて**形状先行**の M5 床板を実装した
（床版の抽出・24mm 固定厚・IFC の床位置尊重＝スキップフロアの段差・横架材天端バインド、
併せて STEP の日本語文字エスケープのデコード）。ローカル目視確認を経て M6 屋根面へ進む。

---

## M0 — 基盤整備（骨組み・STEP リーダ・Document 土台）✅

**目的:** テンプレートを本プラグインへ改名し、2 フェーズの骨組みと無 SDK テスト土台を敷く。
描画対象はまだ無し（土台のみ）。

- ✅ プレースホルダー識別子の置換（`SamplePlugin` → `HomeskzIfcImport`。バンドル名・`.vwr`・
  VCOM ユニバーサル名・**UUID を再生成**・namespace（`CExtMenuImportIfc` /
  `CImportIfcMenu_EventSink`）・`VW_REPO`・メニュー `ファイル` ▸ `ホームズ君IFCをインポート…`）。
  現在の識別子は README「プラグイン識別子」節を参照。`.vwstrings` は UTF-16LE/BOM/CRLF を保持。
- ✅ ディレクトリ骨組み: `src/core/` `src/parse/` `src/draw/` と CMake ターゲット分割。
  **`parse/`・`core/` は VectorWorks SDK を include しない**ビルド構成にする（無 SDK で
  コンパイルできることを CI で担保）。→ 無 SDK 静的ライブラリ `HomeskzIfcCore`
  （`core/` + `parse/`）に分離し、プラグインは SDK 依存 `draw/` を上乗せ、テストは
  同ライブラリを無 SDK でリンク（PR #5）。
- ✅ `core/Geometry.h`: `Vec2` / `Vec3` / `Mat4`（配置行列合成に足りる最小限）。`Vec2`/`Vec3` は
  M0 で、`Mat4`（配置行列合成）とベクトル演算は M2 で実装済み。
- ✅ `core/Document.h`: 空の Document 構造体（バージョン＋各命令リストの器）と
  `validateDocument` の骨組み。任意で `core/DocumentJson`（デバッグ／ゴールデン用）。
  → `Document`（`version`）＋ `validateDocument` を実装（PR #5）。命令リストと
  `DocumentJson`（任意）は各要素マイルストーンで追加。
- ✅ `parse/Step`: **最小 STEP リーダ**。トークナイザ＋エンティティグラフ
  （`byType(name)` / インデックス属性アクセス / 逆参照 lookup）。ホームズ君サブセット前提。
  → `Model`（`entity` / `byType` / `referrers` / `resolve`）と `parseStep` を実装、
  `StepTests` で網羅（PR #6）。
- ✅ `parse/Loader`: ファイル読み込み（テキスト→STEP グラフ）。自前リーダは非正規
  エンティティ（IFC4 専用 `IFCFOOTINGTYPE` の混入等）を許容するため、Python 版
  `loader.py` のようなサニタイズ（除去）は不要（詳細は CLAUDE.md「アーキテクチャ」）。
  → `loadIfc` / `loadIfcFromText` を実装、非正規エンティティを除去せず読めることを
  `LoaderTests` で確認（PR #6）。
- ✅ `tests/`: フィクスチャを Python 版 `tests/fixtures/` から流用。`StepTests` /
  `LoaderTests` / `CoreDocumentTests`（無 SDK）を追加（PR #6）。ホームズ君の実 IFC
  フィクスチャを Python 版から一式流用済み（PR #8）。
- ✅ メニューコマンドを「IFC をインポート」の器へ改修（ファイル選択ダイアログ →
  `summarizeIfc`（Phase 1）→ まだ描画せず主要要素の件数をダイアログ表示）。件数集計と
  文言整形は無 SDK テスト（`ParseSummaryTests`）で検証済みで、SDK 側にはファイル選択と
  アラート表示だけを残す（PR #10）。

**ローカル確認:** メニューからコマンド実行 → IFC を選ぶ → 「グリッド軸 N 本を検出」等の
件数がダイアログに出る（パースが動いている確証）。

---

## M1 — 通り芯（グリッド）★最初の縦切り ✅

**目的:** 2 フェーズが端から端まで通ることを、最も単純な要素で実証する。
grids は配置行列・断面・ストーリを必要としない（`IfcGridAxis` → ポリライン → 中心
オフセット → `GridAxis` オブジェクト）。**Python: `ifc/grid.py` / `vw/grid.py`。**

- ✅ `parse/Grid`: `IfcGridAxis` の `AxisCurve`(`IfcPolyline`) の全点を取り連続点対
  （区間）ごとに 1 本、重複線除去（反転も同一）、bbox 中心でセンタリング、X/Y 通り
  判定（名前 `X`/`Y` 始まり優先、無ければ `|Δx|<|Δy|`）、クラス名付与 → `GridCommand`。
  **Python 版 `ifc/grid.py` と整合**（クラス名・区間分割・非ポリライン曲線のスキップ）。
- ✅ `core/Document`: `GridCommand`（`label` / `layer='共通'` / `drawClass` / `start` / `end`）
  と `Document.grids`。`validateDocument` に通り芯の検証（レイヤ名・非縮退）を追加。
- ✅ `draw/Grid`: `共通` レイヤを（無ければ）作成し、`CreateCustomObjectPath('GridAxis', …)`
  で生成。失敗時は通常線にフォールバック。`Label` / `ShowBubbleAt='Start Point'` を設定。
- ✅ `draw/ExecuteDocument`: grid 命令だけをディスパッチする最小版。メニューコマンドを
  `buildDocument`→`executeDocument`→本数ダイアログの流れへ改修。
- ✅ テスト: `ParseGridTests`（本数・センタリング・X/Y 判定・重複除去・区間分割・
  非ポリラインスキップ・欠損スキップ・決定性を検証。無 SDK で green）。
- ✅ **ローカル目視確認済み**。`共通` レイヤに通り芯が描かれ X/Y でクラス分け・軸名ラベル・
  基点バブルが出て、原点付近にセンタリングされることを VW 実機で確認。

**ローカル確認:** IFC を選ぶと `共通` レイヤに通り芯が描かれ、X/Y でクラス分けされ、
軸名ラベルと基点バブルが出る。位置が原点付近にセンタリングされている。

---

## M2 — 幾何の土台（配置行列・押し出しソリッド・断面） ✅

**目的:** M3 以降のほぼ全要素が使う共有の幾何計算を、描画なしで先に固めて de-risk する。
**Python: ifcopenshell 依存部の自前計算（`ifc/footing.py` の `_world_solid` 等）＋
`ifc/member.py` の `_get_placement_3d` / `_get_profile_dims`。**

- ✅ `core/Geometry`: `Vec2`/`Vec3` の演算と `Mat4`（単位・平行移動・基底からの生成・
  行列積・点/方向の適用）。剛体変換（回転＋平行移動）のみで剪断・スケールは持たない。
- ✅ `parse/IfcGeometry`: `IfcAxis2Placement3D`（Gram-Schmidt で正規直交化）→ 変換行列。
  要素配置 `resolveObjectPlacement` は **RelativePlacement のみ**を使い親を合成しない
  （Python 版と一致。親を合成すると階高が二重計上される）。
- ✅ 押し出しソリッド（`IfcExtrudedAreaSolid`）のワールド変換 → `WorldSolid`
  （配置基底・押し出し方向／長さ・プロファイル 2D 頂点・矩形寸法を保持）。鉛直・水平いずれの
  押し出し方向でも同じ経路で正しく変換される（テストで両方を検証）。
- ✅ 断面プロファイル: `IfcRectangleProfileDef`（`Position` は平行移動のみ）と任意閉断面
  `IfcArbitraryClosedProfileDef`（`OuterCurve` の外形）。
- ✅ 差演算 `IfcBooleanResult` / `IfcBooleanClippingResult` の第 1 オペランドを再帰で辿る。
- ✅ テスト: `GeometryTests`（無 SDK）。行列・配置・断面・押し出し・boolean 辿りを手計算値と
  突き合わせ、実フィクスチャで数百の押し出し・断面・boolean が例外なく解決できることを確認。

**ローカル確認:** （描画なし。テストのみ。）

---

## M3 — ストーリ（階・レベル・レイヤ） ✅

**目的:** 以降の要素の配置先となるストーリ・ストーリレベル・デザインレイヤを生成する。
**Python: `ifc/story.py` / `vw/story.py`。**

- ✅ `parse/Story`: `…FL` で終わる `IfcBuildingStorey` を対象に、`Elevation`＝ストーリ高さ、
  `IfcColumn`/`IfcSlab` のローカル Z 最大値（≤0）＝横架材天端オフセットを算出。
  階名・suffix・レイヤ名・レベル（`FL`/`横架材天端`、最上階 `軒高`）を組み立て → `StoryCommand`。
  Elevation 昇順（同値は #id 昇順で安定）・列挙順に依存しない決定値。屋根組・登り梁・
  span 柱レベルは各要素の導入時（M5〜M8）に追加（M3 は基本レベルのみ）。
- ✅ `core/Document`: `LevelCommand`/`StoryCommand`＋`Document.stories`。希望レイヤ順の**計算**は
  SDK 非依存の `desiredStoryLayerOrder`（`共通` 先頭・最上階→最下階・床/野地板は背面）として
  core に置き無 SDK テスト。
- ✅ `draw/Story`: `CreateStory`→`SetStoryElevationN` を 1 階ずつ。レベルは
  `CreateLevelTemplateN`＋`AddLevelFromTemplate`＋`GetLayerForStory`＋`SetName`。並べ替えは
  `desiredStoryLayerOrder` の順に `HMoveForward`（`toFront=false` で 1 段ずつ・端で打ち切り）。
- ✅ レイヤのスタック順並べ替え。床レイヤ（FL）・野地板を最背面へ回す枠を用意
  （床＝M5 / 野地板＝M6 で効く）。
- ✅ テスト: `ParseStoryTests`（Z 抽出・オフセット最大値・階数・レベル・センタ判定・
  非 FL 除外・希望レイヤ順・決定性を検証。無 SDK で green）。
- ✅ **ローカル目視確認済み**。ストーリと各レイヤが階数分でき、レベル高さ・レイヤの
  スタック順が希望どおりであることを VW 実機で確認。

**ローカル確認:** ストーリと `n-FL`/`n-横架材天端`/`R-軒高` 等のレイヤが階数分でき、
ナビゲーションのレベル高さが正しく、レイヤのスタック順が希望どおり。

---

## M4 — 構造クラス判定（純ロジック） ✅

**目的:** 柱・横架材の `04構造-02木造-…` クラス割り当てを純ロジックとして移植。
描画は無し（M7/M8 が使う）。**Python: `ifc/structural_class.py`。**

- ✅ `parse/StructuralClass`: 種別トークン→クラスの対応表（`memberTypeOfName` /
  `memberClassFromName`。床小梁・床大梁・甲乙梁→床梁、登り梁 等）、名前で判別できない
  ときの状況推定（`resolveMemberClass`＝階・軒高、`resolveColumnClass`＝最上階・貫通判定）。
  `CLASS_*` 定数を Python 版と一字一句合わせる。**純粋な文字列／整数ロジックのみ**で STEP
  グラフにも SDK にも依存しない。基礎クラス（立上り＝`04構造-01基礎-03立ち上がり`／
  底盤＝`…-02基礎スラブ`）は Python 版でも `ifc/footing.py` 側にあるため M9 基礎で定義する。
- ✅ テスト: `tests/ParseStructuralClassTests.cpp`（Python 版 `test_ifc_structural_class.py`
  の全ケースを 1 対 1 で移植。無 SDK・フィクスチャ不要で green）。

**ローカル確認:** （描画なし。M7/M8 の描画時にクラス分けで確認。）

---

## M5 — 床板（形状先行①） 🟨

**目的:** **建物形状を先に据える第一歩。** 各階 `n-FL` にスラブで床を描き、段差
（スキップフロア）を表現。床の位置は IFC の一次情報で、以降の高さ基準の目安になる。
**Python: `ifc/floor.py` / `vw/floor.py`。**

- ✅ `parse/Floor`: `床版`(`IfcSlab`) → `FloorCommand`（**床仕上げ上端**の絶対 Z ＝ FL ± 段差、
  FL レベルバインド＋段差 offset、スラブ構成＝床仕上げ〈FL−横架材天端−24〉＋床下地〈24〉）。
  平面外形・最下端 Z の抽出は M2 の `WorldSolid` を再利用。最上階（屋根）は FL レイヤを
  持たないため対象外。座標は通り芯と同じグリッド中心オフセット。
- ✅ `parse/IfcGeometry` 拡張: 要素の形状表現から押し出しを取り出す `firstExtrudedSolid` /
  `resolveElementWorldSolid`、平面外形 `footprint`（鉛直押し出し＝プロファイル、水平押し出し＝
  掃引矩形）、Z 範囲 `zTopAndThickness`（Python 版 `ifc/footing.py` の低レベルヘルパー相当。
  M9 基礎でもそのまま使う）。`parse/Grid` にセンタリング中心を返す `resolveGridCenter`、
  `parse/Story` に階の収容要素を返す `collectStoryElements` を追加。
- ✅ `parse/Step` 拡張: **ISO 10303-21 の拡張文字エスケープ（`\X2\…\X0\` / `\X\HH` /
  `\S\c` / `\P?\`）を UTF-8 へデコード**。ホームズ君 IFC の日本語 `Name`（`床版` 等）は
  UTF-16 エスケープで出力されるため、これが無いと名前による要素判別が一切通らない
  （ifcopenshell が内部で行っているのと同じ扱い。M7 以降の `木梁:…` 等の判定にも必須）。
- ✅ `core/Document`: `FloorCommand`（`components` ＝ スラブ構成層）＋ `SlabComponentCommand`
  ＋ `StoryBoundCommand`。`validateDocument` に床の検証（外形 3 点以上・構成層・総厚 > 0）。
- ✅ `draw/Floor`: 外形の閉じたポリゴン→`CreateSlab`→クラス・by-class 属性→構成層
  （床仕上げ／床下地の厚み・名前）→`SetSlabHeight`（**天端**＝床仕上げ上端の絶対 Z）→
  `SetObjectStoryBound`（`FL` レベルへ段差 offset でバインド）→`ResetObject`。
  配置先 `n-FL` レイヤが無い命令はスキップ（レイヤは story 命令が作る）。スラブを作れない
  場合は外形ポリゴンへフォールバック。
- ✅ **描画オブジェクトはスラブ**（Python 版の床ツールから意図的に変更）。床ツールは実体が
  押し出しの派生でオブジェクト構造が押し出しとほぼ変わらないのに対し、スラブは BIM
  オブジェクトとして機能（コンポーネント・スタイル・データ連携）が強化されており発展性が
  高い。ISDK にも Floor の生成 API は無く Slab には一式が揃っているため SDK の作法にも
  素直に乗る。**2D 表現は床ツールと異なる**ためローカル確認の対象にする。描画手順は
  Python 版 `vw/footing.py` の `draw_slab`（底盤＝M9）と共通の作法。
- ✅ テスト: `ParseFloorTests`（`test_ifc_floor.py` の全ケースを移植: 枚数・レイヤ振り分け・
  厚み 24 固定・クラス・`elevation = 横架材天端 + offset` の不変条件・スキップフロアの段差
  −832mm・横架材天端より上の床・センタリング済み外形・決定性。合成モデルでの抽出条件も追加）。
  `StepTests` に文字エスケープのデコード、`CoreDocumentTests` に床の検証ケースを追加。
- ⬜ **ローカル目視確認（ユーザー）**。下記チェックリスト。

**床レイヤの最背面化について:** M3 で判明したとおり VW 2026 ISDK にはデザインレイヤの
重ね順を変える呼び出しが無く、目的（伏図で床が柱・梁を覆わない）は per-viewport の
`SetViewportLayerStackingOverride` で満たすため **M13 に委ねる**。希望順の計算
（床＝背面）は `core::desiredStoryLayerOrder` に用意済み。

**ローカル確認:** 各階の `n-FL` レイヤに床（スラブ）が描かれる。**床仕上げ上端が FL**（床レベル
指定のある床は FL ± 差分）に一致し、スラブ構成が上から `床仕上げ`（FL−横架材天端−24）＋
`床下地`（24）になっている（＝スラブ下端が横架材天端に一致）。2D 表現が床ツールと異なる点も
併せて確認する。スキップフロアの段差が高さの差として
表れる（`スキップフロア_サンプル` の 2FL は通常床と 832mm 下がった床が混在）。床を選択して
OIP の高さ基準が `横架材天端` レベル＋offset になっており、編集しても高さがずれない。

---

## M6 — 屋根面・屋根組（垂木・野地板）（形状先行②） ⬜

**目的:** **建物形状の要＝屋根面を確定する。** 屋根版（`IfcSlab` の屋根面）から垂木・
野地板を導出し、屋根面の勾配・外形を据える。これが M7 の**登り梁を屋根面へスナップ補正**
する際の基準になる（形状 → 支持部材の依存を作る）。母屋・棟木・登り梁（＝横架材）は
M7 で扱う。**Python: `ifc/rafter.py` / `ifc/roof.py`（＋屋根面ヘルパー `_roof_plane`）。**

- ⬜ `parse/IfcGeometry` 拡張: 屋根版から**屋根面**（平面外形頂点列＋単位法線）を取り出す
  `roofPlane` ヘルパー（Python `rafter._roof_plane`）。M7 の登り梁スナップも同じ関数を使う。
- ⬜ `parse/Rafter`: 屋根版から垂木を導出（`FramingMember` type=rafter、`n-垂木`、既定 45×45・
  間隔 455mm）。勾配方向へ掃引・走査線クリップ、支持点＝屋根面と横架材天端 Z の交点、
  軒の出・差し込み・ラベル → `RafterCommand`。**差し込み桁幅は横架材未導入なら既定桁幅に
  フォールバック**（M7 導入後に精緻化）。
- ⬜ `parse/Roof`: 屋根版 1 面＝野地板 1 枚（`BeginRoof`、`n-野地板`、厚み 12mm 固定）→
  `RoofCommand`（軒＝屋根軸・upslope・勾配・軒の絶対 Z）。
- ⬜ `parse/Story` 拡張: 屋根版を持つ階に `垂木`/`野地板` レベル（`n-垂木`/`n-野地板` レイヤ）を
  追加（`story_has_roof`）。野地板レイヤは最背面枠へ。
- ⬜ `core/Document`: `RafterCommand`/`RoofCommand`。
- ⬜ `draw/Rafter`: `CreateCustomObjectN('FramingMember', …)`＋向き／`pitch`／各フィールド。
  `draw/Roof`: `BeginRoof`＋レイヤ相対 Z＋高さ自己補正（`GetRoofFaceCoords`）。
- ⬜ テスト: `ParseRafterTests` / `ParseRoofTests`（`test_ifc_rafter.py` / `test_ifc_roof.py`）。

**ローカル確認:** 屋根面に沿って垂木が 455mm 間隔で並び、野地板が屋根版 1 面ごとに 1 枚
描かれる。勾配・高さが屋根版どおり。この屋根面が M7 登り梁のスナップ基準になる。

---

## M7 — 横架材（土台・梁・桁・母屋・棟木・登り梁）★形状へ合わせる支持部材 ⬜

**目的:** 中核の支持部材。土台・梁・桁をストーリレベルにバインドして描き、**母屋・棟木・
登り梁を専用レイヤへ分離**する。**登り梁は M6 で確定した屋根面へスナップ補正**して勾配・
高さを屋根に合わせ、端部を受け材／柱の面まで詰める（＝形状へ支持部材を合わせる）。
**Python: `ifc/member.py` / `ifc/noboribari.py` / `vw/member.py`。**

- ⬜ `parse/Member`: 配置・断面・材種 → `MemberCommand`。天端中央線への基準点補正
  （断面中心＋背/2）、高さバインド（`start_bound`/`end_bound`）、傾斜梁
  （`elevation≠end_elevation`）、食い込み調整（`resolve_member_interferences`）。
- ⬜ 母屋・棟木の `n-母屋` 分離（`母屋` レベル・`story_has_moya`）。登り梁の**任意断面抽出**
  （`_sloped_member_geometry`＝平行四辺形 4 頂点から中心軸・幅・せい・傾斜を導出。火打＝
  鉛直軸・筋かい＝6 頂点は除外）と `n-登り梁` 分離（`登り梁` レベル・`story_has_noboribari`）。
- ⬜ `parse/Noboribari`: **登り梁の位置補正**（`correct_noboribari`）。①端部の食い込み解消
  （`_trim_noboribari_ends`）、②天端中央線を **M6 の屋根面へスナップ**（`_snap_noboribari_to_roof`。
  M6 の `roofPlane` を参照）。端部詰めは受け材（本 M の横架材）・柱（M8）を参照するため、
  **柱導入後（M8）に最終化**する（本 M では横架材ベースで暫定補正）。
- ⬜ `parse/Story` 拡張: `母屋`/`登り梁` レベルを追加（M6 の `垂木`/`野地板` と同じ枠組み）。
- ⬜ `core/Document`: `MemberCommand` ＋ `StoryBoundCommand`。
- ⬜ `draw/Member`: `CreateCustomObjectPath('StructuralMember', …)`＋`Move3D`＋
  `SetObjectStoryBound`（上下端）＋`SetPluginStyle('木質構造材_横架材')`。全配置後に
  `UpdateStyledObjects` を 1 回。**パスに Z 成分を持たせない**（傾斜は bound の offset 差）。
- ⬜ テスト: `ParseMemberTests` / `ParseNoboribariTests`（`test_ifc_member.py` /
  `test_ifc_noboribari.py`）。

**ローカル確認:** 各階に土台・梁・桁が正しい高さ・断面・クラスで並ぶ。母屋・棟木・登り梁が
専用レイヤに分離される。**登り梁の天端が M6 の屋根面（垂木下面）に一致**し、端部が受け材へ
食い込まない。段差梁・傾斜梁の高さが二重加算されない。

---

## M8 — 柱（管柱・通し柱・小屋束）★形状へ合わせる支持部材 ⬜

**目的:** 柱を構造材ツールで鉛直材として描き、span レイヤに分けて配置する。登り梁の
端部詰め（M7）を柱面まで最終化する。**Python: `ifc/column.py` / `vw/column.py`。**

- ⬜ `parse/Column`: 断面・柱高さ・種別・柱頭/柱脚金物・**span（from/to レベル）判定**
  （`resolve_column_to_level`。上階梁下端＝M7 の members を参照）→ `ColumnCommand`。
  `{from}to{to}-柱` レイヤ、`structural_use`（柱=4／小屋束=5）、金物を含む `member_id`。
- ⬜ `parse/Story` 拡張: span 柱レベル（`column_layers_by_story`）をストーリに追加。
- ⬜ `parse/Noboribari` 最終化: 登り梁端部の食い込み解消を柱面（本 M の columns）まで適用。
- ⬜ `draw/Column`: 構造材ツールで鉛直パス＋`Move3D`＋上下端バインド。小屋束は上端
  offset を下端と同値にして二重加算回避。`SetPluginStyle('木質構造材_柱・束')`＋
  全配置後 `UpdateStyledObjects`。
- ⬜ テスト: `test_ifc_column.py` / `test_ifc_column_span.py` を写す。

**ローカル確認:** 柱・小屋束が span レイヤ（`1to2-柱` 等）に分かれ、通し柱が複数階を
跨ぎ、下屋小屋束が上階に写り込まない。上端高さが正しく、登り梁端部が柱に食い込まない。

---

## M9 — 基礎（立上り＝壁・底盤＝スラブ）＋基礎ストーリ ⬜

**目的:** 基礎の主要 2 オブジェクトと基礎ストーリを描く。上部形状（床・屋根・部材）とは
独立なので、M5〜M8 と並行着手も可。地中梁・人通口・壁結合・配筋は M10 に分割。
**Python: `ifc/footing.py`（一部）/ `vw/footing.py`。**

- ⬜ `parse/Footing`: 基礎ストーリ命令（`基礎`/suffix `F`/GL=0、レベル 4 種）。
  立上り（`基礎梁…`）→ `WallCommand`（マージ `merge_wall_commands`・自由端半壁厚延長）。
  底盤（`底盤`）→ `SlabCommand`（マージ `merge_slab_commands`・外面合わせ
  `align_slabs_to_wall_faces`・`thickness`）。基礎クラス（立上り／底盤）を M4 の枠へ定義。
- ⬜ `draw/Footing`: 壁（`DoubLines`→`Wall`→`SetWallOverallHeights`→`SetWallStyle`）、
  スラブ（`CreateSlab`→スラブスタイル→`SetSlabHeight`→`SetObjectStoryBound`）。
  スラブスタイルは `BuildResourceList` で列挙・厚み別に複製。
- ⬜ テスト: `test_ifc_footing.py` の該当部を写す。

**ローカル確認:** 基礎ストーリができ、立上りが壁・底盤がスラブで正しい高さ・厚み・
クラスで描かれ、連続する立上り／底盤が 1 本／1 枚に統合される。

---

## M10 — 基礎の高度化（地中梁・人通口・壁結合・配筋） ⬜

**目的:** 基礎の残り機能。M9 の上に積む。**Python: `ifc/footing.py` / `ifc/reinforcement.py`。**

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

## M11 — シンボル置換系（アンカーボルト・床束・火打・仕口） ⬜

**目的:** ハイブリッドシンボルへの置換 4 種。互いに独立で小さく、まとめて 1 マイルストーン。
仕口は横架材（M7）・柱（M8）の端部判定に依存する。
**Python: `ifc/{anchor_bolt,floor_post,fire_brace,joint}.py` / 対応 `vw/`。**

- ⬜ アンカーボルト（`IfcMechanicalFastener`→`アンカーボルト_M12/M16`、`F-アンカーボルト`）。
- ⬜ 床束（大引の下に 910mm 間隔で決め打ち、継手統合・支持材芯、`床束`、`F-床束`）。
- ⬜ 火打（`火打:…`→`鋼製火打`、端面交点＋回転角、横架材レイヤ）。
- ⬜ 仕口（受ける材／柱のある横架材端部→`仕口`、member/column 命令から判定、横架材レイヤ）。
- ⬜ `draw/`: 各 `Symbol(名, (x,y), angle)`。テスト: 対応する `test_ifc_*.py`。

**ローカル確認:** 各シンボルが正しい位置・角度・レイヤに置かれる。

---

## M12 — 断面記号・伏図記号 ⬜

**目的:** span 柱レイヤごとに `柱束伏図記号` PIO を配置（断面記号＝×/／、伏図記号＝シンボル）。
**Python: `ifc/column_mark.py` / `vw/column_mark.py`。**

- ⬜ `parse/ColumnMark`: span ごとに断面記号（`断面`）＋伏図記号（`平面`、`{to}-柱伏図記号`
  レイヤ、種別でシンボル選択）→ `ColumnMarkCommand`。
- ⬜ `draw/ColumnMark`: `柱束伏図記号` PIO、伏図記号レイヤの生成・`共通` 直下への並べ替え。
- ⬜ テスト: `test_ifc_column_mark.py`。

**ローカル確認:** 各 span レイヤに断面記号が載り、伏図記号レイヤに平面記号が描かれる。

---

## M13 — シート・伏図・データタグ・凡例 ⬜

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

## M14 — 断面ビューポート（軸組図） ⬜

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

**形状（床・屋根面）を先に確定し、支持部材（横架材・柱）をそれに合わせる。**

```
M0 基盤 ─┬─ M1 通り芯（縦切り実証）
         └─ M2 幾何土台 ── M3 ストーリ ── M4 構造クラス
                                │
              ┌──── 形状先行 ────┴──── 形状先行 ────┐
           M5 床板                        M6 屋根面（垂木・野地板）
              │                                    │  屋根面を確定
              └──────────────┬─────────────────────┘
                             │  形状へ支持部材を合わせる
                   M7 横架材（母屋・棟木・登り梁＝屋根面へスナップ補正）
                             │
                   M8 柱（span・小屋束／登り梁端部を柱面へ最終化）

  基礎（上部形状と独立・並行着手可）
  M9 基礎(壁/スラブ) ── M10 基礎(高度)

  記号・伏図系（部材・柱の確定後）
  M11 シンボル ── M12 断面記号/伏図記号 ── M13 伏図/タグ/凡例 ── M14 軸組図
```

依存の要点:
- **M6 屋根面 → M7 登り梁**: 登り梁は M6 で確定した屋根面へスナップ補正する（形状 → 支持部材）。
- **M7 横架材 → M8 柱**: 柱の span（to レベル）判定は上階梁の下端を参照する。
- **M7/M8 → M11 仕口・M12 記号・M13 伏図**: 端部判定・span レイヤ・タグ関連付けは部材／柱に依存。
- **M9/M10 基礎**は上部形状と独立で、M5〜M8 と並行に着手してよい。

各要素の詳細仕様は姉妹リポジトリ `vectorworks-plugin-script-import-ifc-homeskz` の
`CLAUDE.md` および該当ソース・テストを一次資料とする。
