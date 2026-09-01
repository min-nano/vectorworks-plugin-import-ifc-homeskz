//
//	ParseAnchorBoltTests.cpp
//
//	アンカーボルト解析（src/parse/AnchorBolt）の単体テスト。VectorWorks SDK を一切 include
//	せず、無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。
//	**期待値は手書きで持つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M11）: 型名によるボルト本体／座金の判別・座金の有無による
//	シンボル振り分け（M12／M16）・配置先レイヤ（F-アンカーボルト）・軸芯座標のセンタリング・
//	決定性・全フィクスチャの通し。実フィクスチャのパスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "parse/AnchorBolt.h"
#include "parse/Loader.h"
#include "parse/Footing.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::defaultSymbolName;
using HomeskzIfcImport::core::ImportOptions;
using HomeskzIfcImport::core::SymbolCommand;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::parse::buildAnchorBoltCommands;
using HomeskzIfcImport::parse::isAnchorBoltType;
using HomeskzIfcImport::parse::kLayerFoundationAnchor;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::resolveAnchorBoltSymbol;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// 既定のシンボル名。**唯一の定義は役割の表**（core::symbolRoles()）なので、
	// テストもそこから引く（名前を書き写すと表と食い違っても気付けない）。
	const std::string kSymbolAnchorBoltM12 = defaultSymbolName(SymbolRole::AnchorBoltM12);
	const std::string kSymbolAnchorBoltM16 = defaultSymbolName(SymbolRole::AnchorBoltM16);

	// #id を採番しながら STEP 行を溜めるだけの器（他の Parse*Tests と同じ形）。
	class StepText
	{
	public:
		int add(const std::string& body)
		{
			const int id = fNext++;
			fText += "#" + std::to_string(id) + "=" + body + ";\n";
			return id;
		}

		Model build() const
		{
			return loadIfcFromText(fText);
		}

	private:
		int fNext = 1;
		std::string fText;
	};

	std::string num(double value)
	{
		return std::to_string(value);
	}

	std::string ref(int id)
	{
		return "#" + std::to_string(id);
	}

	// IfcMechanicalFastener（ボルト本体または座金）と、その型（IfcMechanicalFastenerType）
	// を 1 組で作る。type 名がボルト本体／座金／柱頭金物の区別を担う。
	void makeFastener(StepText& step, double x, double y, const std::string& typeName)
	{
		const int location = step.add("IFCCARTESIANPOINT((" + num(x) + "," + num(y) + ",0.))");
		const int placement = step.add("IFCAXIS2PLACEMENT3D(" + ref(location) + ",$,$)");
		const int localPlacement = step.add("IFCLOCALPLACEMENT($," + ref(placement) + ")");
		const int fastener = step.add("IFCMECHANICALFASTENER('f',$,'ボルト',$,$," +
									  ref(localPlacement) + ",$,$,$,$)");
		const int type =
			step.add("IFCMECHANICALFASTENERTYPE('t',$,'" + typeName + "',$,$,$,$,$,$)");
		step.add("IFCRELDEFINESBYTYPE('d',$,$,$,(" + ref(fastener) + ")," + ref(type) + ")");
	}
} // namespace

// --- 型名からのシンボル振り分け------------------

TEST(anchor_bolt_washered_is_m12)
{
	// 座金付き（Z1/Z2 等）は M12。名前は取り込み設定が持つので、既定の設定で引く。
	const ImportOptions options;
	CHECK_EQ(resolveAnchorBoltSymbol("アンカーボルト:Z1:定着長さ:360mm", options),
			 kSymbolAnchorBoltM12);
	CHECK_EQ(resolveAnchorBoltSymbol("アンカーボルト:Z2:定着長さ:250mm", options),
			 kSymbolAnchorBoltM12);
}

TEST(anchor_bolt_washerless_is_m16)
{
	const ImportOptions options;
	CHECK_EQ(resolveAnchorBoltSymbol("アンカーボルト:座金なし:定着長さ:360mm", options),
			 kSymbolAnchorBoltM16);
}

TEST(anchor_bolt_symbol_follows_options)
{
	// 設定で差し替えた名前がそのまま出る（座金の有無の振り分けは変わらない）。
	ImportOptions options;
	options.setSymbol(SymbolRole::AnchorBoltM12, "別のボルト_M12");
	options.setSymbol(SymbolRole::AnchorBoltM16, "別のボルト_M16");
	CHECK_EQ(resolveAnchorBoltSymbol("アンカーボルト:Z1:定着長さ:360mm", options),
			 std::string("別のボルト_M12"));
	CHECK_EQ(resolveAnchorBoltSymbol("アンカーボルト:座金なし:定着長さ:360mm", options),
			 std::string("別のボルト_M16"));
}

// --- ボルト本体の判別----------------------------

TEST(anchor_bolt_type_matches)
{
	CHECK(isAnchorBoltType("アンカーボルト:Z1:定着長さ:360mm"));
}

TEST(anchor_bolt_washer_type_does_not_match)
{
	// 座金（"アンカーボルト座金:Zn"）は接頭辞 "アンカーボルト:" に一致しない＝対象外。
	// 両方を採ると同じ軸芯に二重に置かれる。
	CHECK(!isAnchorBoltType("アンカーボルト座金:Z1"));
}

TEST(anchor_bolt_empty_type_does_not_match)
{
	// 型が付いていない金物（fastenerTypeName が空文字を返す）も対象外。
	CHECK(!isAnchorBoltType(""));
	// 柱頭・柱脚金物（parse/Column が拾う別種の IfcMechanicalFastener）も対象外。
	CHECK(!isAnchorBoltType("柱頭金物:(ろ)"));
}

// --- 合成 IFC からの命令組み立て ---------------------------------------------

TEST(anchor_bolt_takes_bolt_body_only)
{
	// 同じ軸芯にボルト本体と座金の 2 要素があっても、命令は本体の 1 件だけ。
	StepText step;
	makeFastener(step, 1000.0, 2000.0, "アンカーボルト:Z1:定着長さ:360mm");
	makeFastener(step, 1000.0, 2000.0, "アンカーボルト座金:Z1");
	const Model model = step.build();

	const std::vector<SymbolCommand> bolts = buildAnchorBoltCommands(model);
	CHECK_EQ(bolts.size(), std::size_t{1});
	CHECK_EQ(bolts.front().layer, std::string(kLayerFoundationAnchor));
	CHECK_EQ(bolts.front().symbol, std::string(kSymbolAnchorBoltM12));
	// 通り芯が無いモデルなのでセンタリング補正は掛からない（生の IFC 座標のまま）。
	CHECK(near(bolts.front().position.x, 1000.0));
	CHECK(near(bolts.front().position.y, 2000.0));
	// アンカーボルトは軸対称なので回転角を持たない。
	CHECK(near(bolts.front().angle, 0.0));
}

TEST(anchor_bolt_skips_fastener_without_placement)
{
	// 配置を解決できない金物はスキップする（1 本の欠損で全体を止めない）。
	StepText step;
	const int fastener = step.add("IFCMECHANICALFASTENER('f',$,'ボルト',$,$,$,$,$,$,$)");
	const int type =
		step.add("IFCMECHANICALFASTENERTYPE('t',$,'アンカーボルト:Z1:定着長さ:360mm',$,$,$,$,$,$)");
	step.add("IFCRELDEFINESBYTYPE('d',$,$,$,(" + ref(fastener) + ")," + ref(type) + ")");

	CHECK(buildAnchorBoltCommands(step.build()).empty());
}

TEST(anchor_bolt_positions_are_centered_by_grid)
{
	// 通り芯があるモデルでは、その bbox 中心を原点へ移す補正が掛かる（横架材・柱と同じ
	// センタリング）。通り芯 (0,0)-(2000,0) / (0,0)-(0,2000) の bbox 中心は (1000, 1000)。
	StepText step;
	const int p1 = step.add("IFCCARTESIANPOINT((0.,0.))");
	const int p2 = step.add("IFCCARTESIANPOINT((2000.,0.))");
	const int p3 = step.add("IFCCARTESIANPOINT((0.,2000.))");
	const int lineX = step.add("IFCPOLYLINE((" + ref(p1) + "," + ref(p2) + "))");
	const int lineY = step.add("IFCPOLYLINE((" + ref(p1) + "," + ref(p3) + "))");
	step.add("IFCGRIDAXIS('X1'," + ref(lineX) + ",.T.)");
	step.add("IFCGRIDAXIS('Y1'," + ref(lineY) + ",.T.)");
	makeFastener(step, 1500.0, 1200.0, "アンカーボルト:座金なし:定着長さ:360mm");

	const std::vector<SymbolCommand> bolts = buildAnchorBoltCommands(step.build());
	CHECK_EQ(bolts.size(), std::size_t{1});
	CHECK_EQ(bolts.front().symbol, std::string(kSymbolAnchorBoltM16));
	CHECK(near(bolts.front().position.x, 500.0));
	CHECK(near(bolts.front().position.y, 200.0));
}

// --- 実フィクスチャ-------------------------

TEST(anchor_bolt_fixture_counts)
{
	// 伏図次郎: 84 本が座金付き（M12）、1 本が座金なし（M16）。解析を変えたときに件数が動けば
	// 気付けるようにするための固定値。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> bolts = buildAnchorBoltCommands(model);
	const auto countOf = [&bolts](const std::string& symbol)
	{
		return std::ranges::count_if(bolts, [&symbol](const SymbolCommand& bolt)
									 { return bolt.symbol == symbol; });
	};
	CHECK_EQ(countOf(kSymbolAnchorBoltM12), 84);
	CHECK_EQ(countOf(kSymbolAnchorBoltM16), 1);
	CHECK_EQ(bolts.size(), std::size_t{85});
}

TEST(anchor_bolt_fixture_positions_are_centered)
{
	// センタリング補正済みなら X は 0 をまたいで分布する。
	bool ok = false;
	const Model& model = fixture("伏図次郎【2階】.ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> bolts = buildAnchorBoltCommands(model);
	CHECK(!bolts.empty());
	double minX = bolts.front().position.x;
	double maxX = bolts.front().position.x;
	for (const SymbolCommand& bolt : bolts)
	{
		minX = std::min(minX, bolt.position.x);
		maxX = std::max(maxX, bolt.position.x);
	}
	CHECK(minX < 0.0);
	CHECK(maxX > 0.0);
}

TEST(anchor_bolt_all_fixtures_build)
{
	// 全フィクスチャで命令が出て、レイヤ・シンボル・角度が命令の規約どおりであること。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);

		const std::vector<SymbolCommand> bolts = buildAnchorBoltCommands(model);
		CHECK(!bolts.empty());
		for (const SymbolCommand& bolt : bolts)
		{
			CHECK_EQ(bolt.layer, std::string(kLayerFoundationAnchor));
			CHECK(bolt.symbol == kSymbolAnchorBoltM12 || bolt.symbol == kSymbolAnchorBoltM16);
			CHECK(near(bolt.angle, 0.0));
		}
	}
}

TEST(anchor_bolt_is_deterministic)
{
	// 同じ入力からは常に同じ並び・同じ値（byType が #id 昇順なので列挙順に依存しない）。
	bool ok = false;
	const Model& model = fixture("グレー本モデルプラン1【3階】.ifc", ok);
	CHECK(ok);

	const std::vector<SymbolCommand> first = buildAnchorBoltCommands(model);
	const std::vector<SymbolCommand> second = buildAnchorBoltCommands(model);
	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size(); ++i)
	{
		CHECK_EQ(first[i].symbol, second[i].symbol);
		CHECK(near(first[i].position.x, second[i].position.x));
		CHECK(near(first[i].position.y, second[i].position.y));
	}
}

TEST_MAIN();
