//
//	draw/Sheet.cpp
//
//	シート（伏図）描画の実装。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【シートレイヤに載るのはビューポートだけではない】伏図には**グラフィック凡例**
//	（VW 標準の "GraphicLegend" PIO）も 1 つ載る（M13）。凡例はビューポート注釈では
//	なくシートレイヤ（＝用紙）へ直接置くので、置き方は draw/Legend が持つ。ここは
//	ビューポートを仕上げた後にそれを呼び、**全シートを置き終えてからスタイルごとの
//	反映**（updateLegendStyles）をまとめて 1 回行う（draw/Legend.h「スタイルは当てる
//	だけでは効かない」）。
//
//	【シートレイヤとビューポートの手当ては draw/DrawUtil が持つ】シートレイヤの用意
//	（PrepareSheetLayer）・表示レイヤの絞り込み・クラス表示・縮尺・図面タイトル/図番・更新
//	（ConfigureViewport）は、軸組図（draw/Section。M14）と**逐語的に同じ**手順なので
//	draw/DrawUtil へ寄せてある（かつてはこのファイルの無名名前空間にあった）。ここに残るのは
//	「伏図 1 枚ごとに平面ビューポートを 1 つ作る」というこの要素固有の流れだけ。
//
//	使用する SDK API のうちこのファイル固有のもの:
//	  * gSDK->CreateViewport(sheetLayer)  … 平面ビューポート生成
//	  * gSDK->GetCurrentLayer / SetCurrentLayer … カレントレイヤの退避と復帰
//
//	【投影は 2D/平面へ作り直させる】`CreateViewport` が作ったビューポートは、パレット上は「2D/
//	平面」なのに**描画は 3D の「上」ビューのまま**という食い違いを起こす（更新ボタンを押しても
//	直らない）。伏図なので `ViewportProjection::Plan` を渡して作り直させる——手順と理由は
//	draw/DrawUtil.h の ViewportProjection。**軸組図（draw/Section）は Keep** で、
//	こちらだけの手当て。
//
//	【用紙の割り付け（M16）】縮尺も用紙上の位置も、**文書全体の平面の広がりと用紙の大きさ**
//	から 1 回だけ決める（core::planLayout）。伏図は全図が同じ縮尺・同じ位置でなければならない
//	——用紙をめくったときに建物が動くと、図面として読めない。位置は「ビューポートの外形の
//	中心を用紙の中心へ」ではなく、**建物の中心が常に用紙の同じ点へ来る**ように合わせる
//	（伏図ごとに映すレイヤが違えば図の中身の広がりも違うので、外形で揃えるとページごとに
//	ずれる）。凡例はビューポートのために空けた右の 1 列へ寄せるので、図とは重ならない。
//
//	【重ね順はここでは扱わない】床・野地板が柱・梁を覆わないようにする件は、**ドキュメントの
//	デザインレイヤの並べ替え**（draw/Story の reorderStoryLayers）が担う。per-viewport の
//	上書き（SetViewportLayerStackingOverride）は実機で効かなかった——呼び出しは true を
//	返すのに GetNumViewportLayerStackingOverrides は 0 のままで、OIP も「順序を上書き:
//	いいえ」だった——ので捨てた。**並べ替えはビューポート生成より前**に済ませる必要がある
//	（生成時の重ね順で描かれるため。draw/ExecuteDocument の実行順）。
//

#include "PluginPrefix.h"
#include "draw/Sheet.h"
#include "draw/DrawUtil.h"
#include "draw/Legend.h"
#include "draw/Tag.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 「用紙に収まったか」を測って確かめるときの遊び（用紙 mm）。線の太さのぶん外形が
		// わずかに広がるので、ぴったりの図を「はみ出した」と数えない。
		constexpr double kFitTol = 1.0;
	} // namespace

	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note, const ObjectHandles* memberHandles)
	{
		const std::vector<core::SheetCommand>& commands = document.sheets;
		if (commands.empty())
			return 0;

		// レイヤとクラスの列挙は全シートで共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup();

		// M16 用紙の割り付け。**縮尺も位置も全伏図で同じ**にするため、文書全体の平面の
		// 広がり（＝どの伏図にも共通の「建物の大きさ」）から 1 回だけ決める。用紙の大きさは
		// 最初に用意できたシートレイヤから読む（どのシートも同じ用紙という前提。M16）ので、
		// 割り付けの計算はループの中で 1 度だけ走る。
		core::Vec2 contentMin;
		core::Vec2 contentMax;
		const bool haveContent = core::planContentBounds(document, {}, contentMin, contentMax);
		const core::Vec2 contentSize{contentMax.x - contentMin.x, contentMax.y - contentMin.y};
		// **建物の中心**。どの伏図でもこの点が用紙の同じところへ来るように置く（用紙を
		// めくっても図が動かない）。
		const core::Vec2 anchor{(contentMin.x + contentMax.x) / 2.0,
								(contentMin.y + contentMax.y) / 2.0};
		std::optional<core::PlanLayout> layout;

		// 描画の前後でカレントレイヤが変わると以降のフェーズ（軸組図＝M14）に響くので、
		// 元のレイヤへ戻せるよう控えておく。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;
		// クラス表示は「設定できた数」を数える（0 なら図形が 1 つも映らない）。
		std::size_t classesApplied = 0;
		// 2D/平面へ作り直せなかった枚数（＝3D の「上」に見えるビューポートの数）。
		std::size_t missingPlanView = 0;
		// 用紙の上で位置を合わせられなかった枚数（外形を測れなかった＝置いた場所のまま）。
		std::size_t missingPlacement = 0;
		// 見積もった縮尺では用紙に収まらなかった枚数（測った外形が作図域より大きい）。
		std::size_t oversized = 0;
		// 断面寸法データタグ（M13）。関連付け先は drawMembers が記録した対応表から引く
		// （渡されなければ空の表＝関連付け無しで置く。draw/Tag.h）。
		const ObjectHandles emptyHandles;
		const ObjectHandleTable& members =
			memberHandles != nullptr ? memberHandles->table() : emptyHandles.table();
		TagCounts tags;
		// タグ PIO の定義を先に用意する（最初の 1 個で設定ダイアログが出るのを防ぐ。draw/Tag.h）。
		// タグが 1 つも無い文書では定義そのものを作らない（使わない PIO を文書へ足さない）。
		if (std::ranges::any_of(commands, [](const core::SheetCommand& sheet)
								{ return !sheet.viewport.tags.empty(); }))
			prepareDataTagPlugin();

		// M13 グラフィック凡例。タグと同じ理由で PIO の定義を先に用意する（凡例を載せる
		// 伏図が 1 枚も無ければ定義そのものを作らない。draw/Legend.h）。
		LegendCounts legends;
		if (std::ranges::any_of(commands, [](const core::SheetCommand& sheet)
								{ return sheet.legend.has_value(); }))
			prepareGraphicLegendPlugin();

		for (const core::SheetCommand& command : commands)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は枚数で報告し、
			// 描画の前に 1 件進める（＝「いま何枚目を作っているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			const MCObjectHandle sheetLayer = PrepareSheetLayer(command.number, command.title);
			if (sheetLayer == nil)
			{
				++missingSheetLayers;
				continue;
			}

			// 用紙の割り付け（縮尺・図の中心・凡例の右上）。**最初のシートレイヤで 1 回だけ**
			// 決め、以降のシートはその値をそのまま使う（＝全伏図で同じ縮尺・同じ位置）。
			// 建物の広がりが求まらない文書（座標を持つ命令が 1 つも無い）でも用紙の割り付け
			// 自体は作る——縮尺と図の位置は触らないが、凡例の置き場所は用紙だけで決まる。
			if (!layout.has_value())
				layout = core::planLayout(haveContent ? contentSize : core::Vec2{},
										  SheetPageArea(sheetLayer));

			const MCObjectHandle viewport = gSDK->CreateViewport(sheetLayer);
			if (viewport == nil)
			{
				// シートレイヤは残る（＝図面に空のシートができる）。件数を診断へ残して
				// 「シートはあるのに図が無い」原因が描画側だと分かるようにする。
				++missingViewports;
				continue;
			}

			const double scale = haveContent && layout.has_value() ? layout->scale : 0.0;
			const ViewportFinish finish = ConfigureViewport(
				viewport, sheetLayer, setup, command.viewport, ViewportProjection::Plan, scale);
			classesApplied += finish.classesApplied;
			if (!finish.planViewApplied)
				++missingPlanView;

			// M16 用紙の上での位置。**この伏図に映る範囲**（命令の表示レイヤで絞った平面の
			// 広がり）の中心が、用紙のどこへ来るべきかを計算して合わせる——伏図ごとに映す
			// ものが違えば図の中身の広がりも違うので、単に外形の中心を用紙の中心へ置くと
			// 用紙をめくるたびに建物がずれる。建物の中心（anchor）が常に同じ点へ来るよう、
			// その差だけずらした位置へ外形の中心を合わせる。
			// **データタグより前に置く**（注釈まで含めた外形で測るとタグの有無でずれる）。
			if (haveContent && layout.has_value())
			{
				core::Vec2 target = layout->viewportCenter;
				core::Vec2 sheetMin;
				core::Vec2 sheetMax;
				if (core::planContentBounds(document, command.viewport.layers, sheetMin, sheetMax))
				{
					const core::Vec2 sheetCenter{(sheetMin.x + sheetMax.x) / 2.0,
												 (sheetMin.y + sheetMax.y) / 2.0};
					target = target + ((sheetCenter - anchor) * (1.0 / layout->scale));
				}
				core::Vec2 drawn;
				if (!PlaceViewport(viewport, target, &drawn))
					++missingPlacement;
				// **見積もりどおりに収まったかを測って確かめる**（core/Layout.h の
				// PlanLayout::plan）。命令の座標には現れないもの（通り芯の丸など）が図に
				// 出るぶん、実際の図は見積もりより大きくなりうる。
				else if (drawn.x > layout->plan.width() + kFitTol ||
						 drawn.y > layout->plan.height() + kFitTol)
					++oversized;
			}

			// 断面寸法データタグは**ビューポートを仕上げた後**に置く（ConfigureViewport
			// の最後が更新で、注釈はその後に足しても図に出る）。
			drawViewportTags(viewport, command.viewport, members, tags);

			// グラフィック凡例は**ビューポートではなくシートレイヤ**に載せる（用紙の上）。
			// タグの後に置くのは、凡例がカレントレイヤをこのシートレイヤへ移すため——
			// タグは生成したカレントレイヤに一旦入ってから注釈へ移るので、順序を逆にすると
			// タグがシートレイヤを経由することになる（結果は同じだが、経路は素直な方がよい）。
			if (command.legend.has_value() && layout.has_value())
				drawSheetLegend(sheetLayer, *command.legend, layout->legendTopRight, legends);
			++drawn;
		}

		// 凡例の中身（スタイルのソースから集めたセル）を流し込み、右上を揃える。**全部
		// 置いてからスタイルごとに 1 回**（draw/Legend.h「スタイルは当てるだけでは効かない」）。
		const core::Vec2 legendTopRight =
			layout.has_value() ? layout->legendTopRight : core::Vec2{};
		updateLegendStyles(legends, legendTopRight);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		// 診断行（何も無ければ空のまま）。「命令はあるのに 0 枚」のときに、シートレイヤを
		// 作れないのか、ビューポートを作れないのかを切り分けられる。
		const bool classesBroken = drawn > 0 && classesApplied == 0;
		if (note != nullptr &&
			(missingSheetLayers > 0 || missingViewports > 0 || classesBroken ||
			 missingPlanView > 0 || missingPlacement > 0 || oversized > 0 || !haveContent))
		{
			std::string text = "伏図の診断: ";
			if (missingSheetLayers > 0)
				text += "シートレイヤを作れなかった命令 " + std::to_string(missingSheetLayers) +
						" 件。";
			if (missingViewports > 0)
				text +=
					"ビューポートを作れなかった命令 " + std::to_string(missingViewports) + " 件。";
			if (classesBroken)
				text += "クラスを表示に戻せませんでした（対象 " +
						std::to_string(setup.classes.size()) + " クラス）。図形が映りません。";
			if (missingPlanView > 0)
				text += "2D/平面（Top/Plan）にできなかった伏図 " + std::to_string(missingPlanView) +
						" 枚（3D の「上」ビューのように描かれます）。";
			if (!haveContent)
				text += "建物の平面の広がりが求まらないため、縮尺と位置を調整していません。";
			if (missingPlacement > 0)
				text += "用紙の上で位置を合わせられなかった伏図 " +
						std::to_string(missingPlacement) + " 枚（外形を測れませんでした）。";
			if (oversized > 0)
				text += "用紙に収まらなかった伏図 " + std::to_string(oversized) +
						" 枚（縮尺の見積もりより図が大きくなりました）。";
			*note = std::move(text);
		}

		// タグ・凡例の診断は伏図の診断とは別行にする（原因が別物なので混ぜない）。
		const auto addNote = [note](const std::string& text)
		{
			if (note == nullptr || text.empty())
				return;
			if (!note->empty())
				*note += "\n";
			*note += text;
		};
		addNote(tagDiagnostics("伏図", tags));
		addNote(legendDiagnostics(legends));
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
