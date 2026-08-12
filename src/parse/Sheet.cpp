//
//	parse/Sheet.cpp
//
//	シート（伏図）の解析。Python 版 ifc/sheet.py に対応する（ROADMAP.md M13）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	どの伏図に何を映すかは「取り込んだ要素の有無」から決まる。したがってこのモジュールは
//	IFC の幾何をほとんど見ず、**他のモジュールが既に出した答え**（ストーリ一覧・柱の span・
//	横架材命令の配置先レイヤ・屋根版の有無・基礎の有無）を組み合わせるだけになる。
//	レイヤ名を自前で組み立てず parse/Story の storyLayerName を通すのも同じ理由で、
//	**ストーリがレイヤを作るときと同じ規約**でしか名前を作らない（規約がズレると、命令は
//	あるのにビューポートが空になる）。
//

#include "parse/Sheet.h"
#include "core/Document.h"
#include "parse/Context.h"
#include "parse/Footing.h"
#include "parse/Member.h"
#include "parse/Rafter.h"
#include "parse/Roof.h"
#include "parse/Story.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 伏図 1 枚の sheet 命令を組み立てる。番号・タイトルは図面タイトル／図番と同じ値を
		// 使う（Python 版と同じ: シートレイヤ番号がそのまま図番になる）。
		core::SheetCommand makeSheet(std::string number, std::string title,
									 std::vector<std::string> layers)
		{
			core::SheetCommand sheet;
			sheet.number = std::move(number);
			sheet.title = std::move(title);
			sheet.viewport.drawingNumber = sheet.number;
			sheet.viewport.drawingTitle = sheet.title;
			sheet.viewport.layers = std::move(layers);
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

		// 底盤 → 立上り → 床束 → アンカーボルト → 通り芯（Python 版 FOUNDATION_PLAN_LAYERS
		// と同じ並び。並びは重ね順ではない＝重なりはビューポートのレイヤ順が決める）。
		std::vector<std::string> layers{kLayerFoundationSlab, kLayerFoundationWall,
										kLayerFoundationFloorPost, kLayerFoundationAnchor,
										core::kGridLayer};
		std::vector<core::SheetCommand> commands;
		commands.push_back(
			makeSheet(kFoundationSheetNumber, kFoundationSheetTitle, std::move(layers)));
		return commands;
	}

	std::vector<core::SheetCommand> buildFloorFramingSheetCommands(Context& context)
	{
		const std::vector<StoryInfo> stories = context.stories();
		const std::vector<ColumnSpan> spans = collectColumnSpans(context.columns());
		const bool foundation = hasFoundation(context.model());

		std::vector<core::SheetCommand> commands;
		commands.reserve(stories.size());
		for (std::size_t i = 0; i < stories.size(); ++i)
		{
			const bool isTop = stories[i].isTop;
			// その階の横架材レイヤ（一般階＝横架材天端・最上階＝軒高）。
			std::vector<std::string> layers{
				storyLayerName(i, isTop, isTop ? kLevelEaves : kLevelBeamTop)};

			// 切断レベル（その階の床レベル + 0.25）を span が含む柱レイヤ。
			const std::vector<std::string> spanLayers =
				spanLayersAtCut(spans, static_cast<double>(i) + kFloorPlanCutOffset);
			layers.insert(layers.end(), spanLayers.begin(), spanLayers.end());

			// TODO(M13/M12): 切断位置の直下（to < 切断で最大の to）の伏図記号レイヤ
			// "{to}-柱伏図記号" もここへ足す。レイヤを作るのは M12（parse/ColumnMark）で、
			// レイヤ名の規約もそちらが持つ（ヘッダ「Python 版との差異」参照）。

			if (!isTop)
			{
				// 最下階は基礎があるときだけアンカーボルトを重ねる（土台と一緒に見たい）。
				if (i == 0 && foundation)
					layers.emplace_back(kLayerFoundationAnchor);
				layers.push_back(storyLayerName(i, isTop, kLevelFL));
			}
			layers.emplace_back(core::kGridLayer);

			std::string title = floorPlanTitle(i, isTop, stories.size());
			std::string number = std::to_string(kFloorPlanStartNumber + static_cast<int>(i));
			commands.push_back(makeSheet(std::move(number), std::move(title), std::move(layers)));
		}
		return commands;
	}

	std::vector<core::SheetCommand> buildMoyaSheetCommands(Context& context)
	{
		const std::vector<StoryInfo> stories = context.stories();
		if (stories.empty())
			return {};

		const std::vector<ColumnSpan> spans = collectColumnSpans(context.columns());
		const std::vector<core::MemberCommand>& members = context.members();

		// 番号は 基礎伏図（1）＋各階の柱梁伏図（ストーリ数）の次から。**柱梁伏図は基礎の
		// 有無に関わらず 2 から振る**ので、ここも基礎の有無に依存しない。
		const int baseNumber = kFloorPlanStartNumber + static_cast<int>(stories.size());

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
			// 登り梁はさらに稀）。parse/Story がレベルを作る条件と同じ判定。
			for (const char* levelType : {kLevelMoya, kLevelNoboribari})
			{
				const std::string layer = storyLayerName(i, isTop, levelType);
				if (anyMemberOnLayer(members, layer))
					layers.push_back(layer);
			}
			layers.push_back(storyLayerName(i, isTop, kLevelTaruki));
			layers.push_back(storyLayerName(i, isTop, kLevelNojiita));

			// 切断レベル（その階の床レベル + 0.75）を span が含む柱レイヤ＝屋根を貫いて
			// 立ち上がる主屋の柱（母屋を支える小屋束はこの切断より低いので載らない）。
			const std::vector<std::string> spanLayers =
				spanLayersAtCut(spans, static_cast<double>(i) + kMoyaPlanCutOffset);
			layers.insert(layers.end(), spanLayers.begin(), spanLayers.end());

			// TODO(M13/M12): 柱梁伏図と同じく、切断位置の直下の伏図記号レイヤを足す。
			// 母屋伏図ではこれが「母屋を支える小屋束の位置」を示す平面記号になる。

			layers.emplace_back(core::kGridLayer);

			std::string title = moyaPlanTitle(i);
			std::string number = std::to_string(baseNumber + seq);
			++seq;
			commands.push_back(makeSheet(std::move(number), std::move(title), std::move(layers)));
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
	// 捨てる＝従来どおりの挙動。CLAUDE.md「共有コンテキスト」）-----------------------
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
