//
//	parse/AnchorBolt.cpp
//
//	アンカーボルト解析の実装。【SDK 非依存】ここでは VectorWorks SDK を include しない
//	（core/parse のみ依存）。
//

#include "parse/AnchorBolt.h"
#include "core/ImportOptions.h"
#include "parse/Column.h"
#include "parse/Context.h"
#include "parse/Footing.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::SymbolCommand;
	using core::Vec2;

	bool isAnchorBoltType(const std::string& typeName)
	{
		return typeName.starts_with(kAnchorBoltTypePrefix);
	}

	std::string resolveAnchorBoltSymbol(const std::string& typeName,
										const core::ImportOptions& options)
	{
		// 座金なし（型名に "座金なし" を含む）は M16 の役割、そうでなければ（Z1/Z2 等の
		// 角座金付き）M12 の役割。**名前そのものは取り込み設定が持つ**（既定は従来と同じ
		// "アンカーボルト_M16" / "アンカーボルト_M12"）。
		if (typeName.find(kWasherlessToken) != std::string::npos)
			return options.symbol(core::SymbolRole::AnchorBoltM16);
		return options.symbol(core::SymbolRole::AnchorBoltM12);
	}

	std::vector<SymbolCommand> buildAnchorBoltCommands(Context& context)
	{
		const Model& model = context.model();

		// 通り芯と同じセンタリングオフセット（通り芯が無ければ (0,0)＝生の IFC 座標）。
		const Vec2 center = context.gridCenter();

		std::vector<SymbolCommand> commands;
		for (const int elementId : model.byType("IFCMECHANICALFASTENER"))
		{
			const Entity* element = model.entity(elementId);
			if (element == nullptr)
				continue;

			// 型（IfcMechanicalFastenerType）の名前でボルト本体／座金／柱頭・柱脚金物を
			// 見分ける。本体以外はここで落ちる（座金を採ると同じ軸芯に二重で置かれる）。
			const std::string typeName = fastenerTypeName(model, *element);
			if (!isAnchorBoltType(typeName))
				continue;

			// 軸芯の平面座標。柱と同じ「ローカル配置 Location の XY」の読み方
			// （parse/Column の columnPosition2D）で、同じ座標系・同じセンタリングに乗る。
			Vec2 position;
			if (!columnPosition2D(model, *element, position))
				continue;

			SymbolCommand command;
			command.layer = kLayerFoundationAnchor;
			command.symbol = resolveAnchorBoltSymbol(typeName, context.options());
			command.position = position - center;
			// 回転角は持たない（ボルトは軸対称）。SymbolCommand::angle の既定 0 のまま。
			commands.push_back(std::move(command));
		}
		return commands;
	}

	std::vector<SymbolCommand> buildAnchorBoltCommands(const Model& model)
	{
		Context context(model);
		return buildAnchorBoltCommands(context);
	}
} // namespace HomeskzIfcImport::parse
