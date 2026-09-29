//
//	draw/Dimension.h
//
//	Phase 2（VW 描画）の寸法モジュール（docs/DEV-NOTES.md M31）。解析側（parse/Dimension）が
//	組み立てた**連続寸法の列**（core::DimensionChainCommand）と、軸組図の**レベル記号**
//	（core::LevelMarkCommand）を、伏図・軸組図のビューポートの**注釈**として置く。
//
//	【SDK 型を公開するヘッダ】ビューポートのハンドルを引数に取るので、draw/Tag.h と同じく
//	**SDK 型を公開する共通ヘッダ**。呼び出し元は draw/Sheet.cpp と draw/Section.cpp の 2 つ
//	だけで、要素ごとの draw/*.h から include してはならない（DrawUtil.h 冒頭の約束）。
//
//	【寸法の作法】（SDK リファレンス Findings「Dimensions」の実測に従う）
//	  CreateLinearDimension(p1, p2, startOffset, 0, (0,0), 0) で直線寸法を 1 本ずつ作る →
//	  寸法規格を**名前で**当てる（ovDimStandardName。図面に無い名前は false で弾かれる）→
//	  寸法クラスへ置く → 隣り合う 2 本を CreateChainDimension で**連続寸法へ繋いでいく** →
//	  AddViewportAnnotationObject で注釈へ移す
//	  * 測点は注釈空間の座標そのまま（伏図＝平面座標、軸組図＝(断面線の終点からの距離, Z)）。
//	    注釈へ移しても座標は変わらない（実測）ので、タグのように測って動かす必要は無い。
//	  * **startOffset の符号は図面の座標軸で決まる**（水平な寸法は + が上、垂直な寸法は + が
//	    右。測る向きには依らない）。命令の side（直交軸の正負）とそのまま一致するので、
//	    寸法線の位置（core::dimensionLineCoord）から根元を引いた値をそのまま渡す。
//	  * **規格は繋ぐ前の直線寸法へ当てる**——連続寸法そのものには寸法のオブジェクト変数が
//	    効かない。
//	  * 作った寸法は**その場でアクティブレイヤに入る**ので、注釈へ移せなかったものは消す
//	    （シートレイヤに寸法だけが浮かないように）。
//	  * 寸法線までの距離は**用紙 mm × 縮尺**（core::dimensionLineCoord）。注釈空間の座標は
//	    モデルの mm なので縮尺を掛ける。
//	  * **文字スタイルを SetTextStyleRef で明示する**（寸法規格が持つ文字スタイル。
//	    Findings「Dimensions」#157）→ 文字の大きさ（ovDimFontSize）を「紙の pt × 25.4/72 ×
//	    ビューポートの縮尺」で書く → **ResetObject で引き直す**（#161。書いただけでは絵に
//	    出ない）→ 繋ぐ。文字の大きさは作るときのアクティブレイヤの縮尺で焼き付くので、
//	    1:1 のシートレイヤがアクティブなうちに作る軸組図で要る。
//
//	【レベル基準線の作法】（Findings「Level Objects」「Viewports」#141 / #147）
//	  CreateCustomObject("Elevation Benchmark2") → 注釈へ移す → SetPointObjectPos で注釈の
//	  座標を明示 → **ストーリレベルへ結ぶ 3 つ組**（__StoryName ＋ __LevelTypeName ＋
//	  Datum＝StoryLevel。Axis は既定の Z のまま）→ ResetObject → **マーカーレイアウトを
//	  組み直して渡し直す**（▽＋名前。下記）→ 図の全記号の幅が揃ったら起点へ置き直し、
//	  **引出線を切って基準線をレイアウトへ足す** → ResetObject →
//	  （ビューポートの更新を全部済ませた後で）**断面の向き（1055）をビュー行列（1050）へ
//	  写して**個体を ResetObject（finishLevelMarks）
//	  * 高さはストーリレベルの絶対 Z から来る——記号をドラッグしても数値は動かない
//	    （round 3 のご指摘。Axis＝YAxis2DMode で注釈の Y を読ませていた版は動いた）。
//	  * SDK の CreateSectionViewport が作るビューポートはビュー行列が単位行列のままで、
//	    写さないと高さが 0 と描かれる。UpdateViewport が写したものを戻すので、写すのは最後。
//	  * 名前はパラメータでは出ない。レイアウトの既定の中身（高さ "#Elev#"・ストーリレベル名
//	    "#STLT#…" の 2 つのテキストと記号のポリライン）を消し、CreateTextBlock /
//	    VWPolygon2DObj（閉じたポリライン。塗りが効く）で作った**名前と▽**を入れて
//	    SetCustomObjectProfileGroup で
//	    渡し直す（中身を入れ替えるだけでは絵に出ない。実測）。**高さの数値は描かない**
//	    （ご要望）。差し替えても結び付き（Datum）が残っているかは読み戻して確かめる。
//	  * レイアウトの長さは**用紙 mm**（Findings「Drawing Labels」の「レイアウトの文字の
//	    大きさ」を同じ作りのマーカーレイアウトにも当てはめた。round 1 の実機で縦の大きさが
//	    合った）。名前の大きさは寸法の文字と同じ紙の pt、▽ はその 0.8 倍の高さの正三角形
//	    （core/Layout.h「軸組図のレベル記号の形と位置」）。
//	  * **PIO 自身は挿入点から左へ水平引出線を引く**（既定では用紙 36mm。実機で分かった。
//	    パスは持たず、カスタム制御点を動かしても変わらない）。`UseHorizontalLeader` を切り、
//	    **基準線はレイアウトの中に起点から右へ**、図の右端を少し越えるまで自分で引く
//	    （draw/Dimension.cpp の DrawLevelLine）。
//	  * **起点は左の寸法列より外**で、**同じ図の記号は ▽ の左端を揃える**（ご要望）。名前の
//	    幅は描くまで分からないので、図の全記号のレイアウトを組んで名前を測り、いちばん広い
//	    記号で起点を決め（core::levelMarkStartX）、全部をそこへ置き直す。
//
//	【注釈へ足した後はクラスを戻して描き直す】注釈へ後から足した図形のクラスはビューポートで
//	非表示のまま（Findings「Viewports」）なので、置き終えたら全クラスを表示へ戻して更新する
//	（データタグと同じ後処理。draw/Tag.h の落とし穴 2）。
//
//	実描画（寸法の位置・規格の当たり方・レベル基準線の名前の位置）はローカルの VectorWorks で
//	目視確認する（docs/DEV-NOTES.md「実機確認の作法」）。
//

#pragma once

#include "draw/DrawUtil.h"
#include "draw/Verify.h"

#include "core/Document.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 寸法・レベル記号の描画の集計。**実描画はローカルの VW でしか確認できない**ので、
	// 出なかったときに原因（規格が無い・繋げない・注釈へ入らない）を件数で持ち帰る。
	struct DimensionCounts
	{
		std::size_t chains = 0;		// 描けた列（1 本でも注釈へ置けた列）
		std::size_t dimensions = 0; // 注釈へ置けた直線寸法
		std::size_t failed = 0;		// 作れなかった・注釈へ入らなかった直線寸法
		std::size_t standardRejected = 0; // 寸法規格を当てられなかった直線寸法（図面に無い名前）
		std::size_t unjoined = 0;		  // 連続寸法へ繋げなかった継ぎ目
		std::size_t textStyleMissing = 0; // 文字スタイルを当てられなかった直線寸法
		std::size_t textSizeUnread = 0; // 文字の大きさを縮尺に合わせられなかった直線寸法
		std::size_t levels = 0;		  // 注釈へ置けたレベル記号
		std::size_t levelsFailed = 0; // 作れなかった・注釈へ入らなかったレベル記号
		std::size_t levelLayoutFailed = 0; // マーカーレイアウトを組み直せなかったレベル記号
		std::size_t levelPathFailed = 0; // 基準線（パス）を差し替えられなかったレベル記号
		std::size_t levelBindFailed = 0; // ストーリレベルへ結べなかったレベル記号
		std::size_t viewMatrixFailed = 0; // 断面の向きをビュー行列へ写せなかったビューポート
		std::size_t levelHeightUnread = 0; // 描いた高さを読めなかったレベル記号
		// 描いた高さが命令の高さと合わなかったレベル記号と、その 1 個目の実際。
		std::size_t levelHeightMismatch = 0;
		std::string levelHeightProbe;
		std::size_t classesShown = 0; // 置いた後に表示へ戻せたクラス数（0 なら映らない）
		std::size_t updateFailed = 0; // クラスを戻した後の再更新に失敗したビューポート
#if VW_DRAW_VERIFY
		// 検算（dev だけ）: 置いたレベル記号のうち、描いた文字に名前が見つからなかった数。
		std::size_t levelNameUnseen = 0;
		// その 1 個目の実際（描いた文字とレイアウトの中身）。
		std::string levelNameProbe;
		// 寸法 1 本目の見え方（値表示・文字の大きさ・規格）。伏図と軸組図を並べて比べる。
		std::string dimensionProbe;
		// レベル記号 1 個目の形: 組み直す前のレイアウトの中身（型と外形）と、描いた範囲と
		// 狙い（起点〜基準線の終点）。レイアウトの長さが用紙 mm で効いているかを確かめる。
		std::string levelShapeProbe;
		bool levelShapeDrawn = false; // 描いた範囲を控え終えたか（文書で 1 個目だけ）
		bool levelPathProbed = false; // 引出線を切る前後を控え終えたか（文書で 1 個目だけ）
#endif
	};

	// 注釈へ置けたレベル記号と、その命令の高さ（仕上げの作り直しと検算に使う。
	// finishLevelMarks）。ハンドルは描画の間だけ使い、命令には載せない。
	//   startX / endX … 狙った起点と基準線の終点（注釈空間の x。dev の検算が描いた範囲と
	//                    比べる）
	struct PlacedLevelMark
	{
		MCObjectHandle mark = nil;
		double elevation = 0.0;
		double startX = 0.0;
		double endX = 0.0;
	};

	// レベル基準線 PIO の定義を**設定ダイアログを出さない**で用意する。レベル記号を 1 つでも
	// 置くフェーズ（軸組図）の先頭で 1 回呼ぶ（DrawUtil の PrepareCustomObjectDefinition）。
	void prepareLevelMarkPlugin();

	// ビューポート 1 枚ぶんの寸法（command.dimensions）とレベル記号（levels。伏図は空）を
	// 注釈として置く。standard は寸法規格の名前（core::Document::dimensionStandard）、scale は
	// そのビューポートの縮尺の分母（寸法線までの距離を用紙 mm からモデル mm へ直す）。
	// 置けた列の数を返し、内訳を counts へ積む。placedLevels を渡せば、置けたレベル記号を
	// そこへ積む（ビューポートの更新を済ませた後に finishLevelMarks へ渡す）。
	std::size_t drawViewportDimensions(MCObjectHandle viewport,
									   const core::ViewportCommand& command,
									   const std::vector<core::LevelMarkCommand>& levels,
									   const std::string& standard, double scale,
									   DimensionCounts& counts,
									   std::vector<PlacedLevelMark>* placedLevels = nullptr);

	// 断面ビューポートのレベル記号を仕上げる。**そのビューポートの更新をすべて済ませた後**に
	// 呼ぶ——断面の向きをビュー行列へ写し（DrawUtil の CopySectionViewMatrix）、置いた
	// レベル記号を作り直して、描いた高さを命令と引き比べる（合わなければ数えるだけで直さない。
	// 高さはストーリレベルから来る出力で、書けない）。marks が空なら何もしない。
	void finishLevelMarks(MCObjectHandle viewport, const std::vector<PlacedLevelMark>& marks,
						  DimensionCounts& counts);

	// 集計を人が読める 1 行の診断にする（異常が無ければ空文字）。label は図の種別
	// （"伏図" / "軸組図"）。
	std::string dimensionDiagnostics(const std::string& label, const DimensionCounts& counts);

	// 平常の内訳（レベル記号の高さの補正・dev の検算）を 1 行にする（無ければ空文字）。
	// 診断（dimensionDiagnostics）と違って「問題あり」にはしない——描画側の outInfo へ出す。
	std::string dimensionInfo(const std::string& label, const DimensionCounts& counts);
} // namespace HomeskzIfcImport::draw
