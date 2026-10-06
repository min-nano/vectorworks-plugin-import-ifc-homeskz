//
//	draw/Sheet.cpp
//
//	シート（伏図）描画の実装。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【図面枠（M28）】取り込み設定で図面枠スタイルが選ばれていれば、各シートレイヤへ図面枠を
//	1 つ置く。**置くのはビューポートの縮尺を確定させた後**で、置いたら最背面へ移動する——先に
//	置くと縮尺欄が 1:1 のまま残り、後に置いたままだと図を覆う。置き方・スタイルの適用方法・
//	重ね順・位置合わせは draw/TitleBlock が持ち、ここは用意した各シートレイヤを記録して、
//	最後に仕上げを呼ぶだけ。
//
//	【シートレイヤに載るのはビューポートだけではない】伏図には**グラフィック凡例**
//	（VW 標準の "GraphicLegend" PIO）も 1 つ載る（M13）。凡例はビューポート注釈では
//	なくシートレイヤ（＝用紙）へ直接置くので、置き方は draw/Legend が持つ。ここは
//	ビューポートを仕上げた後にそれを呼ぶ。凡例は**スタイル無しで置く**ので、置いた後に
//	スタイルを反映させる処理は要らない。**イメージの縮率は変更しない**（PIO 既定の 1:50 の
//	まま。理由は draw/Legend.h「イメージの縮率」）。
//
//	【シートレイヤとビューポートの手当ては draw/DrawUtil が持つ】シートレイヤの用意
//	（PrepareSheetLayer）・表示レイヤの絞り込み・クラス表示・縮尺・図面タイトル/図番・更新
//	（ConfigureViewport）は、軸組図（draw/Section。M14）と**逐語的に同じ**手順なので
//	draw/DrawUtil へ寄せてある（かつてはこのファイルの無名名前空間にあった）。ここに残るのは
//	「伏図 1 枚ごとに平面ビューポートを 1 つ生成する」というこの要素固有の流れだけ。
//
//	使用する SDK API のうちこのファイル固有のもの:
//	  * gSDK->CreateViewport(sheetLayer)  … 平面ビューポート生成
//	  * gSDK->GetCurrentLayer / SetCurrentLayer … カレントレイヤの退避と復帰
//
//	【投影は 2D/平面へ作り直させる】`CreateViewport` が生成したビューポートは、パレット上は「2D/
//	平面」なのに**描画は 3D の「上」ビューのまま**という食い違いを起こす（更新ボタンを押しても
//	直らない）。伏図なので `ViewportProjection::Plan` を渡して再設定させる——手順と理由は
//	draw/DrawUtil.h の ViewportProjection。**軸組図（draw/Section）は Keep** で、
//	こちらだけの処理。
//
//	【用紙の割り付け（M18）】縮尺も用紙上の位置も、**文書全体の平面の広がりと用紙の大きさ**
//	から 1 回だけ決める（core::planLayout）。伏図は全図が同じ縮尺・同じ位置でなければならない
//	——用紙をめくったときに建物が動くと、図面として読めない。位置は「ビューポートの外形の
//	中心を用紙の中心へ」ではなく、**建物の中心が常に用紙の同じ点へ来る**ように合わせる
//	（伏図ごとに表示するレイヤが違えば図の中身の広がりも違うので、外形で揃えるとページごとに
//	ずれる）。凡例はビューポートのために空けた右の 1 列へ寄せるので、図とは重ならない。
//
//	【重ね順はここでは扱わない】床・野地板が柱・梁を覆わないようにする件は、**ドキュメントの
//	デザインレイヤの並べ替え**（draw/Story の reorderStoryLayers）が担う。per-viewport の
//	上書き（SetViewportLayerStackingOverride）は実機で反映されなかった——呼び出しは true を
//	返すのに GetNumViewportLayerStackingOverrides は 0 のままで、OIP も「順序を上書き:
//	いいえ」だった——ので採用しなかった。**並べ替えはビューポート生成より前**に済ませる必要がある
//	（生成時の重ね順で描画されるため。draw/ExecuteDocument の実行順）。
//

#include "PluginPrefix.h"
#include "draw/Sheet.h"
#include "draw/DrawUtil.h"
#include "draw/Legend.h"
#include "draw/Tag.h"
#include "draw/Dimension.h"
#include "draw/TitleBlock.h"
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
		// 1 巡目で生成した伏図 1 枚（2 巡目で縮尺・タグ・位置を仕上げる。drawSheets）。
		// 命令はポインタで持つ——commands は drawSheets の間ずっと生存している（呼び出し元の
		// Document が所有する）ので、コピーせずに指しておけばよい。
		struct PlacedSheet
		{
			const core::SheetCommand* command = nullptr;
			MCObjectHandle viewport = nil;
			MCObjectHandle sheetLayer = nil; // 寸法を生成する間アクティブにする（2 巡目）
		};

		// 伏図に表示されるデザインレイヤ（命令の表示レイヤ）の縮尺を、すべて伏図の縮尺へ揃える。
		// 揃えたレイヤ数を返す。
		//
		// 【なぜ要るか】用紙基準（縮尺無視）のシンボル——耐力壁の伏図記号・柱記号・通り芯の
		// 丸——の大きさは「定義の図形（用紙 mm）× **置いたレイヤの縮尺**」で決まる（実機で
		// 確認: 図形 300mm・用紙基準のシンボルを縮尺 1/100 のレイヤへ置くと外接 30000mm）。
		// 伏図はビューポート越しに表示されるので、**レイヤの縮尺を伏図の縮尺に揃えて初めて**紙の
		// 上の大きさが一定になる。プラグインはレイヤを作るときに縮尺を書かないので、何もしないで
		// おくと図面の既定（1/100 等）のまま残る。
		//
		// 【耐力壁レイヤだけにしない】かつては耐力壁レイヤ（"n-耐力壁"）だけを揃えていたが、
		// そうすると伏図が 1/50 に決まった図面で**耐力壁だけ 1/50・他は 1/100** と
		// レイヤの縮尺が食い違い、同じ用紙基準の記号でも柱記号・通り芯の丸だけ紙の上で
		// 倍の大きさで表示された（スキップフロアのフィクスチャで指摘）。伏図が表示するレイヤは
		// すべて同じ縮尺で表示されるのだから、揃える範囲も伏図が表示するレイヤ全部にする。
		//
		// **伏図の縮尺は用紙を読まないと決まらない**（core::planLayout）ので、レイヤを作る
		// ときには分からない。割り付けが確定したあと、ビューポートを仕上げる前に呼ぶ。
		// 耐力壁 PIO はレイヤ縮尺の変化で再描画する印（kObjXPropHasLayerScaleDeps）を
		// 設定してあるので、呼べば記号も追従する。
		std::size_t applyPlanLayerScale(const core::Document& document, double scale)
		{
			if (!(scale > 0.0))
				return 0;

			// 伏図の表示レイヤを**重複なく・決まった順で**集める（同じレイヤへ何度も縮尺を
			// 書かない。順序に依らない結果にする＝CLAUDE.md「決定性を守る」）。耐力壁の
			// レイヤも追加しておく——どの伏図にも表示されない耐力壁があっても記号の大きさは
			// これまでどおり伏図の縮尺で決める。
			std::vector<std::string> layers;
			for (const core::SheetCommand& sheet : document.sheets)
				layers.insert(layers.end(), sheet.viewport.layers.begin(),
							  sheet.viewport.layers.end());
			for (const core::ShearWallCommand& wall : document.shearWalls)
				layers.push_back(wall.layer);
			std::ranges::sort(layers);
			const auto duplicates = std::ranges::unique(layers);
			layers.erase(duplicates.begin(), duplicates.end());

			std::size_t applied = 0;
			for (const std::string& name : layers)
			{
				// 無いレイヤは通知せずにスキップする（その階の生成がスキップされただけ。
				// 描画と同じ規約）。
				const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
				if (layer == nil)
					continue;
				gSDK->SetLayerScaleN(layer, scale);
				++applied;
			}

			// 揃えた枚数は呼び出し側（drawSheets）が割り付けの行と並べて outInfo へ出力する
			// ——診断ログへは要素から直接書かない（CLAUDE.md「重複を作らない置き場所」の
			// 診断ログへの書き出し口）。
			return applied;
		}
	} // namespace

	std::size_t drawSheets(const core::Document& document, core::ProgressReporter& progress,
						   std::string* note, const ObjectHandles* memberHandles,
						   std::string* outInfo, core::DrawCounts* outCounts)
	{
		const std::vector<core::SheetCommand>& commands = document.sheets;
		if (commands.empty())
			return 0;

		// レイヤとクラスの列挙は全シートで共通なので 1 回だけ行う（draw/DrawUtil）。
		const ViewportSetup setup = PrepareViewportSetup();

		// M18 用紙の割り付け。**縮尺も位置も全伏図で同じ**にするため、文書全体の平面の
		// 広がり（＝どの伏図にも共通の「建物の大きさ」）から 1 回だけ決める。用紙の大きさは
		// 最初に用意できたシートレイヤから読む（どのシートも同じ用紙という前提。M18）ので、
		// 割り付けの計算はループの中で 1 度だけ実行される。
		core::Vec2 contentMin;
		core::Vec2 contentMax;
		const bool haveContent = core::planContentBounds(document, {}, contentMin, contentMax);
		const core::Vec2 contentSize{contentMax.x - contentMin.x, contentMax.y - contentMin.y};
		// **建物の中心**。どの伏図でもこの点が用紙の同じところへ来るように置く（用紙を
		// めくっても図が動かない）。
		const core::Vec2 anchor{(contentMin.x + contentMax.x) / 2.0,
								(contentMin.y + contentMax.y) / 2.0};
		// M31 寸法の帯（用紙 mm）。寸法は図の外へ張り出すので、縮尺はその分を用紙から
		// 引いてから選ぶ（core/Layout.h の planLayout）。寸法を入れない文書では 0。
		const double band = core::dimensionBand(core::outermostDimensionTier(commands));
		// M31 寸法の集計。
		DimensionCounts dimensions;
		// 描画の前後でカレントレイヤが変わると以降のフェーズ（軸組図＝M14）に影響するので、
		// 元のレイヤへ戻せるよう記録しておく。
		MCObjectHandle const previousLayer = gSDK->GetCurrentLayer();

		std::size_t drawn = 0;
		std::size_t missingSheetLayers = 0;
		std::size_t missingViewports = 0;
		// クラス表示は「設定できた数」を数える（0 なら図形が 1 つも表示されない）。
		std::size_t classesApplied = 0;
		// 2D/平面へ再設定できなかった枚数（＝3D の「上」に見えるビューポートの数）。
		std::size_t missingPlanView = 0;
		// 用紙の上で位置を合わせられなかった枚数（外形を測れなかった＝置いた場所のまま）。
		std::size_t missingPlacement = 0;
		// 確定した縮尺を再設定できなかった枚数（仮の縮尺のまま残る）。
		std::size_t missingScale = 0;
		// 見積もった縮尺では用紙に収まらなかった枚数（測った外形が図の領域より大きい）と、
		// その**1 枚目の実測**（図番・測った外形・割り当てた枠・はみ出し量）。件数だけでは
		// 「見積もりが少し足りない」と「図そのものが壊れている」を分けられない
		// （M29。組み立ては draw/DrawUtil の DescribeFitOverflow）。
		std::size_t oversized = 0;
		std::string oversizedProbe;
		// 凡例と重なった枚数（図が広くて右上の空きへ避けきれなかった）と 1 枚目の実測。
		std::size_t legendOverlap = 0;
		std::string legendProbe;
		// 測る前に再描画できなかった枚数。**「はみ出した」とは別に数える**——再描画できて
		// いないビューポートの外形は「前に何があったか」でしかなく、収まったかの判定に
		// 使える値ではない（draw/DrawUtil の RefreshViewport）。
		std::size_t staleViewports = 0;
		// 断面寸法データタグ（M13）。関連付け先は drawMembers が記録した対応表から引く
		// （渡されなければ空の表＝関連付け無しで置く。draw/Tag.h）。
		const ObjectHandles emptyHandles;
		const ObjectHandleTable& members =
			memberHandles != nullptr ? memberHandles->table() : emptyHandles.table();
		TagCounts tags;
		// タグ PIO の定義を先に用意する（最初の 1 個で設定ダイアログが表示されるのを防ぐ。
		// draw/Tag.h）。タグが 1 つも無い文書では定義そのものを生成しない（使わない PIO
		// を文書へ追加しない）。
		if (std::ranges::any_of(commands, [](const core::SheetCommand& sheet)
								{ return !sheet.viewport.tags.empty(); }))
			prepareDataTagPlugin();

		// M13 グラフィック凡例。タグと同じ理由で PIO の定義を先に用意する（凡例を載せる
		// 伏図が 1 枚も無ければ定義そのものを生成しない。draw/Legend.h）。
		LegendCounts legends;
		if (std::ranges::any_of(commands, [](const core::SheetCommand& sheet)
								{ return sheet.legend.has_value(); }))
			prepareGraphicLegendPlugin();

		// M28 図面枠。スタイル名が空（＝置かない）か、その名前のスタイルが図面に無ければ
		// 以降の addTitleBlockSheet / finishTitleBlocks は何もしない（draw/TitleBlock.h）。
		TitleBlockCounts titleBlocks = prepareTitleBlocks(document);

		// --- 1 巡目: シートレイヤ・ビューポート・凡例を生成する -------------------------
		//
		// **縮尺はまだ確定できない。** 用紙をどれだけ凡例のために空けるかは
		// 「実際に置いた凡例の幅」で決まり（core/Layout.h「凡例の幅は定数で持たない」）、
		// その凡例に何が並ぶかは**ビューポートに表示されるもの**が決めるので、相互に依存する。
		// そこで 1 巡目は**凡例の分を空けない仮の割り付け**で図を生成し、凡例を置いてから
		// 幅を測って割り付けを確定し、2 巡目で縮尺と位置を仕上げる。
		std::vector<PlacedSheet> placed;
		placed.reserve(commands.size());
		// **仮の割り付けはそのままの値で持つ**（optional にしない）。用紙が読めるまでは既定値の
		// ままで、最初のシートレイヤで埋める——2 つの optional を連動させると
		// 「片方が入っていればもう片方も入っている」ことをコンパイラにも clang-tidy にも
		// 説明できず、bugprone-unchecked-optional-access に該当する。
		std::optional<SheetPaper> paper;
		core::PlanLayout provisional;

		for (const core::SheetCommand& command : commands)
		{
			if (!AdvanceProgress(progress))
				break;

			const MCObjectHandle sheetLayer = PrepareSheetLayer(command.number, command.title);
			if (sheetLayer == nil)
			{
				++missingSheetLayers;
				continue;
			}

			// M28 図面枠を置く用紙として記録する（置くのはビューポートを仕上げた後の
			// finishTitleBlocks。draw/TitleBlock.h）。
			addTitleBlockSheet(sheetLayer, titleBlocks);

			// 用紙の大きさは**最初に用意できたシートレイヤ**から読む（どのシートも同じ用紙
			// という前提。M18）。仮の割り付けもここで 1 回だけ生成する。
			if (!paper.has_value())
			{
				paper = SheetPaperArea(sheetLayer);
				provisional = core::planLayout(haveContent ? contentSize : core::Vec2{},
											   paper->printable, 0.0, band);
			}

			const MCObjectHandle viewport = gSDK->CreateViewport(sheetLayer);
			if (viewport == nil)
			{
				// シートレイヤは残る（＝図面に空のシートができる）。件数を診断へ残して
				// 「シートはあるのに図が無い」原因が描画側だと判別できるようにする。
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
			// 置き場所は仮——中身が設定されて大きさが定まってから右上へ揃える
			// （draw/Legend の placeLegends）。凡例に並ぶのは**このシートのビューポートに
			// 表示されているシンボルだけ**にしたいので、直前に生成したビューポートを渡す
			// （draw/Legend.h「そのシートのビューポートでフィルタする」）——**凡例を
			// ビューポートより後に生成する**のはそのためでもある。
			if (command.legend.has_value())
				drawSheetLegend(sheetLayer, provisional.legendTopRight, viewport, legends);

			placed.push_back(PlacedSheet{&command, viewport, sheetLayer});
		}

		// --- 凡例を実測して割り付けを確定する ---------------------------------------
		//
		// **中身を設定するまで凡例の大きさは決まらない**（draw/Legend.h）。設定してから
		// いちばん広い凡例の幅を測り、その分だけ右を空けた割り付けを生成する。
		//
		refreshLegends(legends);
		const double legendWidth = measureLegendWidth(legends);
		const core::PlanLayout layout =
			paper.has_value() ? core::planLayout(haveContent ? contentSize : core::Vec2{},
												 paper->printable, legendWidth, band)
							  : core::PlanLayout{};

		// --- 伏図記号の大きさを紙の上で一定にする ------------------------------------
		//
		// 耐力壁の伏図記号・柱記号・通り芯の丸は**用紙基準（縮尺無視）のシンボル**で、
		// その大きさは「定義の図形（用紙 mm）× そのレイヤの縮尺」で決まる。伏図は
		// ビューポート越しに表示されるので、**伏図に表示されるレイヤの縮尺を伏図の縮尺へ
		// 揃えて初めて**紙の上で一定になる（applyPlanLayerScale）。
		//
		// **ここでしかできない。** 伏図の縮尺は用紙を読まないと決まらない（core::planLayout）
		// ので、耐力壁を描画する時点では分からない。ビューポートを仕上げる 2 巡目より**前**に
		// 済ませて、更新が新しい縮尺を参照するようにする。
		// **揃えたかを記録する**（2 巡目で再描画するかの判断に要る）——レイヤの縮尺を
		// 変えると伏図に表示される記号の大きさが変わるので、縮尺が同じでビューポートを再描画し
		// ないままだと、**中身が変わった後の図を再描画する前に測る**ことになる（M29）。
		const std::size_t rescaledLayers = applyPlanLayerScale(document, layout.scale);
		const bool layersRescaled = rescaledLayers > 0;

		// --- 2 巡目: 確定した縮尺を設定し、タグを置き、用紙の上へ動かす ----------------
		//
		// **縮尺が変わったときだけ**再設定する（更新は重い。draw/DrawUtil の
		// ApplyViewportScale）。凡例が細くて仮の割り付けと同じ縮尺に落ち着くなら、
		// 1 巡目の図をそのまま使える。
		const bool rescale = haveContent && paper.has_value() && layout.scale != provisional.scale;
		for (const PlacedSheet& sheet : placed)
		{
			const core::SheetCommand& command = *sheet.command;
			if (rescale)
			{
				// 縮尺の再設定＝再描画も兼ねる（ApplyViewportScale が Update する）。
				if (!ApplyViewportScale(sheet.viewport, layout.scale))
					++missingScale;
			}
			// ★**測る前に再描画する。** 縮尺を再設定したなら ApplyViewportScale が済ませて
			// いるが、縮尺が同じでも**伏図のレイヤの縮尺を変えていれば図の中身は変わって
			// いる**（用紙基準の伏図記号の大きさがレイヤ縮尺で決まるため）。ここを省くと
			// `GetObjectBounds` は**変える前に描画した外形**を返すので、同じ命令・同じ割り付け
			// でも「収まったか」の答えが図面の直前の状態で変わる（M29 で実際にそうなった:
			// 描画結果を変えない 2 つのビルドで件数が 1 枚 → 2 枚 ＋ 凡例と重なり 1 枚）。
			// **どちらも起きていないなら再描画しない**（1 巡目の更新のまま中身は変わって
			// いない）。更新は重いので、要らない処理を追加しない。
			else if (layersRescaled && !RefreshViewport(sheet.viewport))
				++staleViewports;

			// M18 用紙の上での位置。**この伏図に表示される範囲**（命令の表示レイヤで絞った平面の
			// 広がり）の中心が、用紙のどこへ来るべきかを計算して合わせる——伏図ごとに表示する
			// ものが違えば図の中身の広がりも違うので、単に外形の中心を用紙の中心へ置くと
			// 用紙をめくるたびに建物がずれる。建物の中心（anchor）が常に同じ点へ来るよう、
			// その差だけずらした位置へ外形の中心を合わせる。
			//
			// ★**測る → データタグを置く → 動かす**の順で行う（draw/DrawUtil の
			// MoveViewportBy）。タグは注釈へ置いた実位置を測って補正する仕組みで、その実測は
			// ビューポートが用紙のどこにあるかに影響されるため、先に動かすとタグだけが
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
					delta = target - drawnCenter;
			}

			// 断面寸法データタグは**ビューポートを仕上げた後**に置く（ConfigureViewport
			// の最後が更新で、注釈はその後に追加しても図に表示される）。**ビューポートを動かす前**
			// でなければならない（上記 ★）。
			drawViewportTags(sheet.viewport, command.viewport, members, tags);
			// M31 寸法も注釈なので同じ時機に置く（**確定した縮尺**で寸法線までの距離を
			// 決める。draw/Dimension.h）。寸法は注釈の座標へそのまま置かれ、測って動かす
			// ことはしないので、ビューポートを動かす前後どちらでもよいが、収まったかの判定に
			// 含めるためここで置く。
			//
			// ★**寸法を生成する間だけ、この伏図のシートレイヤをアクティブにする**（軸組図と同じ
			// 状態）。寸法は生成した瞬間にアクティブレイヤへ入り、連続寸法へ繋ぐとき
			// （CreateChainDimension）に元の直線寸法が undo 記録つきで消える。取り消すと
			// その削除だけが戻り、**生成したときのアクティブレイヤへ直線寸法が復活する**——
			// デザインレイヤ（テンプレートに最初からある「共通」等）がアクティブだと、
			// 取り込み前からあったレイヤに不要な寸法が残った（実機の指摘。取り込み直後の図には
			// 表示されない）。このインポートが作ったシートレイヤの上で生成すれば、
			// 取り消しでレイヤごと消える（DrawUtil.h「なぜレイヤを記録するのか」）。
			// 軸組図で残らなかったのもこれによると考えている。復活する寸法を AddAfterSwapObject
			// で申告する方法（通り芯のパスの作法）は機能しなかった（実機 round 1。
			// docs/DEV-NOTES.md M31）。文字の大きさは縮尺で書いて再取得するので、
			// 1:1 のレイヤで生成しても変わらない（軸組図と同じ。draw/Dimension.h）。
			// タグはこれまでどおりの状態で生成するよう、寸法を置いたら元へ戻す。
			{
				const MCObjectHandle previous = gSDK->GetCurrentLayer();
				if (sheet.sheetLayer != nil && previous != sheet.sheetLayer)
					gSDK->SetCurrentLayer(sheet.sheetLayer);
				drawViewportDimensions(sheet.viewport, command.viewport, {},
									   document.dimensionStandard, haveContent ? layout.scale : 0.0,
									   dimensions);
				if (previous != nil && previous != sheet.sheetLayer)
					gSDK->SetCurrentLayer(previous);
			}

			// --- 収まったかは**タグを置いた後**の外形で判定する --------------------------
			//
			// 用紙に載るのは「ビューポート＋その注釈」なので、タグを置く前の外形で判定すると
			// **実際に用紙を占める大きさとは別のもの**を測っていることになる（タグも用紙
			// 基準の大きさを持つ。M29）。位置合わせ（delta）だけは上記 ★ のとおりタグを
			// 置く前の中心から決めなければならないので、測るのは 2 回になる。
			if (measured && haveContent && paper.has_value())
			{
				core::Vec2 finalCenter;
				core::Vec2 finalSize;
				const bool remeasured = MeasureViewport(sheet.viewport, finalCenter, finalSize);
				// 再測定できなければタグを置く前の実測で判定する（判定を省くよりはよい）。
				const core::Vec2 footprint = remeasured ? finalSize : drawnSize;
				// **見積もりどおりに収まったかを測って確かめる**（core/Layout.h の
				// PlanLayout::plan）。命令の座標には現れないもの（通り芯の丸など）が
				// 図に表示される分、実際の図は見積もりより大きくなりうる。
				if (footprint.x > layout.plan.width() + kFitTol ||
					footprint.y > layout.plan.height() + kFitTol)
				{
					++oversized;
					if (oversizedProbe.empty())
						oversizedProbe = DescribeFitOverflow(command.viewport.drawingNumber,
															 footprint, layout.plan.size());
				}
				// 凡例の帯へ入り込んだか。縮尺は凡例の分を引いてから決めている
				// （core/Layout.h の planLayout）ので通常は重ならないが、命令の座標に
				// 現れないもの（通り芯の丸など）の分だけ実際の図は見積もりより大きく
				// なりうる——通知せずに重ねることはせず、数えて診断へ残す。
				//
				// **動かした後の右端**で判定する。delta はタグを置く前の中心から決めてあるので、
				// 再測定した中心へそのまま加えれば、用紙の上での位置になる。
				//
				// ★**許容差（kFitTol）は緩める向きに加える**（M29）。かつてここだけ引いており
				// （`… - legendWidth - kFitTol` と比べていた）、ぴったり接した図を
				// 「重なった」と数えていた——許容差は「ぴったりの図をはみ出したと数えない」
				// ためのものなので、はみ出しの判定（上）と同じく加える側でなければならない。
				const double legendLeft = layout.legendTopRight.x - legendWidth;
				const double right =
					(remeasured ? finalCenter.x : drawnCenter.x) + delta.x + (footprint.x / 2.0);
				if (legendWidth > 0.0 && right > legendLeft + kFitTol)
				{
					++legendOverlap;
					if (legendProbe.empty())
					{
						std::array<char, 96> buffer{};
						std::snprintf(buffer.data(), buffer.size(),
									  ": 図の右端 %.1f / 凡例の左端 %.1f", right, legendLeft);
						legendProbe = command.viewport.drawingNumber + buffer.data();
					}
				}
			}

			// 用紙の上へ動かす（注釈も一緒に動く）。
			if (measured)
				MoveViewportBy(sheet.viewport, delta);
			++drawn;
		}

		// 図が仕上がったので**もう一度**中身を設定し（凡例に並ぶのはそのシートの
		// ビューポートに表示されるシンボルなので、縮尺を再設定した後の図で再取得する）、
		// 右上を揃える。
		refreshLegends(legends);

		placeLegends(legends, layout.legendTopRight);

		// M28 図面枠を置き、最背面へ移動して用紙の中心へ寄せる。**縮尺を確定させた後**で
		// なければ縮尺欄がビューポートの縮尺を取得しない（draw/TitleBlock.h）。
		finishTitleBlocks(titleBlocks);

		if (previousLayer != nil)
			gSDK->SetCurrentLayer(previousLayer);

		// 診断行は要素ごとに 1 行ずつ追加する（原因が別物なので混ぜない。連結は draw/DrawUtil の
		// AppendLine）。**異常は note、平常でも出力される内訳は outInfo** と出力先を分ける
		// （前者だけが完了ダイアログの「問題あり」に影響し、後者は診断ログにだけ出力される。
		// core::DrawCounts）。
		const auto addNote = [note](const std::string& text) { AppendLine(note, text); };
		const auto addInfo = [outInfo](const std::string& text) { AppendLine(outInfo, text); };

		// M18 割り付けの結果。**縮尺は「印刷可能領域・凡例の幅・建物の広がり」の 3 つで
		// 決まる**（M31 からは寸法の帯 band も影響するが、この行には出力していない）ので、
		// その 3 つと結果の縮尺を残す——思ったより小さい（大きい）ときに、
		// どれが影響したのかをローカル確認の場で確認できる（実際に「1/50 のはずが 1/75 に
		// なる」の切り分けで必要になった。docs/DEV-NOTES.md M18）。
		//
		// **調査のための値はここには出力しない**（DEV-NOTES「実機確認の作法」——「役目を終えた
		// 計装は消す」）。余白の生の値と単位の解釈は規約を確定するために必要だったもので、
		// 実機で確定した（図面の単位で返る）ので、**解釈できなかったときだけ**下の診断行へ
		// 出力する。はみ出し・凡例との重なりも同じく件数として下で数える。

		// 用紙まわりの長さは mm の整数で書く（下の割り付けの行と、余白の食い違いを説明する
		// 行が共有する）。
		const auto mm = [](double value) { return std::to_string(std::lround(value)); };
		if (outInfo != nullptr && paper.has_value())
		{
			std::string text = "伏図の割り付け（mm）: 用紙 " + mm(paper->paper.x) + "×" +
							   mm(paper->paper.y) + " / 印刷可能 " + mm(paper->printable.width()) +
							   "×" + mm(paper->printable.height()) + " / 凡例 " + mm(legendWidth);
			if (haveContent)
				text += " / 建物 " + mm(contentSize.x) + "×" + mm(contentSize.y) + " → 用紙上 " +
						mm(contentSize.x / layout.scale) + "×" + mm(contentSize.y / layout.scale) +
						" / 縮尺 1/" + mm(layout.scale);
			// ★**余白が四辺 0 のときだけ、その根拠を添える**（M29）。「印刷可能 ＝ 用紙」に
			// なる経路は 2 つあり——本当に縁なしの用紙設定なのか、`ISDK::GetPageMargins` が
			// 何も書かなかったのか——**出力される数字は同じ**なので、区別するには
			// 「SDK が値を書いたか」と「シートレイヤの大きさ」が要る。
			// **平常でも出力される記録なので outInfo（＝ログだけ）
			// へ出力する**（core::DrawCounts）。0 でない余白が読めているときは何も追加しない
			// （役目を終えた計装は残さない）。
			if (paper->marginsRead && paper->margins.left <= 0.0 && paper->margins.right <= 0.0 &&
				paper->margins.bottom <= 0.0 && paper->margins.top <= 0.0)
			{
				// ★**ここへ来るのは「SDK が値を書いて、それが 0 だった」ときだけ**である。
				// `marginsRead` は `SheetPaperArea` が `marginsQueried` のときにしか代入
				// しない（既定 false）ので、`marginsRead` は `marginsQueried` を含意する。
				// **読み出せなかった側はこの行では表せない**——そちらは下の
				// 「用紙の余白を解釈できなかったので…」が受け持つ。条件分岐にすると
				// 到達しない分岐が残り、両方とも到達しうると読み違える（自動レビューの指摘）。
				text += " / 余白 四辺 0（SDK は値を書いた / シートレイヤ ";
				text += paper->sheet.x > 0.0 && paper->sheet.y > 0.0
							? mm(paper->sheet.x) + "×" + mm(paper->sheet.y)
							: std::string("読めない");
				text += "）";
			}
			addInfo(text);
		}
		// 伏図のレイヤの縮尺を揃えた記録（applyPlanLayerScale）。用紙基準の記号の大きさが
		// 紙の上で一定にならないときの手掛かりで、平常でも出力されるので outInfo へ。
		if (rescaledLayers > 0)
			addInfo("伏図のデザインレイヤ " + std::to_string(rescaledLayers) +
					" 枚の縮尺を伏図に合わせた（1/" + mm(layout.scale) + "）");

		// 「命令はあるのに 0 枚」のときに、シートレイヤを生成できないのか、ビューポートを
		// 生成できないのかを切り分けられるようにする。
		const bool classesBroken = drawn > 0 && classesApplied == 0;
		// 余白を解釈できなかった（＝用紙いっぱいで割り付けた）のは異常側。生の値を添えて、
		// 単位の解釈を疑えるようにする（draw/DrawUtil の SheetPaperArea）。
		// ★**四辺 0 の用紙設定はここに来ない**（縁なし印刷ができる機種では余白 0 が実際に
		// 選べるので、0 は「余白なし」として受け取る。core::resolvePageMargins）。
		const bool marginsUnread = paper.has_value() && !paper->marginsRead;
		if (note != nullptr &&
			(missingSheetLayers > 0 || missingViewports > 0 || classesBroken ||
			 missingPlanView > 0 || missingPlacement > 0 || missingScale > 0 || oversized > 0 ||
			 legendOverlap > 0 || staleViewports > 0 || !haveContent || marginsUnread))
		{
			std::string text = "伏図の診断: ";
			AppendCount(text, "シートレイヤを作れなかった命令", missingSheetLayers, "件");
			AppendCount(text, "ビューポートを作れなかった命令", missingViewports, "件");
			if (classesBroken)
				text += "クラスを表示に戻せませんでした（対象 " +
						std::to_string(setup.classes.size()) + " クラス）。図形が映りません。";
			AppendCount(text, "2D/平面（Top/Plan）にできなかった伏図", missingPlanView, "枚",
						"3D の「上」ビューのように描かれます");
			if (!haveContent)
				text += "建物の平面の広がりが求まらないため、縮尺と位置を調整していません。";
			AppendCount(text, "用紙の上で位置を合わせられなかった伏図", missingPlacement, "枚",
						"外形を測れませんでした");
			AppendCount(text, "縮尺を当て直せなかった伏図", missingScale, "枚",
						"凡例の幅から決めた縮尺が入らず、仮の縮尺のままです");
			AppendCount(text, "測る前に描き直せなかった伏図", staleViewports, "枚",
						"収まったかの判定が当てになりません");
			// **1 枚目の実測を添える**（用紙 mm）。はみ出しが数 mm なら見積もりの不足、
			// 桁違いなら図そのものの異常——件数だけでは分かれない（M29）。
			std::string oversizedDetail = "縮尺の見積もりより図が大きくなりました";
			if (!oversizedProbe.empty())
				oversizedDetail += "。1 枚目: " + oversizedProbe;
			AppendCount(text, "用紙に収まらなかった伏図", oversized, "枚", oversizedDetail.c_str());
			std::string legendDetail = "図が広く、右上の空きへ避けきれませんでした";
			if (!legendProbe.empty())
				legendDetail += "。1 枚目: " + legendProbe;
			AppendCount(text, "凡例と重なった伏図", legendOverlap, "枚", legendDetail.c_str());
			if (marginsUnread)
			{
				const auto raw = [](double value)
				{
					std::array<char, 32> buffer{};
					std::snprintf(buffer.data(), buffer.size(), "%.3f", value);
					return std::string(buffer.data());
				};
				const core::PageMargins& margins = paper->rawMargins;
				const bool zero = margins.left <= 0.0 && margins.right <= 0.0 &&
								  margins.bottom <= 0.0 && margins.top <= 0.0;
				// 四辺 0 でここへ来る経路は 2 つしかない（core::resolvePageMargins が 0 を
				// 「余白なし」として受け取るため）——シートレイヤが用紙より小さい
				// （＝余白があるはずなのに 0 が返った）か、SDK から読み出せずに 0 のままか。
				// **どちらなのかを書き分ける**（縁なし印刷の 0 と取り違えないため）。
				const bool sheetSmaller =
					paper->sheet.x > 0.0 && paper->sheet.y > 0.0 &&
					((paper->paper.x - paper->sheet.x) > core::kPageMarginMatchTol ||
					 (paper->paper.y - paper->sheet.y) > core::kPageMarginMatchTol);
				text += "用紙の余白を解釈できなかったので、用紙いっぱいで割り付けました";
				if (zero && !paper->marginsQueried)
					text += "（SDK から余白を読み出せませんでした）。";
				else if (zero && !sheetSmaller)
					text += "（SDK は四辺 0 を返しましたが、解釈できませんでした）。";
				else
				{
					text += "（SDK が返した値: 左" + raw(margins.left) + " 右" +
							raw(margins.right) + " 下" + raw(margins.bottom) + " 上" +
							raw(margins.top) + "）。";
					if (zero)
						text += "四辺 0 ですが、シートレイヤ（" + mm(paper->sheet.x) + "×" +
								mm(paper->sheet.y) + "）が用紙（" + mm(paper->paper.x) + "×" +
								mm(paper->paper.y) +
								"）より小さいので、余白なしとは見なして"
								"いません。";
				}
			}
			addNote(text);
		}

		addNote(tagDiagnostics("伏図", tags));
		addNote(dimensionDiagnostics("伏図", dimensions));
		addInfo(dimensionInfo("伏図", dimensions));
		if (outCounts != nullptr)
			outCounts->dimensions += dimensions.chains;
		addNote(legendDiagnostics(legends));
		// M28 図面枠。異常は note、平常でも出力される内訳（適用したスタイル名・通った登録名）は
		// outInfo——出力先を分ける理由は上の割り付けの行と同じ。
		addNote(titleBlockDiagnostics(titleBlocks));
		addInfo(titleBlockInfo("伏図", titleBlocks));
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
