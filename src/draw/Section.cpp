//
//	draw/Section.cpp
//
//	軸組図（断面ビューポート）描画の実装。Python 版 vw/section.py の execute_sections /
//	_place_section_line / _arrange_viewports に対応する。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド（SDK あり）でのみ
//	コンパイルされ、無 SDK の core/parse ライブラリには入れない。
//
//	使用する SDK API（Vectorworks 2026 SDK。ci-debug の sdk-grep / shell で確認済み）:
//	  * gSDK->CreateSectionViewport(pt1, pt2, pt3, depth, startHeight, endHeight, layer)
//	                                      … 断面ビューポートの新規作成。ヘッダのコメントに
//	                                        「クラス・レイヤの表示はこの呼び出しでは扱わない。
//	                                        呼び出し後に設定し、そのあとで更新すること」と
//	                                        あるので、後段は伏図と同じ手順（draw/DrawUtil の
//	                                        ConfigureViewport）で仕上げる。
//	  * gSDK->SetObjectVariable(h, 1064/1035/1059, …) … 断面の見え方（下記）
//	  * gSDK->GetObjectBounds(h, WorldRect&) … できたビューポートの実寸（並べるのに使う）
//	  * gSDK->MoveObject(h, dx, dy)          … シートレイヤ上での移動
//
//	【切断面の与え方（ローカル確認で実証済み）】ISDK の引数名は 3 点とも "sectionLinePt" だが、
//	VW の UI は「切断線の点を 2 つ以上クリック → **切断の向き**をクリック → 奥行きを指定」
//	という順で、API の引数もこの順（点 2 つ＋向きの点＋奥行き）に対応する:
//	  * pt1 / pt2 … 切断線（通りの上を端から端まで。命令の lineStart / lineEnd）
//	  * pt3       … **見る側**を示す点（命令の viewPoint。指示線の中点から視線方向へ離した点）
//	**実機で断面指示線が直線になり、視線の向きも意図どおりであることを確認済み**（pt3 が
//	「切断線の 3 点目」であれば指示線は L 字に折れるはずで、そうならなかった）。視線の向きの
//	決め方（X通り＝−X 方向・Y通り＝+Y 方向。図面の右へ座標が増える＝通り名が左から右へ並ぶ）は
//	parse/Section が持つ。
//
//	【軸組図としての見え方（要件）】断面の範囲は**限らず**（長さ・高さ・奥行きとも無制限＝
//	VW の「範囲」タブの既定）、**切断面より奥は表示せず**、**プレイナー（アクティブレイヤ
//	平面）図形は表示せず**、**2D コンポーネントは表示する**。範囲以外はオブジェクト変数で
//	設定する（下記の定数）。これらは「どこを切るか」ではなく「どう描くか」なので、命令セット
//	（core::SectionCommand）には載せずここが持つ。
//

#include "PluginPrefix.h"
#include "draw/Section.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// シートレイヤ上でビューポートを並べるレイアウト（用紙上・mm。Python 版 vw/section.py の
		// _ARRANGE_ORIGIN / _ARRANGE_COLUMNS / _ARRANGE_GAP と同じ値）。左上を基準に 1 行
		// kArrangeColumns 枚ずつ、各ビューポートの実寸に余白を足して詰める。
		constexpr double kArrangeOriginX = 0.0;
		constexpr double kArrangeOriginY = 0.0;
		constexpr int kArrangeColumns = 5;
		constexpr double kArrangeGap = 300.0;

		// 断面の範囲（長さ・高さ・奥行き）に渡す値。**軸組図は範囲を限らない**（要件）。
		// VW の「範囲」タブは既定が全方向 infinite で、CreateSectionViewport の
		// depth / startHeight / endHeight は**その範囲を限りたいときに与える値**なので、
		// 0＝制限なし＝既定の infinite のままにする、と読んで 0 を渡す。
		// **ローカル確認の要点**: 断面が空になる・帯状に切れて見えるなら、この値が
		// 「範囲 0」と解釈されているということなので、その時は実寸の範囲を渡す形へ戻す。
		constexpr double kInfiniteExtent = 0.0;

		// 断面ビューポートのオブジェクト変数（Kernel/API/ObjectVariables.h。ci-debug で確認）。
		//   1064 … 切断面より**奥**の図形を表示するか（要件: 表示しない）
		//   1035 … プレイナー（レイヤ平面）／2D 図形を表示するか（要件: 表示しない）
		//   1059 … ハイブリッドシンボル等の 2D コンポーネントを表示するか（要件: 表示する。
		//          既定でも表示だが、既定値に頼らず明示する）
		// **どれもビューポートの更新より前に設定する**（更新時の描画へ効かせるため。
		// CreateSectionViewport のヘッダコメントも「表示設定は呼び出し後、更新はその後」）。
		constexpr short kOVDisplayObjectsBeyondCutPlane = 1064;
		constexpr short kOVDisplayPlanarObjects = 1035;
		constexpr short kOVDisplay2DComponents = 1059;
		constexpr Boolean kShowObjectsBeyondCutPlane = false;
		constexpr Boolean kShowPlanarObjects = false;
		constexpr Boolean kShow2DComponents = true;

		// オブジェクト変数へ真偽値を書き込む（draw/Footing の SetBooleanVariable と同じ流儀）。
		void SetBooleanVariable(MCObjectHandle object, short variable, Boolean value)
		{
			gSDK->SetObjectVariable(object, variable, TVariableBlock(value));
		}

		// 断面ビューポートを 1 枚作る。作れなければ nil。範囲（長さ・高さ・奥行き）は
		// 限らない（上記 kInfiniteExtent）。
		MCObjectHandle CreateSectionViewport(const core::SectionCommand& command,
											 MCObjectHandle sheetLayer)
		{
			const WorldPt start(command.lineStart.x, command.lineStart.y);
			const WorldPt end(command.lineEnd.x, command.lineEnd.y);
			const WorldPt view(command.viewPoint.x, command.viewPoint.y);
			return gSDK->CreateSectionViewport(start, end, view, kInfiniteExtent, kInfiniteExtent,
											   kInfiniteExtent, sheetLayer);
		}

		// 軸組図としての見え方を整える（要件。ConfigureViewport＝更新より**前**に呼ぶ）。
		void ApplySectionDisplayOptions(MCObjectHandle viewport)
		{
			SetBooleanVariable(viewport, kOVDisplayObjectsBeyondCutPlane,
							   kShowObjectsBeyondCutPlane);
			SetBooleanVariable(viewport, kOVDisplayPlanarObjects, kShowPlanarObjects);
			SetBooleanVariable(viewport, kOVDisplay2DComponents, kShow2DComponents);
		}

		// できたビューポートをシートレイヤ上で重ならないように格子状へ並べる（Python 版
		// _arrange_viewports）。**実寸は描いてみるまで分からない**ので、GetObjectBounds で
		// 測ってから左上を合わせる。測れないものはその場に残す（並びが崩れるだけで図は残る）。
		void ArrangeViewports(const std::vector<MCObjectHandle>& viewports)
		{
			double cursorX = kArrangeOriginX;
			double cursorY = kArrangeOriginY;
			double rowHeight = 0.0;
			int column = 0;
			for (const MCObjectHandle viewport : viewports)
			{
				WorldRect bounds;
				if (!gSDK->GetObjectBounds(viewport, bounds))
					continue;
				const double width = bounds.right - bounds.left;
				// WorldRect は top > bottom（Y 上向き）。高さは絶対値で見る。
				const double height = std::abs(bounds.top - bounds.bottom);
				gSDK->MoveObject(viewport, cursorX - bounds.left, cursorY - bounds.top);
				cursorX += width + kArrangeGap;
				rowHeight = std::max(rowHeight, height);
				++column;
				if (column >= kArrangeColumns)
				{
					column = 0;
					cursorX = kArrangeOriginX;
					cursorY -= rowHeight + kArrangeGap;
					rowHeight = 0.0;
				}
			}
		}
	} // namespace

	std::size_t drawSections(const core::Document& document, core::ProgressReporter& progress,
							 std::string* note)
	{
		const std::vector<core::SectionCommand>& commands = document.sections;
		if (commands.empty())
			return 0;

		// レイヤの走査とクラスの数え上げは全命令で共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup(document);

		// 描画の前後でカレントレイヤが変わらないようにする（伏図と同じ作法）。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;
		std::size_t classesApplied = 0;
		std::vector<MCObjectHandle> viewports;
		viewports.reserve(commands.size());

		for (const core::SectionCommand& command : commands)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。
			if (progress.cancelled())
				break;
			progress.step();

			const MCObjectHandle sheetLayer = PrepareSheetLayer(command.number, command.title);
			if (sheetLayer == nil)
			{
				++missingSheetLayers;
				continue;
			}

			const MCObjectHandle viewport = CreateSectionViewport(command, sheetLayer);
			if (viewport == nil)
			{
				// 断面ビューポートを作れなかった。件数を診断へ残して「命令はあるのに軸組図が
				// 無い」原因を切り分けられるようにする（1 枚の失敗で残りを止めない）。
				++missingViewports;
				continue;
			}

			// 表示の作法（奥を出さない・プレイナー図形を出さない・2D コンポーネントは出す）は
			// **更新より前**に設定する（ConfigureViewport の最後が更新）。
			ApplySectionDisplayOptions(viewport);
			classesApplied += ConfigureViewport(viewport, sheetLayer, setup, command.viewport);
			viewports.push_back(viewport);
			++drawn;
		}

		// 作ったぶんをシート上に並べる（中止で途中まででも、描けたものは重ならないようにする）。
		ArrangeViewports(viewports);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		const bool classesBroken = drawn > 0 && classesApplied == 0;
		if (note != nullptr && (missingSheetLayers > 0 || missingViewports > 0 || classesBroken))
		{
			std::string text = "軸組図の診断: ";
			if (missingSheetLayers > 0)
				text += "シートレイヤを作れなかった命令 " + std::to_string(missingSheetLayers) +
						" 件。";
			if (missingViewports > 0)
				text += "断面ビューポートを作れなかった命令 " + std::to_string(missingViewports) +
						" 件。";
			if (classesBroken)
				text += "クラスを表示に戻せませんでした（対象 " +
						std::to_string(setup.classes.size()) + " クラス）。図形が映りません。";
			*note = std::move(text);
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
