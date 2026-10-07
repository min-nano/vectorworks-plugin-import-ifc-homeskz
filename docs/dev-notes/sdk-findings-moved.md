# Vectorworks SDK の実測知見（SDK リファレンスへ移動）

かつて開発メモ（`docs/DEV-NOTES.md`）にあった SDK の実測知見（実機でしか判明しなかった落とし穴・SDK に
無い／機能しない API・SDK 側の打ち切った調査）は、プロジェクトをまたいで参照できるよう
**[SDK リファレンスリポジトリ](https://github.com/min-nano/vectorworks-developer-sdk-reference)の
`Findings/` へ移動した**。**SDK の挙動は SDK リファレンスだけを正とし、開発メモには書かない**
（新しい知見も SDK リファレンスへ追加する。着手前の「この API は SDK にあるか」の確かめ方
——`sdk-grep`——も SDK リファレンスの CLAUDE.md にある）。

| 旧トピック | 移動先（`Findings/`） |
| --- | --- |
| VectorScript → SDK の対応（無いものが多い） | [VectorScript to SDK Mapping](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/VectorScript%20to%20SDK%20Mapping.md) |
| プラグインオブジェクト（PIO）・構造材ツール・自作 PIO の 3 点 | [Parametric Objects](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Parametric%20Objects.md) |
| シンボル | [Symbols](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Symbols.md) |
| 壁 | [Walls](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Walls.md) |
| スラブ・押し出し（`ModifySlab` の打ち切りを含む） | [Slabs and Extrudes](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Slabs%20and%20Extrudes.md) |
| 屋根面 | [Roof Faces](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Roof%20Faces.md) |
| レイヤ・ストーリ・重ね順（重ね順上書きの打ち切りを含む） | [Layers and Stories](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layers%20and%20Stories.md) |
| ビューポート（伏図・断面。範囲「無限」の打ち切りを含む） | [Viewports](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Viewports.md) |
| 用紙（シートレイヤ）と割り付けの SDK 作法 | [Sheet Layers and Page Layout](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Sheet%20Layers%20and%20Page%20Layout.md) |
| データタグ（相対配置の打ち切りを含む） | [Data Tags](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Data%20Tags.md) |
| グラフィック凡例（ソース定義・フィルタ・縮率の打ち切りを含む） | [Graphic Legends](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Graphic%20Legends.md) |
| タグ付きデータの型・読み書きの癖・`'DMDT'` の構造 | [Tagged Data](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Tagged%20Data.md) |
| Undo（取り消し） | [Undo](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Undo.md) |
| 進捗・診断・例外の SDK 作法 | [Progress and Diagnostics](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Progress%20and%20Diagnostics.md) |
| 結果ダイアログ（レイアウトダイアログの作法） | [Layout Dialogs](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md) |
| 調査の作法（読み戻す・測る・正解と差分） | [Investigation Techniques](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Investigation%20Techniques.md) |

このプラグイン固有の方針（何をどう描画するか・診断の出し方・割り付けの決め事）は開発メモの
「[設計の考え方](../DEV-NOTES.md#設計の考え方)」、入力データの癖は「[IFC / STEP 側の癖](ifc-quirks.md)」
に残っている。
