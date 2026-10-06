# 描画側の作法（M31）

（伏図と軸組図へ寸法を自動で入れる（M31）は 4 ページに分けてある: [押さえ方の決め事](README.md) → **描画側の作法** → [実機で分かったこと](live-findings.md) → [レベル基準線の描き方の調整](level-lines.md)）

**描画側（`draw/Dimension`）は SDK リファレンスの調査結果に従う**
（[#129](https://github.com/min-nano/vectorworks-developer-sdk-reference/issues/129) →
[Findings「Dimensions」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Dimensions.md)・
[#130](https://github.com/min-nano/vectorworks-developer-sdk-reference/issues/130) →
[Findings「Level Objects」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Level%20Objects.md)）:

- **寸法は直線寸法を 1 本ずつ作り、`CreateChainDimension` で連続寸法へ繋ぐ。** 寸法規格は
  **繋ぐ前に**名前（`ovDimStandardName`）で当てる（連続寸法そのものには効かない）。図面に
  無い名前は書けずに弾かれるので、件数を診断へ出す。
- **伏図の寸法も、そのシートレイヤをアクティブにして作る**（軸組図と同じ状態。
  `draw/Sheet.cpp`）。`CreateChainDimension` は繋ぐ元の直線寸法を undo 記録つきで消すので、
  取り消すと**作ったときのアクティブレイヤへ直線寸法が復活する**。デザインレイヤ（テンプレートに
  最初から在る「共通」）がアクティブだと、取り込み前から在ったレイヤに残骸が残った（実機の
  指摘。取り込み直後の図には出ない。復活した寸法の文字スタイルは〈なし〉＝当てる前の姿だった）。
  取り込みが作ったシートレイヤの上で作れば、取り消しでレイヤごと消える。**先に試した
  「繋ぐ前に `AddAfterSwapObject`（`RecordCreatedObject`）で申告する」は効かなかった**
  （round 1。通り芯のパスでは効いた作法だが、寸法では残骸が消えなかった）。
- **寸法線の位置は `startOffset` で渡す。** 符号は図面の座標軸で決まる（+ が上／右）ので、
  命令の side とそのまま一致する。
- **GL・FL・軒高はレベル基準線（`Elevation Benchmark2`）で置く。** 高さは `Axis`＝
  `YAxis2DMode` で注釈の Y から読ませ、名前はマーカーレイアウトのストーリレベル名の
  テキストを**固定の文字へ作り直して渡し直す**。ストーリレベルへの関連付けは使わない
  （注釈では高さを読む設定と両立しない）。**新しいテキストの位置は古いテキストの揃え方の
  辺に合わせる**（左揃えなら左端）——ここは Findings に無い、このプラグインの判断で、
  実機で確かめる。（後に変わった: round 3 の後、`Axis` は触らず Z 軸のままストーリレベルへ
  結ぶ 3 つ組に切り替え、マーカーレイアウトは全部消して名前と ▽ を作り直すようにした。→
  [実機で分かったこと](live-findings.md)・[レベル基準線の描き方の調整](level-lines.md)）
- **寸法の帯を用紙から引いて縮尺を選ぶ**（`core::dimensionBand` → `planLayout` /
  `sectionLayout` の `band`）。帯＝最も外の段の寸法線までの距離＋文字の見込み 4mm。
  軸組図のマスは帯を含めた大きさで並べる。（軸組図は M35 で、帯を辺ごとに数える
  `core::sectionBands` に変わった。→ [M35](../m35-section-layout.md)）
- **設定ダイアログの寸法規格の行は、形に依らず名前のプルダウン**（寸法規格は資源では
  ないのでサムネイルにできない）。候補は index を総当たりして名前が引けたもの（組み込み
  1〜9 → カスタム 0〜−8）。
- 寸法・データタグ・レベル基準線は同じクラス「寸法」（`draw/DrawUtil` の
  `kDimensionClass`）へ置き、置いた後に全クラスを表示へ戻して描き直す（データタグと同じ）。
