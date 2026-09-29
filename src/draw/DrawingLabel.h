//
//	draw/DrawingLabel.h
//
//	Phase 2（VW 描画）の図面ラベルモジュール。軸組図（断面ビューポート）1 枚ごとに、その
//	**真下の中央へ図面ラベル（PIO "Drawing Label2"）を置いて図面タイトルを出す**。
//	図番・縮尺は出さない（要件）。
//
//	SDK の挙動の根拠はすべて SDK リファレンスの
//	[Findings「Drawing Labels」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Drawing%20Labels.md)
//	（実機確認済み）。要点:
//	  * **ラベルレイアウトはラベル自身のプロファイルグループ**。既定の中身は
//	    「タイトルのテキスト・下線・縮尺のテキスト・図番の丸・図番のテキスト」の順。
//	  * 動的な文字列の式（`#Drawing Label2#.#Title#`）は**テキストが抱える隠れた状態**で、
//	    データタグの IDataTagTextLinkSupport では読み書きできない。新しく作ったテキストに
//	    式は持たせられない——**既定のテキストを DuplicateObject で複製**するのが唯一の組み方。
//	    そこで「**タイトルのテキストと下線だけを複製して新しい群へ入れ、渡し直す**」
//	    （図番と縮尺はそれで落ちる。パラメータに表示を切る欄は無い）。
//	  * **ビューポートの注釈へ入れる**（AddViewportAnnotationObject）とビューポートにリンク
//	    され、後から図面タイトルを変えるとラベルも追随する。**ただし置いた直後の Title は
//	    当てにならない**（隣のビューポートのタイトルが入っていた実測がある）ので、注釈へ
//	    入れた後に**命令の図面タイトルを Title へ自分で書く**。
//
//	★**スタイルは作らない・当てない**（CLAUDE.md 開発の基本方針 4。データタグと同じ扱い。
//	draw/Tag.h の ★）。ただし**ツールのスタイルは作った直後に勝手に当たる**うえ、**既定の
//	レイアウトはそのスタイルが配る**（Findings #175）ので、「スタイル付きで作る → レイアウトを
//	組み直す → SetPluginObjectStyle(h, 0) で外す」の順で置く（外してもレイアウトは残る）。
//	文字スタイル「図面ラベル(10pt)」は**図面にあれば当て、無ければ紙の 10pt を直接与える**
//	（作らない。データタグの「寸法(6pt)」と同じ流儀）。
//
//	【文字の大きさは紙の上の mm】レイアウトのテキストへ与える大きさは**紙の上の mm**で、
//	容れ物（注釈）の縮尺は VW が掛ける——寸法の ovDimFontSize（縮尺を自分で掛ける）とは
//	逆。文字スタイルは当てたときのアクティブレイヤの縮尺が焼き付くので、当てる間だけ 1:1 の
//	シートレイヤをアクティブにする（Findings「レイアウトの文字の大きさ」）。
//
//	【位置は測って合わせる】ラベルの実寸はレイアウトの中身（タイトルの長さ・文字の大きさ）が
//	決めるので、置いてから GetObjectBounds で測り、**上端中央**が目標へ来るように動かす
//	（データタグと同じ「置いた後に測って動かす」。Findings「Data Tags」）。目標は注釈空間の
//	**建物の最下点の、図の左右の中央**（core::sectionLabelAnchor）から、図の下に出る寸法の
//	帯のさらに外まで下げた点（core::sectionLabelDrop。軸組図は柱の位置の寸法を建物の真下に
//	持つので、直下に置くと寸法と重なる）。注釈空間の座標は断面寸法タグ・寸法と同じ投影
//	（parse/Tag.h「断面の注釈空間」）で、注釈空間の長さは**モデル mm**（用紙 mm × 縮尺）で
//	測られる。
//
//	【SDK 型を公開するヘッダ】ビューポートのハンドルを引数に取るため、draw/Tag.h と同じく
//	**SDK 型を公開する共通ヘッダ**で、要素ごとの draw/*.h から include してはならない
//	（draw/DrawUtil.h 冒頭の約束）。呼び出し元は draw/Section.cpp だけ。
//
//	実描画（ラベルの見え方・位置・ビューポートへのリンク）はローカルの VectorWorks で目視
//	確認する（docs/DEV-NOTES.md「軸組図の図面ラベル（M32）」）。
//

#pragma once

#include "draw/DrawUtil.h"

#include "core/Geometry.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// 図面ラベルの集計。**実描画はローカルの VW でしか確認できない**ので、ラベルが出ない・
	// 図番が残る・位置がずれるときに原因を切り分けられるように件数で持ち帰る（draw/Tag の
	// TagCounts と同じ流儀）。
	struct DrawingLabelCounts
	{
		std::size_t drawn = 0;	// 注釈に置けたラベル
		std::size_t failed = 0; // PIO を作れなかった／注釈に入れられなかった
		// レイアウトを組めなかった（既定のレイアウトのまま＝図番と縮尺が出る）
		std::size_t layoutFailed = 0;
		// 既定のレイアウトが空で置かなかった（ツールにスタイルが無く、複製する元が無い）
		std::size_t noDefaultLayout = 0;
		std::size_t styleLeft = 0; // スタイルを外せなかった（スタイルの編集と見た目が食い違う）
		std::size_t unmeasured = 0; // 実位置を測れず動かせなかったラベル
		bool textStyleMissing = false; // 文字スタイル（"図面ラベル(10pt)"）が図面に無かった
	};

	// 図面ラベル PIO の定義を**設定ダイアログを出さない**で用意する。ラベルを置くフェーズ
	// （軸組図）の先頭で 1 回呼ぶ（理由は DrawUtil の PrepareCustomObjectDefinition）。
	void prepareDrawingLabelPlugin();

	// ビューポート 1 枚の真下の中央へ図面ラベルを置く。置けたら true で、内訳を counts へ積む
	// （複数のビューポートぶんを 1 つの counts へ積んでよい）。
	//
	//   sheetLayer … ビューポートが載っているシートレイヤ（1:1）。文字スタイルを当てる間だけ
	//                アクティブにする（当てたときのアクティブレイヤの縮尺が焼き付くため）
	//   title  … 表示する図面タイトル（ビューポートに与えたものと同じ文字列）
	//   anchor … 注釈空間の、建物の最下点の左右の中央（core::sectionLabelAnchor）
	//   drop   … anchor からラベルの上端までの距離（用紙 mm。下に出る寸法の帯を含む。
	//            core::sectionLabelDrop）
	//
	// ★**ビューポートを用紙の上で動かす前に呼ぶ**（データタグと同じ。draw/DrawUtil の
	// MoveViewportBy）——注釈へ置いた実位置の実測は、ビューポートが用紙のどこに在るかに
	// 影響される。
	bool drawSectionLabel(MCObjectHandle viewport, MCObjectHandle sheetLayer,
						  const std::string& title, const core::Vec2& anchor, double drop,
						  DrawingLabelCounts& counts);

	// 集計を人が読める 1 行の診断にする（異常が無ければ空文字）。label は図の種別
	// （"軸組図"）。
	std::string drawingLabelDiagnostics(const std::string& label, const DrawingLabelCounts& counts);
} // namespace HomeskzIfcImport::draw
