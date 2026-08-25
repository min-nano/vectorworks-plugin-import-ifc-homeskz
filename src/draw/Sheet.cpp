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
//	【重ね順の決め方はここが持つ】床・野地板が柱・梁を覆わないようにする件は、
//	**ビューポート単位のレイヤ重ね順上書き**（draw/DrawUtil の ConfigureViewport に希望順を
//	渡す）で行う。図面のレイヤの並びを動かさないので、ユーザーのナビゲーションパレットが
//	勝手に組み変わらない（CLAUDE.md「既存の図面リソースを…書き換えない」の趣旨）。
//
//	ただしこの上書きは**書き方を外すと 1 件も記録されないまま true が返る**（M13 の 1 回目・
//	その次の 1 回目とも空振りした。位置の与え方は draw/DrawUtil の ApplyLayerStacking）。
//	そこで**1 枚目のビューポートで記録されたかを読み戻し**、0 件なら退避路——**デザイン
//	レイヤの並べ替え**（draw/Story の reorderStoryLayers）——へ切り替える。並べ替えは
//	**ビューポート生成より前**でないと効かないので、そのときは 1 枚目のビューポートを作り
//	直す。どちらに落ちたかは診断行に出る。
//

#include "PluginPrefix.h"
#include "draw/Sheet.h"
#include "draw/ColumnMark.h"
#include "draw/DrawUtil.h"
#include "draw/Legend.h"
#include "draw/Story.h"
#include "draw/Tag.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note, const ObjectHandles* memberHandles)
	{
		const std::vector<core::SheetCommand>& commands = document.sheets;
		if (commands.empty())
			return 0;

		// レイヤとクラスの列挙は全シートで共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup();

		// 希望スタック順（前面→背面）。伏図記号レイヤ（"{to}-柱伏図記号"）はストーリに属さない
		// 独立レイヤなので topLayers として通り芯 "共通" の直下へ差し込む（draw/ColumnMark）。
		// **どのビューポートにも同じ並びを丸ごと与える**——重ね順の上書きは「その図に映る
		// レイヤだけ」では記録されず、図面の全デザインレイヤに位置が要る（実機で確認。
		// draw/DrawUtil の ApplyLayerStacking）。
		const std::vector<std::string> desiredOrder =
			core::desiredStoryLayerOrder(document.stories, planMarkLayerNames(document));

		// 【診断】いま図面にあるビューポートの重ね順上書きを読む（**書き込みは一切しない**）。
		// **これが位置の与え方を教えてくれた**——GUI で「順序を上書き」を付けたビューポートを
		// 読んだら "共通=1 … 2-野地板=27" と並び、1 始まり・1 が最前面・全デザインレイヤに
		// 位置が要ることが分かった（draw/DrawUtil の ApplyLayerStacking）。上書きが安定して
		// 効くと分かったら外してよい（それ以降は自分が付けた上書きを読み上げるだけになる）。
		const std::string existingStacking = ReadStackingOverrideDiagnostics(desiredOrder);

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
		// 重ね順: ビューポート単位の上書きを試し続けてよいか（1 枚目で空振りしたら false）。
		bool stackingPerViewport = true;
		// 退避路（デザインレイヤの並べ替え）へ落ちたときの記録。診断行に出す。
		bool stackingFellBack = false;
		bool stackingIndexVerified = true;
		std::size_t stackingRequested = 0;
		std::size_t reorderedLayers = 0;
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

			MCObjectHandle viewport = gSDK->CreateViewport(sheetLayer);
			if (viewport == nil)
			{
				// シートレイヤは残る（＝図面に空のシートができる）。件数を診断へ残して
				// 「シートはあるのに図が無い」原因が描画側だと分かるようにする。
				++missingViewports;
				continue;
			}

			// 重ね順（前面→背面）。退避路へ落ちた後は空を渡す（＝上書きしない。並べ替え
			// 済みのドキュメント順で描かれる）。
			const std::vector<std::string> stacking =
				stackingPerViewport ? desiredOrder : std::vector<std::string>{};

			ViewportFinish finish = ConfigureViewport(viewport, sheetLayer, setup, command.viewport,
													  ViewportProjection::Plan, stacking);

			// 上書きが**1 件も記録されなかった**ら、この VW では機能そのものが効いていない。
			// 退避路（デザインレイヤの並べ替え）へ 1 度だけ切り替え、このビューポートを作り
			// 直す——ビューポートは**生成時の重ね順**で描かれるので、並べ替えを後から行っても
			// 既にある図には反映されない（docs/DEV-NOTES.md「レイヤ・ストーリ・重ね順」）。
			// 判断は**2 枚以上のレイヤに位置を与えたとき**だけ行う（1 枚では並べようが無く、
			// 記録 0 件が空振りの証拠にならない）。
			if (stackingPerViewport && finish.stackingRequested >= 2 &&
				finish.stackingRecorded == 0)
			{
				stackingPerViewport = false;
				stackingFellBack = true;
				stackingIndexVerified = finish.layerIndexVerified;
				stackingRequested = finish.stackingRequested;
				reorderedLayers = reorderStoryLayers(document);

				gSDK->DeleteObject(viewport);
				viewport = gSDK->CreateViewport(sheetLayer);
				if (viewport == nil)
				{
					++missingViewports;
					continue;
				}
				finish = ConfigureViewport(viewport, sheetLayer, setup, command.viewport,
										   ViewportProjection::Plan, {});
			}

			classesApplied += finish.classesApplied;
			if (!finish.planViewApplied)
				++missingPlanView;

			// 断面寸法データタグは**ビューポートを仕上げた後**に置く（ConfigureViewport
			// の最後が更新で、注釈はその後に足しても図に出る）。
			drawViewportTags(viewport, command.viewport, members, tags);

			// グラフィック凡例は**ビューポートではなくシートレイヤ**に載せる（用紙の上）。
			// タグの後に置くのは、凡例がカレントレイヤをこのシートレイヤへ移すため——
			// タグは生成したカレントレイヤに一旦入ってから注釈へ移るので、順序を逆にすると
			// タグがシートレイヤを経由することになる（結果は同じだが、経路は素直な方がよい）。
			if (command.legend.has_value())
				drawSheetLegend(sheetLayer, *command.legend, legends);
			++drawn;
		}

		// 凡例の中身（スタイルのソースから集めたセル）を流し込む。**全部置いてから
		// スタイルごとに 1 回**（draw/Legend.h「スタイルは当てるだけでは効かない」）。
		updateLegendStyles(legends);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		// 診断行（何も無ければ空のまま）。「命令はあるのに 0 枚」のときに、シートレイヤを
		// 作れないのか、ビューポートを作れないのかを切り分けられる。
		const bool classesBroken = drawn > 0 && classesApplied == 0;
		if (note != nullptr && (missingSheetLayers > 0 || missingViewports > 0 || classesBroken ||
								missingPlanView > 0))
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
		// 重ね順の診断（別行）。**効いたときは何も出さない**——図面のレイヤの並びが動いて
		// いないこと自体が、ビューポート単位の上書きが効いた証拠になる。
		if (stackingFellBack)
		{
			std::string text = "伏図の重ね順: ビューポート単位の上書きが図面に記録されません"
							   "でした（与えた " +
							   std::to_string(stackingRequested) + " 件・記録 0 件・レイヤ索引は" +
							   (stackingIndexVerified ? "妥当" : "不正の疑い") +
							   "）。デザインレイヤの並べ替えへ切り替えました（動かせたレイヤ " +
							   std::to_string(reorderedLayers) + " 件）。";
			if (reorderedLayers == 0)
				text += "並べ替えも 0 件のため、床・野地板が柱・梁を覆います。";
			addNote(text);
		}
		addNote(existingStacking);
		addNote(tagDiagnostics("伏図", tags));
		addNote(legendDiagnostics(legends));
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
