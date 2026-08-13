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
//	  * gSDK->GetObjectBounds(h, WorldRect&) … できたビューポートの実寸（並べるのに使う）
//	  * gSDK->MoveObject(h, dx, dy)          … シートレイヤ上での移動
//
//	【切断面の与え方（ローカル確認の要点）】ISDK の引数名は 3 点とも "sectionLinePt" だが、
//	VW の UI は「切断線の点を 2 つ以上クリック → **切断の向き**をクリック → 奥行きを指定」
//	という順で、API の引数もこの順（点 2 つ＋向きの点＋奥行き）に対応するとみて、
//	  * pt1 / pt2 … 切断線（通りの上を端から端まで。命令の lineStart / lineEnd）
//	  * pt3       … **見る側**を示す点（命令の viewPoint。指示線の中点から視線方向へ離した点）
//	として渡す。**もし pt3 が「切断線の 3 点目」だった場合は指示線が L 字に折れる**ので、
//	ローカル確認で図がおかしければここ（と parse/Section の viewPoint）を直す。
//	視線の向きの決め方（X通り＝−X 方向・Y通り＝+Y 方向。図面の右へ座標が増える＝通り名が
//	左から右へ並ぶ）は parse/Section が持つ。
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

		// 断面ビューポートを 1 枚作る。作れなければ nil。
		MCObjectHandle CreateSectionViewport(const core::SectionCommand& command,
											 MCObjectHandle sheetLayer)
		{
			const WorldPt start(command.lineStart.x, command.lineStart.y);
			const WorldPt end(command.lineEnd.x, command.lineEnd.y);
			const WorldPt view(command.viewPoint.x, command.viewPoint.y);
			return gSDK->CreateSectionViewport(start, end, view, command.depth, command.startHeight,
											   command.endHeight, sheetLayer);
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
