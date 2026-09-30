//
//	parse/Sheet.cpp
//
//	シート（伏図）の解析（docs/DEV-NOTES.md M13）。【SDK 非依存】ここでは VectorWorks SDK を
//	include しない。
//
//	どの伏図に何を映すかは「取り込んだ要素の有無」から決まる。したがってこのモジュールは
//	IFC の幾何をほとんど見ず、**他のモジュールが既に出した答え**（ストーリ一覧・柱の span・
//	横架材命令の配置先レイヤ・屋根版の有無・基礎の有無）を組み合わせるだけになる。
//	グラフィック凡例（M13）も同じ性格で、決めるのは「どの伏図に凡例を載せるか」だけ
//	——凡例は**スタイル無しのオブジェクト**として置き、並ぶ中身は凡例オブジェクト自身の
//	ソース定義が決める（core/Document.h の LegendCommand）。
//	レイヤ名を自前で組み立てず parse/Story の storyLayerName を通すのも同じ理由で、
//	**ストーリがレイヤを作るときと同じ規約**でしか名前を作らない（規約がズレると、命令は
//	あるのにビューポートが空になる）。
//

#include "parse/Sheet.h"
#include "parse/ShearWall.h"
#include "core/Document.h"
#include "parse/ColumnMark.h"
#include "parse/Context.h"
#include "parse/Footing.h"
#include "parse/Member.h"
#include "parse/PlanLevel.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 伏図 1 枚の sheet 命令を組み立てる。番号・タイトルは図面タイトル／図番と同じ値を使
		// う。withLegend なら、グラフィック凡例をシートレイヤに載せる（空の凡例を作らないため
		// の出し分けは呼び出し側が持つ。ヘッダ冒頭）。
		core::SheetCommand makeSheet(core::PlanKind kind, std::string number, std::string title,
									 std::vector<std::string> layers, bool withLegend = false)
		{
			core::SheetCommand sheet;
			sheet.number = std::move(number);
			sheet.title = std::move(title);
			// 伏図の種類（M31）。寸法で何を押さえるかは種類で決まる（parse/Dimension）。
			sheet.kind = kind;
			sheet.viewport.drawingNumber = sheet.number;
			sheet.viewport.drawingTitle = sheet.title;
			sheet.viewport.layers = std::move(layers);
			if (withLegend)
			{
				// 凡例は**有ることだけ**が意味を持つ（中身も置き場所も描画側が決める。
				// core/Document.h の LegendCommand）。
				sheet.legend = core::LegendCommand{};
			}
			return sheet;
		}

		// 横架材命令のうち 1 本でも layer に載っているか（parse/Story が母屋・登り梁レベルを
		// 作る条件と同じ判定）。**同じ述語を通すことで、レイヤの有無と伏図の表示レイヤが
		// 食い違わない**。
		bool anyMemberOnLayer(const std::vector<core::MemberCommand>& members,
							  const std::string& layer)
		{
			return std::ranges::any_of(members, [&layer](const core::MemberCommand& member)
									   { return member.layer == layer; });
		}
	} // namespace

	std::string floorPlanTitle(std::size_t index, bool isTop, std::size_t count)
	{
		if (isTop)
		{
			// 最上階は主屋根が架かる階番号（＝階数）を付けた "{count-1}階小屋伏図"。
			// count が 0 の呼び出しは無い（最上階がある＝ストーリが 1 つ以上ある）が、
			// 念のため下限を 1 に留めて負数の階番号を作らない。
			const std::size_t floors = count > 1 ? count - 1 : 1;
			return std::to_string(floors) + "階" + kFloorPlanRoofLabel + "伏図";
		}
		return std::to_string(index + 1) + "階" + kFloorPlanFloorLabel + "伏図";
	}

	std::string moyaPlanTitle(std::size_t index)
	{
		return std::to_string(index) + "階" + kMoyaPlanLabel + "伏図";
	}

	std::vector<std::string> spanLayersAtCut(const std::vector<ColumnSpan>& spans, double cut)
	{
		std::vector<std::string> layers;
		for (const ColumnSpan& span : spans)
		{
			if (span.from <= cut && cut <= span.to)
				layers.push_back(span.layer);
		}
		return layers;
	}

	std::vector<core::SheetCommand> buildFoundationSheetCommands(Context& context)
	{
		// 基礎が無ければ表示すべきレイヤ（"F-底盤" ほか）自体が作られないので伏図も作らない
		// （空のビューポートを残さない）。
		if (!hasFoundation(context.model()))
			return {};

		// 底盤 → 立上り → 床束 → アンカーボルト → 通り芯（並びは重ね順ではない＝重なりは
		// ビューポートのレイヤ順が決める）。
		std::vector<std::string> layers{kLayerFoundationSlab, kLayerFoundationWall,
										kLayerFoundationFloorPost, kLayerFoundationAnchor,
										core::kGridLayer};
		// グラフィック凡例は**アンカーボルトを 1 本でも置いたときだけ**載せる。凡例に並ぶのは
		// 基礎伏図に映るシンボル（＝アンカーボルト）なので、1 本も無ければ中身の無い箱が図面
		// に残るだけになる（あちらは「載せるシンボルが 1 つも無ければ空リスト」と書いていた）。
		const bool withLegend = !context.anchorBolts().empty();

		core::SheetCommand sheet = makeSheet(core::PlanKind::Foundation, kFoundationSheetNumber,
											 kFoundationSheetTitle, std::move(layers), withLegend);
		// 床付け（捨てコンクリート・砕石）は**クラスで隠す**（ご要望）。伏図は基礎の形を
		// 読む図で、床付けは要らない。レイヤでは切り分けられない（底盤の構成層・地中梁の
		// 下の床付けソリッドは底盤と同じレイヤに載る）ので、素材クラス（parse/StructuralClass）
		// で絞る。軸組図では床付けを見せたいので基礎伏図だけに挙げる。
		sheet.viewport.hiddenClasses = {CLASS_COMPONENT_LEAN_CONCRETE, CLASS_COMPONENT_GRAVEL};

		std::vector<core::SheetCommand> commands;
		commands.push_back(std::move(sheet));
		return commands;
	}

	std::vector<core::SheetCommand> buildFloorFramingSheetCommands(Context& context)
	{
		const std::vector<StoryInfo>& stories = context.stories();
		// 伏図は**横架材の高さ（伏図レベル）ごとに 1 枚**（parse/PlanLevel）。どの階も高さが
		// 1 つなら階ごとに 1 枚＝従来と同じ。
		const std::vector<PlanLevel>& planLevels = context.planLevels();
		const std::vector<ColumnSpan> spans = collectColumnSpans(context.columns());
		const std::vector<PlanMarkLayer> markLayers = collectPlanMarkLayers(spans);
		const bool foundation = hasFoundation(context.model());
		// 耐力壁レイヤは**命令があるときだけ**作られるので、載せる前に有無を確かめる
		// （空のレイヤ名をビューポートへ渡さない）。
		const std::vector<core::ShearWallCommand>& shearWalls = context.shearWalls();
		const std::vector<core::FloorCommand>& floors = context.floors();
		const std::vector<core::MemberCommand>& members = context.members();

		std::vector<core::SheetCommand> commands;
		commands.reserve(planLevels.size());
		for (const PlanLevel& level : planLevels)
		{
			const std::size_t i = level.story;
			if (i >= stories.size())
				continue;
			const StoryInfo& story = stories[i];
			const bool isTop = story.isTop;
			// その伏図レベルの横架材レイヤ（一般階＝横架材天端・最上階＝軒高。標準でない
			// 高さは "(FL-…)" の付いたレイヤ）。
			std::vector<std::string> layers{planLevelBeamLayer(level, story)};
			// 軒桁は専用レイヤに分けてある（母屋伏図に薄く重ねるため。parse/PlanLevel）ので、
			// 横架材レイヤと一緒に映す。
			if (const std::string eaves = planLevelLayer(level, story, kLevelNokigeta);
				anyMemberOnLayer(members, eaves))
				layers.push_back(eaves);

			// 切断レベル（その伏図レベルの通し番号 + 0.25）を span が含む柱レイヤ。span の
			// 番号も伏図レベルの通し番号なので（parse/Column）、この伏図の横架材の上に立つ柱
			// と、下から貫いてこの高さを通り過ぎる柱の断面が出る。
			const double cut = static_cast<double>(level.ordinal) + kFloorPlanCutOffset - 1.0;
			const std::vector<std::string> spanLayers = spanLayersAtCut(spans, cut);
			layers.insert(layers.end(), spanLayers.begin(), spanLayers.end());

			// 切断位置の**直下**の伏図記号レイヤ（M12）。その伏図が対象とする横架材の下に
			// ある柱・小屋束を平面記号で示す（例 2 階床伏図＝切断 2.25 → "2-柱伏図記号"＝
			// 1 階管柱 "1to2-柱" の平面記号）。断面記号（span レイヤ）とは排他になる。
			// 伏図レベルごとの伏図では、この高さの横架材を受ける柱だけが出る（上端が届く
			// 伏図レベルで to を決めているため。parse/Column の spanToOrdinal）。
			if (const std::string markLayer = planMarkLayerBelowCut(markLayers, cut);
				!markLayer.empty())
				layers.push_back(markLayer);

			// M19 耐力壁: **その階自身**の "n-耐力壁" レイヤを重ねる（1 階床＝土台伏図 →
			// "1-耐力壁"、2 階床伏図 → "2-耐力壁"）。伏図記号（"{to}-柱伏図記号"）が
			// 切断の**直下**を映すのとは規約が違う——耐力壁は「どの階の壁か」で呼ばれる
			// ものなので、1 階の耐力壁は 1 階の伏図（土台伏図）に、2 階の耐力壁は 2 階床
			// 伏図に出るのが図面としての読み方に合う（実機確認で決めた。M19）。耐力壁は
			// 立つ天端の伏図レベルのレイヤへ置かれている（parse/ShearWall）。
			//
			// **重ね順もこの規約に乗っている**: 同じ階のレイヤどうしなら 耐力壁レベルは
			// 横架材天端の 1 段上（前面）に積まれるので、記号が横架材の後ろへ回らない
			// （下の階のレイヤを載せていたときは、上の階の横架材に必ず隠れていた）。
			// 加えて core::desiredStoryLayerOrder が耐力壁レイヤを最前面群へ回す。
			if (const std::string shearLayer = planLevelLayer(level, story, kLevelShearWall);
				anyShearWallOnLayer(shearWalls, shearLayer))
				layers.push_back(shearLayer);

			// 登り梁は**水下側**の伏図レベルの伏図にも映す（ご要望。parse/PlanLevel が水下側の
			// 伏図レベルのレイヤへ分けてある）。母屋伏図にも従来どおり映る（下）。
			if (const std::string noboribariLayer = planLevelLayer(level, story, kLevelNoboribari);
				anyMemberOnLayer(members, noboribariLayer))
				layers.push_back(noboribariLayer);

			if (!isTop)
			{
				// 最下階は基礎があるときだけアンカーボルトを重ねる（土台と一緒に見たい）。
				if (i == 0 && foundation)
					layers.emplace_back(kLayerFoundationAnchor);
				// 床はその伏図レベルの "n-FL"。標準のレイヤは床が無くてもストーリが作るので
				// 従来どおり常に載せ、標準でない高さのレイヤは床があるときだけ（parse/Story）。
				const std::string floorLayer = planLevelLayer(level, story, kLevelFL);
				if (level.standard ||
					std::ranges::any_of(floors, [&floorLayer](const core::FloorCommand& floor)
										{ return floor.layer == floorLayer; }))
					layers.push_back(floorLayer);
			}
			layers.emplace_back(core::kGridLayer);

			// 階に伏図レベルが 2 つ以上あれば、タイトルに高さを添えて見分ける
			// （"2階床伏図（FL-872）"）。
			std::string title =
				floorPlanTitle(i, isTop, stories.size()) + planLevelTitleSuffix(planLevels, level);
			std::string number = std::to_string(kFloorPlanStartNumber + level.ordinal - 1);
			// グラフィック凡例は常に載せる（何が並ぶかは凡例オブジェクトのソース定義が決める
			// ので、ここでは中身の有無を判断できない）。
			commands.push_back(makeSheet(core::PlanKind::Framing, std::move(number),
										 std::move(title), std::move(layers), true));
		}
		return commands;
	}

	std::vector<core::SheetCommand> buildMoyaSheetCommands(Context& context)
	{
		const std::vector<StoryInfo>& stories = context.stories();
		if (stories.empty())
			return {};

		const std::vector<PlanLevel>& planLevels = context.planLevels();
		const std::vector<ColumnSpan> spans = collectColumnSpans(context.columns());
		const std::vector<PlanMarkLayer> markLayers = collectPlanMarkLayers(spans);
		const std::vector<core::MemberCommand>& members = context.members();

		// 番号は 基礎伏図（1）＋柱梁伏図（伏図レベルの数）の次から。**柱梁伏図は基礎の
		// 有無に関わらず 2 から振る**ので、ここも基礎の有無に依存しない。
		const int baseNumber = kFloorPlanStartNumber + static_cast<int>(planLevels.size());

		std::vector<core::SheetCommand> commands;
		int seq = 0;
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			// 屋根版を持つ階（最上階の主屋根・中間階の下屋根）だけに母屋伏図を作る。
			// 垂木・野地板レイヤが作られる条件（parse/Story）と同じ述語を通す。
			if (!storyHasRoofSlab(context, stories[i].id))
				continue;

			const bool isTop = stories[i].isTop;
			std::vector<std::string> layers;
			// 母屋・登り梁はその階に命令があるときだけ（下屋根は母屋を持たないこともあり、
			// 登り梁はさらに稀）。parse/Story がレベルを作る条件と同じ判定。登り梁は水下側の
			// 伏図レベルのレイヤへ分かれている（parse/PlanLevel）ので、その階の分を全部映す。
			const std::vector<const PlanLevel*> storyLevels = storyPlanLevels(planLevels, i);
			std::vector<std::string> candidates{storyLayerName(i, isTop, kLevelMoya),
												storyLayerName(i, isTop, kLevelNoboribari)};
			for (const PlanLevel* level : storyLevels)
			{
				if (!level->standard)
					candidates.push_back(planLevelLayer(*level, stories[i], kLevelNoboribari));
			}
			for (const std::string& layer : candidates)
			{
				if (anyMemberOnLayer(members, layer))
					layers.push_back(layer);
			}
			layers.push_back(storyLayerName(i, isTop, kLevelTaruki));
			layers.push_back(storyLayerName(i, isTop, kLevelNojiita));

			// 切断レベル（その階の最も高い伏図レベルの通し番号 + 0.75）を span が含む柱
			// レイヤ＝屋根を貫いて立ち上がる主屋の柱（母屋を支える小屋束はこの切断より低い
			// ので載らない）。伏図レベルの無い階（あり得ないが）は階の番号で数える。
			const double top = storyLevels.empty()
								   ? static_cast<double>(i + 1)
								   : static_cast<double>(storyLevels.back()->ordinal);
			const double cut = top + kMoyaPlanCutOffset - 1.0;
			const std::vector<std::string> spanLayers = spanLayersAtCut(spans, cut);
			layers.insert(layers.end(), spanLayers.begin(), spanLayers.end());

			// 切断位置の直下の伏図記号レイヤ（M12）。母屋伏図ではこれが「母屋を支える
			// 小屋束の位置」を示す平面記号になる（例 1 階母屋伏図＝切断 2.75 →
			// "2.5-柱伏図記号"＝下屋小屋束 "2to2.5-柱" の平面記号）。
			if (const std::string markLayer = planMarkLayerBelowCut(markLayers, cut);
				!markLayer.empty())
				layers.push_back(markLayer);

			layers.emplace_back(core::kGridLayer);

			std::string title = moyaPlanTitle(i);
			std::string number = std::to_string(baseNumber + seq);
			++seq;
			// 柱梁伏図と同じく凡例を載せる（母屋伏図に映るシンボルも同じ形で集まる）。
			core::SheetCommand sheet = makeSheet(core::PlanKind::Moya, std::move(number),
												 std::move(title), std::move(layers), true);
			// 同じ階の軒桁を薄く重ねる（ご要望: 登り梁が取り付く相手を見せる。寸法は
			// parse/Dimension がその交点を押さえる）。軒桁だけのレイヤなので、仕口・継手・
			// 小屋梁は出ない（parse/Joint・parse/Splice は横架材レイヤへ置く）。軒桁も伏図
			// レベルごとのレイヤへ分かれている（parse/PlanLevel）ので、その階の分を全部。
			std::vector<std::string> eaves{storyLayerName(i, isTop, kLevelNokigeta)};
			for (const PlanLevel* level : storyLevels)
			{
				if (!level->standard)
					eaves.push_back(planLevelLayer(*level, stories[i], kLevelNokigeta));
			}
			for (const std::string& layer : eaves)
			{
				if (anyMemberOnLayer(members, layer))
					sheet.viewport.grayedLayers.push_back(layer);
			}
			commands.push_back(std::move(sheet));
		}
		return commands;
	}

	std::vector<core::SheetCommand> buildSheetCommands(Context& context)
	{
		std::vector<core::SheetCommand> commands = buildFoundationSheetCommands(context);
		for (core::SheetCommand& sheet : buildFloorFramingSheetCommands(context))
			commands.push_back(std::move(sheet));
		for (core::SheetCommand& sheet : buildMoyaSheetCommands(context))
			commands.push_back(std::move(sheet));
		return commands;
	}

	// --- const Model& を直接取るオーバーロード（単体テスト用。内部でコンテキストを作って
	// 捨てる＝従来どおりの挙動。docs/DEVELOPMENT.md「置き場所の一覧」の共有コンテキスト）--
	std::vector<core::SheetCommand> buildFoundationSheetCommands(const Model& model)
	{
		Context context(model);
		return buildFoundationSheetCommands(context);
	}

	std::vector<core::SheetCommand> buildFloorFramingSheetCommands(const Model& model)
	{
		Context context(model);
		return buildFloorFramingSheetCommands(context);
	}

	std::vector<core::SheetCommand> buildMoyaSheetCommands(const Model& model)
	{
		Context context(model);
		return buildMoyaSheetCommands(context);
	}

	std::vector<core::SheetCommand> buildSheetCommands(const Model& model)
	{
		Context context(model);
		return buildSheetCommands(context);
	}
} // namespace HomeskzIfcImport::parse
