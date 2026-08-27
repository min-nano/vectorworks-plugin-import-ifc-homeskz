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
//	ビューポートを仕上げた後にそれを呼ぶ。凡例は**スタイル無しで置く**ので、置いた後に
//	スタイルを反映させる手当ては要らない。**イメージの縮率は渡さない**——プラグインからは
//	変えられないと分かったので PIO 既定の 1:50 のままにする（draw/Legend.h「既知の制限」）。
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
//	【用紙の割り付け（M18）】縮尺も用紙上の位置も、**文書全体の平面の広がりと用紙の大きさ**
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
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 「用紙に収まったか」を測って確かめるときの遊び（用紙 mm）。線の太さのぶん外形が
		// わずかに広がるので、ぴったりの図を「はみ出した」と数えない。
		constexpr double kFitTol = 1.0;

		// 1 巡目で作った伏図 1 枚（2 巡目で縮尺・タグ・位置を仕上げる。drawSheets）。
		// 命令はポインタで持つ——commands は drawSheets の間ずっと生きている（呼び出し元の
		// Document が所有する）ので、コピーせずに指しておけばよい。
		struct PlacedSheet
		{
			const core::SheetCommand* command = nullptr;
			MCObjectHandle viewport = nil;
		};
	} // namespace

	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note, const ObjectHandles* memberHandles)
	{
		const std::vector<core::SheetCommand>& commands = document.sheets;
		if (commands.empty())
			return 0;

		// レイヤとクラスの列挙は全シートで共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup();

		// M18 用紙の割り付け。**縮尺も位置も全伏図で同じ**にするため、文書全体の平面の
		// 広がり（＝どの伏図にも共通の「建物の大きさ」）から 1 回だけ決める。用紙の大きさは
		// 最初に用意できたシートレイヤから読む（どのシートも同じ用紙という前提。M18）ので、
		// 割り付けの計算はループの中で 1 度だけ走る。
		core::Vec2 contentMin;
		core::Vec2 contentMax;
		const bool haveContent = core::planContentBounds(document, {}, contentMin, contentMax);
		const core::Vec2 contentSize{contentMax.x - contentMin.x, contentMax.y - contentMin.y};
		// **建物の中心**。どの伏図でもこの点が用紙の同じところへ来るように置く（用紙を
		// めくっても図が動かない）。
		const core::Vec2 anchor{(contentMin.x + contentMax.x) / 2.0,
								(contentMin.y + contentMax.y) / 2.0};
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
		// 確定した縮尺を当て直せなかった枚数（仮の縮尺のまま残る）。
		std::size_t missingScale = 0;
		// 見積もった縮尺では用紙に収まらなかった枚数（測った外形が図の領域より大きい）。
		std::size_t oversized = 0;
		// 凡例と重なった枚数（図が広くて右上の空きへ避けきれなかった）。
		std::size_t legendOverlap = 0;
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

		// --- 1 巡目: シートレイヤ・ビューポート・凡例を作る -------------------------
		//
		// **縮尺はまだ確定できない。** 用紙をどれだけ凡例のために空けるかは
		// 「実際に置いた凡例の幅」で決まり（core/Layout.h「凡例の幅は定数で持たない」）、
		// その凡例に何が並ぶかは**ビューポートに映るもの**が決めるので、鶏と卵になる。
		// そこで 1 巡目は**凡例のぶんを空けない仮の割り付け**で図を作り、凡例を置いてから
		// 幅を測って割り付けを確定し、2 巡目で縮尺と位置を仕上げる。
		std::vector<PlacedSheet> placed;
		placed.reserve(commands.size());
		// **仮の割り付けは素の値で持つ**（optional にしない）。用紙が読めるまでは既定値の
		// ままで、最初のシートレイヤで埋める——2 つの optional を連動させると
		// 「片方が入っていればもう片方も入っている」ことをコンパイラにも clang-tidy にも
		// 説明できず、bugprone-unchecked-optional-access に引っかかる。
		std::optional<SheetPaper> paper;
		core::PlanLayout provisional;

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

			// 用紙の大きさは**最初に用意できたシートレイヤ**から読む（どのシートも同じ用紙
			// という前提。M18）。仮の割り付けもここで 1 回だけ作る。
			if (!paper.has_value())
			{
				paper = SheetPaperArea(sheetLayer);
				provisional = core::planLayout(haveContent ? contentSize : core::Vec2{},
											   paper->printable, 0.0);
			}

			const MCObjectHandle viewport = gSDK->CreateViewport(sheetLayer);
			if (viewport == nil)
			{
				// シートレイヤは残る（＝図面に空のシートができる）。件数を診断へ残して
				// 「シートはあるのに図が無い」原因が描画側だと分かるようにする。
				++missingViewports;
				continue;
			}

			const double scale = haveContent ? provisional.scale : 0.0;
			const ViewportFinish finish = ConfigureViewport(
				viewport, sheetLayer, setup, command.viewport, ViewportProjection::Plan, scale);
			classesApplied += finish.classesApplied;
			if (!finish.planViewApplied)
				++missingPlanView;

			// グラフィック凡例は**ビューポートではなくシートレイヤ**に載せる（用紙の上）。
			// 置き場所は仮——中身が流し込まれて大きさが定まってから右上へ揃える
			// （draw/Legend の placeLegends）。凡例に並ぶのは**このシートのビューポートに
			// 映っているシンボルだけ**にしたいので、いま作ったビューポートを渡す
			// （draw/Legend.h「そのシートのビューポートでフィルタする」）——**凡例を
			// ビューポートより後に作る**のはそのためでもある。
			if (command.legend.has_value())
				drawSheetLegend(sheetLayer, provisional.legendTopRight, viewport, legends);

			placed.push_back(PlacedSheet{&command, viewport});
		}

		// --- 凡例を実測して割り付けを確定する ---------------------------------------
		//
		// **中身を流し込むまで凡例の大きさは決まらない**（draw/Legend.h）。流し込んでから
		// いちばん広い凡例の幅を測り、そのぶんだけ右を空けた割り付けを作る。
		//
		refreshLegends(legends);
		const double legendWidth = measureLegendWidth(legends);
		const core::PlanLayout layout =
			paper.has_value() ? core::planLayout(haveContent ? contentSize : core::Vec2{},
												 paper->printable, legendWidth)
							  : core::PlanLayout{};

		// --- 2 巡目: 確定した縮尺を当て、タグを置き、用紙の上へ動かす ----------------
		//
		// **縮尺が変わったときだけ**当て直す（更新は重い。draw/DrawUtil の
		// ApplyViewportScale）。凡例が細くて仮の割り付けと同じ縮尺に落ち着くなら、
		// 1 巡目の図をそのまま使える。
		const bool rescale = haveContent && paper.has_value() && layout.scale != provisional.scale;
		for (const PlacedSheet& sheet : placed)
		{
			const core::SheetCommand& command = *sheet.command;
			if (rescale && !ApplyViewportScale(sheet.viewport, layout.scale))
				++missingScale;

			// M18 用紙の上での位置。**この伏図に映る範囲**（命令の表示レイヤで絞った平面の
			// 広がり）の中心が、用紙のどこへ来るべきかを計算して合わせる——伏図ごとに映す
			// ものが違えば図の中身の広がりも違うので、単に外形の中心を用紙の中心へ置くと
			// 用紙をめくるたびに建物がずれる。建物の中心（anchor）が常に同じ点へ来るよう、
			// その差だけずらした位置へ外形の中心を合わせる。
			//
			// ★**測る → データタグを置く → 動かす**の順で行う（draw/DrawUtil の
			// MoveViewportBy）。タグは注釈へ置いた実位置を測って直す作りで、その実測は
			// ビューポートが用紙のどこに在るかに影響されるため、先に動かすとタグだけが
			// 同じ量ずれる。注釈はビューポートと一緒に動くので、後から動かせば位置は保たれる。
			core::Vec2 drawnCenter;
			core::Vec2 drawnSize;
			const bool measured = MeasureViewport(sheet.viewport, drawnCenter, drawnSize);
			core::Vec2 delta;
			if (haveContent && paper.has_value())
			{
				core::Vec2 target = layout.viewportCenter;
				core::Vec2 sheetMin;
				core::Vec2 sheetMax;
				if (core::planContentBounds(document, command.viewport.layers, sheetMin, sheetMax))
				{
					const core::Vec2 sheetCenter{(sheetMin.x + sheetMax.x) / 2.0,
												 (sheetMin.y + sheetMax.y) / 2.0};
					target = target + ((sheetCenter - anchor) * (1.0 / layout.scale));
				}
				if (!measured)
					++missingPlacement;
				else
				{
					delta = target - drawnCenter;
					// **見積もりどおりに収まったかを測って確かめる**（core/Layout.h の
					// PlanLayout::plan）。命令の座標には現れないもの（通り芯の丸など）が
					// 図に出るぶん、実際の図は見積もりより大きくなりうる。
					if (drawnSize.x > layout.plan.width() + kFitTol ||
						drawnSize.y > layout.plan.height() + kFitTol)
						++oversized;
					// 凡例の帯へ食い込んだか。縮尺は凡例のぶんを引いてから決めている
					// （core/Layout.h の planLayout）ので通常は重ならないが、命令の座標に
					// 現れないもの（通り芯の丸など）のぶん実際の図は見積もりより大きく
					// なりうる——黙って重ねずに数えて診断へ残す。
					if (legendWidth > 0.0 && target.x + (drawnSize.x / 2.0) >
												 layout.legendTopRight.x - legendWidth - kFitTol)
						++legendOverlap;
				}
			}

			// 断面寸法データタグは**ビューポートを仕上げた後**に置く（ConfigureViewport
			// の最後が更新で、注釈はその後に足しても図に出る）。**ビューポートを動かす前**
			// でなければならない（上記 ★）。
			drawViewportTags(sheet.viewport, command.viewport, members, tags);

			// 用紙の上へ動かす（注釈も一緒に動く）。
			if (measured)
				MoveViewportBy(sheet.viewport, delta);
			++drawn;
		}

		// 図が仕上がったので**もう一度**中身を流し込み（凡例に並ぶのはそのシートの
		// ビューポートに映るシンボルなので、縮尺を当て直した後の図で取り直す）、右上を揃える。
		refreshLegends(legends);
		placeLegends(legends, layout.legendTopRight);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		// 診断行は要素ごとに 1 行ずつ足す（原因が別物なので混ぜない）。
		const auto addNote = [note](const std::string& text)
		{
			if (note == nullptr || text.empty())
				return;
			if (!note->empty())
				*note += "\n";
			*note += text;
		};

		// M18 割り付けの結果。**縮尺は「印刷可能領域・凡例の幅・建物の広がり」の 3 つだけで
		// 決まる**ので、その 3 つと結果の縮尺を残す——思ったより小さい（大きい）ときに、
		// どれが効いたのかをローカル確認の場で確かめられる（実際に「1/50 のはずが 1/75 に
		// なる」の切り分けで要った。docs/DEV-NOTES.md M18）。
		//
		// **調査のための値はここには出さない**（DEV-NOTES「実機確認の作法」の「役目を終えた
		// 計装は消す」）。余白の生の値と単位の解釈は規約を詰めるために要ったもので、
		// 実機で確定した（図面の単位で返る）ので、**解釈できなかったときだけ**下の診断行へ
		// 出す。はみ出し・凡例との重なりも同じく件数として下で数える。
		if (note != nullptr && paper.has_value())
		{
			const auto mm = [](double value) { return std::to_string(std::lround(value)); };
			std::string text = "伏図の割り付け（mm）: 用紙 " + mm(paper->paper.x) + "×" +
							   mm(paper->paper.y) + " / 印刷可能 " + mm(paper->printable.width()) +
							   "×" + mm(paper->printable.height()) + " / 凡例 " + mm(legendWidth);
			if (haveContent)
				text += " / 建物 " + mm(contentSize.x) + "×" + mm(contentSize.y) + " → 用紙上 " +
						mm(contentSize.x / layout.scale) + "×" + mm(contentSize.y / layout.scale) +
						" / 縮尺 1/" + mm(layout.scale);
			addNote(text);
		}

		// 「命令はあるのに 0 枚」のときに、シートレイヤを作れないのか、ビューポートを
		// 作れないのかを切り分けられるようにする。
		const bool classesBroken = drawn > 0 && classesApplied == 0;
		// 余白が読めなかった（＝用紙いっぱいで割り付けた）のは異常側。生の値を添えて、
		// 単位の解釈を疑えるようにする（draw/DrawUtil の SheetPaperArea）。
		const bool marginsUnread = paper.has_value() && !paper->marginsRead;
		if (note != nullptr &&
			(missingSheetLayers > 0 || missingViewports > 0 || classesBroken ||
			 missingPlanView > 0 || missingPlacement > 0 || missingScale > 0 || oversized > 0 ||
			 legendOverlap > 0 || !haveContent || marginsUnread))
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
			if (missingScale > 0)
				text += "縮尺を当て直せなかった伏図 " + std::to_string(missingScale) +
						" 枚（凡例の幅から決めた縮尺が入らず、仮の縮尺のままです）。";
			if (oversized > 0)
				text += "用紙に収まらなかった伏図 " + std::to_string(oversized) +
						" 枚（縮尺の見積もりより図が大きくなりました）。";
			if (legendOverlap > 0)
				text += "凡例と重なった伏図 " + std::to_string(legendOverlap) +
						" 枚（図が広く、右上の空きへ避けきれませんでした）。";
			if (marginsUnread)
			{
				const auto raw = [](double value)
				{
					std::array<char, 32> buffer{};
					std::snprintf(buffer.data(), buffer.size(), "%.3f", value);
					return std::string(buffer.data());
				};
				const SheetMargins& margins = paper->rawMargins;
				text += "用紙の余白を解釈できなかったので、用紙いっぱいで割り付けました"
						"（SDK が返した値: 左" +
						raw(margins.left) + " 右" + raw(margins.right) + " 下" +
						raw(margins.bottom) + " 上" + raw(margins.top) + "）。";
			}
			addNote(text);
		}

		addNote(tagDiagnostics("伏図", tags));
		addNote(legendDiagnostics(legends));
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
