//
//	ParseColumnTests.cpp
//
//	柱解析（src/parse/Column）の単体テスト。VectorWorks SDK を一切 include せず、
//	無 SDK のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」:
//	core/ parse/ は無 SDK で単体テスト）。Python 版 test_ifc_column.py と
//	test_ifc_column_span.py のケースを 1 対 1 で写している（期待値は手書き。
//	docs/DEV-NOTES.md「Python 版出力との比較はしない」）。
//
//	検証項目（docs/DEV-NOTES.md M8）: 配置座標と断面の抽出・柱種別／構造材 ID・柱頭/柱脚金物の
//	対応付け・**span（またぐレベル区間）の to レベル判定**（管柱／通し柱／屋根束）・
//	span レイヤへの振り分けと base ごとのまとめ・上下端のストーリバウンド（柱＝当階と上階、
//	小屋束＝当階のみ。**差は常に柱高さ**）・構造用途・クラス割り当て・**小屋束の断面を直上の
//	母屋幅へ合わせる補正**・センタリング・決定性・全フィクスチャの通し。実フィクスチャの
//	パスは CMake が HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "parse/Column.h"
#include "parse/Loader.h"
#include "parse/Member.h"
#include "parse/Story.h"
#include "parse/StructuralClass.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::ColumnCommand;
using HomeskzIfcImport::core::MemberCommand;
using HomeskzIfcImport::core::StoryBoundCommand;
using HomeskzIfcImport::core::Vec2;
using HomeskzIfcImport::parse::buildColumnCommands;
using HomeskzIfcImport::parse::CLASS_KOYAZUKA;
using HomeskzIfcImport::parse::CLASS_KUDABASHIRA;
using HomeskzIfcImport::parse::CLASS_MOYA;
using HomeskzIfcImport::parse::CLASS_NOBORIBARI;
using HomeskzIfcImport::parse::CLASS_NOKIGETA;
using HomeskzIfcImport::parse::CLASS_TOSHIBASHIRA;
using HomeskzIfcImport::parse::collectColumnLayersByStory;
using HomeskzIfcImport::parse::collectColumnSpans;
using HomeskzIfcImport::parse::columnHardwareSpec;
using HomeskzIfcImport::parse::columnPosition2D;
using HomeskzIfcImport::parse::ColumnSpan;
using HomeskzIfcImport::parse::Entity;
using HomeskzIfcImport::parse::isThroughColumn;
using HomeskzIfcImport::parse::kSpanLevelTol;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::makeColumnMemberId;
using HomeskzIfcImport::parse::memberWidthOnTop;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::resolveColumnToLevel;
using HomeskzIfcImport::parse::resolveColumnType;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::near;

namespace
{
	// -----------------------------------------------------------------------
	// 合成 IFC（STEP テキスト）の組み立て。Python 版 test_ifc_column.py の
	// make_storey / make_column / make_hardware / make_grid_axis に対応する。
	// -----------------------------------------------------------------------

	// #id を採番しながら STEP 行を溜めるだけの器（ParseMemberTests と同じ形）。
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

	int point3(StepText& step, double x, double y, double z)
	{
		return step.add("IFCCARTESIANPOINT((" + num(x) + "," + num(y) + "," + num(z) + "))");
	}

	int point2(StepText& step, double x, double y)
	{
		return step.add("IFCCARTESIANPOINT((" + num(x) + "," + num(y) + "))");
	}

	// IfcBuildingStorey(GlobalId, OwnerHistory, Name=2, …, Elevation=9)。
	int makeStorey(StepText& step, const std::string& name, double elevation)
	{
		return step.add("IFCBUILDINGSTOREY('s',$,'" + name + "',$,$,$,$,$,.ELEMENT.," +
						num(elevation) + ")");
	}

	// 要素を階へ所属させる（IfcRelContainedInSpatialStructure）。
	void contain(StepText& step, int storey, int element)
	{
		step.add("IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(" + ref(element) + ")," +
				 ref(storey) + ")");
	}

	// 柱（Python 版 make_column）。押し出しは局所 Z 方向で、Depth が柱高さ。
	struct ColumnSpec
	{
		double ox = 0.0;
		double oy = 0.0;
		double oz = 0.0;
		double width = 105.0;
		double depth = 105.0;
		double height = 2844.0;
		std::string objectType; // 空文字なら $（未設定＝管柱）
		std::string name;		// 空文字なら $（未設定）
		bool withGeometry = true; // false なら配置・形状を持たない柱（スキップされる）
	};

	int makeColumn(StepText& step, int storey, const ColumnSpec& spec)
	{
		std::string placementRef = "$";
		std::string shapeRef = "$";
		if (spec.withGeometry)
		{
			const int location = point3(step, spec.ox, spec.oy, spec.oz);
			const int placement = step.add("IFCAXIS2PLACEMENT3D(" + ref(location) + ",$,$)");
			placementRef = ref(step.add("IFCLOCALPLACEMENT($," + ref(placement) + ")"));

			const int profile = step.add("IFCRECTANGLEPROFILEDEF(.AREA.,$,$," + num(spec.width) +
										 "," + num(spec.depth) + ")");
			const int extrudeDir = step.add("IFCDIRECTION((0.,0.,1.))");
			const int solid = step.add("IFCEXTRUDEDAREASOLID(" + ref(profile) + ",$," +
									   ref(extrudeDir) + "," + num(spec.height) + ")");
			const int shape =
				step.add("IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(" + ref(solid) + "))");
			shapeRef = ref(step.add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + ref(shape) + "))"));
		}

		const std::string name = spec.name.empty() ? std::string("$") : "'" + spec.name + "'";
		const std::string objectType =
			spec.objectType.empty() ? std::string("$") : "'" + spec.objectType + "'";
		const int column = step.add("IFCCOLUMN('c',$," + name + ",$," + objectType + "," +
									placementRef + "," + shapeRef + ",$)");
		contain(step, storey, column);
		return column;
	}

	// 柱頭／柱脚金物（Python 版 make_hardware）。Name に "柱頭金物" / "柱脚金物" を含めると
	// その種別として扱われ、型（IfcMechanicalFastenerType）の名前が仕様になる。
	void makeHardware(StepText& step, int storey, double ox, double oy, const std::string& name,
					  const std::string& typeName)
	{
		const int location = point3(step, ox, oy, 0.0);
		const int placement = step.add("IFCAXIS2PLACEMENT3D(" + ref(location) + ",$,$)");
		const int localPlacement = step.add("IFCLOCALPLACEMENT($," + ref(placement) + ")");
		const int fastener = step.add("IFCMECHANICALFASTENER('f',$,'" + name + "',$,$," +
									  ref(localPlacement) + ",$,$,$,$)");
		const int type =
			step.add("IFCMECHANICALFASTENERTYPE('t',$,'" + typeName + "',$,$,$,$,$,$)");
		step.add("IFCRELDEFINESBYTYPE('d',$,$,$,(" + ref(fastener) + ")," + ref(type) + ")");
		contain(step, storey, fastener);
	}

	// 通り芯（センタリング中心の算出に使う）。
	void makeGridAxis(StepText& step, const std::string& tag, double x1, double y1, double x2,
					  double y2)
	{
		const int a = point2(step, x1, y1);
		const int b = point2(step, x2, y2);
		const int polyline = step.add("IFCPOLYLINE((" + ref(a) + "," + ref(b) + "))");
		step.add("IFCGRIDAXIS('" + tag + "'," + ref(polyline) + ",.T.)");
	}

	// -----------------------------------------------------------------------
	// 命令レベルのテスト用ヘルパー（Python 版 _top_member / _column に対応）
	// -----------------------------------------------------------------------

	// 小屋束の上に乗る横架材命令（母屋等）。topZ は天端の絶対 Z で、断面下端は topZ − height。
	struct TopMemberSpec
	{
		double width = 90.0;
		Vec2 start;
		Vec2 end;
		double topZ = 0.0;
		double endTopZ = 0.0; // 0 なら topZ と同じ（水平材）
		double height = 90.0;
		std::string memberClass = CLASS_MOYA;
		std::string layer = "R-母屋";
	};

	MemberCommand topMember(const TopMemberSpec& spec)
	{
		MemberCommand command;
		command.layer = spec.layer;
		command.memberId = "m";
		command.drawClass = spec.memberClass;
		command.start = spec.start;
		command.end = spec.end;
		command.width = spec.width;
		command.height = spec.height;
		command.elevation = spec.topZ;
		command.endElevation = spec.endTopZ == 0.0 ? spec.topZ : spec.endTopZ;
		command.startBound = StoryBoundCommand{0, "母屋", 0.0};
		command.endBound = StoryBoundCommand{0, "母屋", 0.0};
		return command;
	}

	// span の列挙だけを見るテスト用の最小 column 命令（Python 版 _column）。
	ColumnCommand columnOnLayer(const std::string& layer)
	{
		ColumnCommand command;
		command.layer = layer;
		return command;
	}

	// 文字列ベクタの一致。
	bool sameVec(const std::vector<std::string>& a, const std::vector<std::string>& b)
	{
		return a == b;
	}
} // namespace

// ---------------------------------------------------------------------------
// columnPosition2D（Python 版 _get_position_2d）
// ---------------------------------------------------------------------------

TEST(position_extracts_origin)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	{
		ColumnSpec spec;
		spec.ox = 1000.0;
		spec.oy = 2000.0;
		spec.oz = -174.0;
		makeColumn(step, storey, spec);
	}
	const Model model = step.build();

	const std::vector<int> columns = model.byType("IFCCOLUMN");
	CHECK_EQ(columns.size(), std::size_t(1));
	if (columns.empty())
		return;
	Vec2 position;
	CHECK(columnPosition2D(model, *model.entity(columns.front()), position));
	CHECK(near(position.x, 1000.0));
	CHECK(near(position.y, 2000.0));
}

TEST(position_false_for_malformed_placements)
{
	// ObjectPlacement が IfcLocalPlacement でない／RelativePlacement が
	// IfcAxis2Placement3D でない／Location が無い／座標が 2 要素に満たない、
	// のいずれでも取れない。
	const std::vector<std::string> texts = {
		"#1=IFCAXIS2PLACEMENT3D($,$,$);\n"
		"#2=IFCCOLUMN('c',$,$,$,$,#1,$,$);\n",
		"#1=IFCAXIS2PLACEMENT2D($,$);\n"
		"#2=IFCLOCALPLACEMENT($,#1);\n"
		"#3=IFCCOLUMN('c',$,$,$,$,#2,$,$);\n",
		"#1=IFCAXIS2PLACEMENT3D($,$,$);\n"
		"#2=IFCLOCALPLACEMENT($,#1);\n"
		"#3=IFCCOLUMN('c',$,$,$,$,#2,$,$);\n",
		// 座標が 1 要素しかない（平面座標にならない）。
		"#1=IFCCARTESIANPOINT((0.));\n"
		"#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
		"#3=IFCLOCALPLACEMENT($,#2);\n"
		"#4=IFCCOLUMN('c',$,$,$,$,#3,$,$);\n",
	};
	for (const std::string& text : texts)
	{
		const Model model = loadIfcFromText(text);
		const std::vector<int> columns = model.byType("IFCCOLUMN");
		CHECK_EQ(columns.size(), std::size_t(1));
		if (columns.empty())
			continue;
		Vec2 position;
		CHECK(!columnPosition2D(model, *model.entity(columns.front()), position));
	}
}

TEST(position_false_when_no_placement)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	{
		ColumnSpec spec;
		spec.withGeometry = false;
		makeColumn(step, storey, spec);
	}
	const Model model = step.build();

	const std::vector<int> columns = model.byType("IFCCOLUMN");
	CHECK_EQ(columns.size(), std::size_t(1));
	if (columns.empty())
		return;
	Vec2 position;
	CHECK(!columnPosition2D(model, *model.entity(columns.front()), position));
}

// ---------------------------------------------------------------------------
// resolveColumnType / columnHardwareSpec / makeColumnMemberId
// ---------------------------------------------------------------------------

TEST(column_type_unset_is_kudabashira)
{
	CHECK_EQ(resolveColumnType(""), std::string("管柱"));
}

TEST(column_type_standcolumn_is_koyazuka)
{
	CHECK_EQ(resolveColumnType("STANDCOLUMN"), std::string("小屋束"));
}

TEST(column_type_unknown_falls_back_to_default)
{
	CHECK_EQ(resolveColumnType("SOMETHING_ELSE"), std::string("管柱"));
}

TEST(hardware_spec_returns_type_name_unprocessed)
{
	// 型名は加工せず（コロン分割せず）そのまま仕様文字列になる。
	CHECK_EQ(columnHardwareSpec("柱頭金物:(ろ)"), std::string("柱頭金物:(ろ)"));
	CHECK_EQ(columnHardwareSpec("柱脚金物:C12"), std::string("柱脚金物:C12"));
	CHECK_EQ(columnHardwareSpec("HD-B20"), std::string("HD-B20"));
	CHECK_EQ(columnHardwareSpec(""), std::string(""));
}

TEST(member_id_section_and_type_without_hardware)
{
	CHECK_EQ(makeColumnMemberId(105.0, 105.0, "管柱", "", ""), std::string("105×105 - 管柱"));
}

TEST(member_id_rounds_dimensions)
{
	CHECK_EQ(makeColumnMemberId(104.6, 120.4, "管柱", "", ""), std::string("105×120 - 管柱"));
}

TEST(member_id_appends_both_hardware)
{
	CHECK_EQ(makeColumnMemberId(105.0, 105.0, "管柱", "柱頭金物:(ろ)", "柱脚金物:(い)"),
			 std::string("105×105 - 管柱 / 柱頭金物:(ろ) / 柱脚金物:(い)"));
}

TEST(member_id_appends_only_present_hardware)
{
	CHECK_EQ(makeColumnMemberId(105.0, 105.0, "小屋束", "柱頭金物:(ろ)", ""),
			 std::string("105×105 - 小屋束 / 柱頭金物:(ろ)"));
}

// ---------------------------------------------------------------------------
// memberWidthOnTop（小屋束の直上に乗る材の幅。Python 版 _member_width_on_top）
// ---------------------------------------------------------------------------

TEST(width_on_top_returns_width_of_member_resting_on_top)
{
	// 小屋束上端 7000 に下端（7090 − 90）が接する母屋の幅を返す。
	TopMemberSpec spec;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 7090.0;
	const MemberCommand moya = topMember(spec);
	const std::optional<double> width = memberWidthOnTop(0.0, 0.0, 7000.0, {moya});
	CHECK(width.has_value());
	CHECK(near(width.value_or(0.0), 90.0));
}

TEST(width_on_top_none_when_no_member)
{
	CHECK(!memberWidthOnTop(0.0, 0.0, 7000.0, {}).has_value());
}

TEST(width_on_top_ignores_non_roof_top_member)
{
	// 母屋・棟木・登り梁以外（軒桁等）は対象にしない。
	TopMemberSpec spec;
	spec.width = 105.0;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 7090.0;
	spec.memberClass = CLASS_NOKIGETA;
	spec.layer = "R-軒高";
	const MemberCommand girder = topMember(spec);
	CHECK(!memberWidthOnTop(0.0, 0.0, 7000.0, {girder}).has_value());
}

TEST(width_on_top_matches_member_pierced_by_post)
{
	// 小屋束が母屋を貫いて天端付近まで伸びる（棟束）場合も拾う。母屋の Z 範囲
	// [6754, 6859] に小屋束上端 6861（天端 +2mm）が収まる。
	TopMemberSpec spec;
	spec.width = 105.0;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 6859.0;
	spec.height = 105.0;
	const MemberCommand moya = topMember(spec);
	const std::optional<double> width = memberWidthOnTop(0.0, 0.0, 6861.0, {moya});
	CHECK(width.has_value());
	CHECK(near(width.value_or(0.0), 105.0));
}

TEST(width_on_top_ignores_degenerate_member)
{
	// 平面投影長が 0 の材（始端＝終端）は軸が定まらないので対象にしない。
	TopMemberSpec spec;
	spec.start = Vec2{0.0, 0.0};
	spec.end = Vec2{0.0, 0.0};
	spec.topZ = 7090.0;
	const MemberCommand degenerate = topMember(spec);
	CHECK(!memberWidthOnTop(0.0, 0.0, 7000.0, {degenerate}).has_value());
}

TEST(width_on_top_ignores_member_far_below)
{
	TopMemberSpec spec;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 5000.0;
	const MemberCommand low = topMember(spec);
	CHECK(!memberWidthOnTop(0.0, 0.0, 7000.0, {low}).has_value());
}

TEST(width_on_top_ignores_member_off_to_the_side)
{
	// 平面上、小屋束 (0,0) が母屋の軸（y=500〜1500）から大きく外れる。
	TopMemberSpec spec;
	spec.start = Vec2{0.0, 500.0};
	spec.end = Vec2{0.0, 1500.0};
	spec.topZ = 7090.0;
	const MemberCommand moya = topMember(spec);
	CHECK(!memberWidthOnTop(0.0, 0.0, 7000.0, {moya}).has_value());
}

TEST(width_on_top_prefers_member_closest_to_top)
{
	// 複数候補があれば下端が小屋束上端に最も近い材を選ぶ。
	TopMemberSpec spec;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 7090.0;
	const MemberCommand nearMember = topMember(spec);
	// 下端 6990（小屋束上端 7000 から 10mm 下）。
	TopMemberSpec farSpec;
	farSpec.width = 120.0;
	farSpec.start = Vec2{-1000.0, 0.0};
	farSpec.end = Vec2{1000.0, 0.0};
	farSpec.topZ = 7110.0;
	farSpec.height = 120.0;
	const MemberCommand farMember = topMember(farSpec);
	const std::optional<double> width = memberWidthOnTop(0.0, 0.0, 7000.0, {farMember, nearMember});
	CHECK(width.has_value());
	CHECK(near(width.value_or(0.0), 90.0));
}

TEST(width_on_top_interpolates_sloped_noboribari)
{
	// 始端 (−1000,0) 天端 6000 → 終端 (1000,0) 天端 8000 の傾斜梁（せい 90）。中央 (0,0) の
	// 天端は補間で 7000、下端 6910。小屋束上端 6950 は範囲内。
	TopMemberSpec spec;
	spec.width = 120.0;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 6000.0;
	spec.endTopZ = 8000.0;
	spec.memberClass = CLASS_NOBORIBARI;
	spec.layer = "R-登り梁";
	const MemberCommand nobori = topMember(spec);
	const std::optional<double> width = memberWidthOnTop(0.0, 0.0, 6950.0, {nobori});
	CHECK(width.has_value());
	CHECK(near(width.value_or(0.0), 120.0));
}

// ---------------------------------------------------------------------------
// isThroughColumn / resolveColumnToLevel（span の to レベル判定）
// ---------------------------------------------------------------------------

TEST(through_column_false_without_upper_story)
{
	CHECK(!isThroughColumn(9999.0, std::nullopt));
}

TEST(through_column_true_when_piercing_next_floor)
{
	// 次階 FL 3500 を許容値（100mm）より高く超える → 通し柱。
	CHECK(isThroughColumn(3700.0, std::optional<double>(3500.0)));
	CHECK(!isThroughColumn(3550.0, std::optional<double>(3500.0)));
}

namespace
{
	// 各階の横架材（床梁）下端・天端。index は 0 起点で base 階の上の階を参照する。
	// 天端は下端より梁背ぶん上。最上階（index 2）の天端は軒高。
	const std::vector<double> kBottoms = {590.0, 3165.0, 6120.0};
	const std::vector<double> kTops = {830.0, 3405.0, 6400.0};
} // namespace

TEST(to_level_kudabashira_reaches_next_floor)
{
	// 1 階管柱: 上端が 2 階梁下端（3165）以上・2 階梁天端（3405）未満 → 次階＝to 2。
	CHECK(near(resolveColumnToLevel(0, 3300.0, kBottoms, kTops), 2.0));
}

TEST(to_level_roof_post_does_not_reach_next_floor)
{
	// 下屋の小屋束: 上端が直上階の梁下端（3165）未満 → 届かず from + 0.5（base＝2 階）。
	CHECK(near(resolveColumnToLevel(1, 4000.0, kBottoms, kTops), 2.5));
}

TEST(to_level_through_column_reaches_two_floors_up)
{
	// 通し柱（1・2 階）: 上端が屋根梁下端（6120）以上・軒高（6400）未満 → 3 階床＝to 3。
	CHECK(near(resolveColumnToLevel(0, 6200.0, kBottoms, kTops), 3.0));
}

TEST(to_level_top_story_column_is_roof_post)
{
	// 最上階の柱（主屋根束）: 上に階が無いため from + 0.5。
	CHECK(near(resolveColumnToLevel(2, 7000.0, kBottoms, kTops), 3.5));
}

TEST(to_level_tolerance_counts_top_just_below_bottom_as_reached)
{
	// 下端よりわずか（許容値内）下でも到達とみなす。
	const double top = kBottoms[1] - (kSpanLevelTol / 2.0);
	CHECK(near(resolveColumnToLevel(0, top, kBottoms, kTops), 2.0));
}

TEST(to_level_roof_post_above_eaves_is_half_level)
{
	// 2 階建て（1 階=0・屋根=1）。1 階に立つ小屋束で上端が軒高（屋根の横架材天端 3300）より
	// 高い → 屋根軒高の梁下端（3165）に達しても管柱ではなく屋根束 → 1to2.5。
	const std::vector<double> bottoms = {590.0, 3165.0};
	const std::vector<double> tops = {830.0, 3300.0}; // tops[1] = 軒高
	CHECK(near(resolveColumnToLevel(0, 3500.0, bottoms, tops), 2.5));
}

TEST(to_level_column_reaching_top_story_at_eaves_stays_integer)
{
	// 対照: 上端が軒高（3300）以下で屋根軒高の梁下端に止まる柱は管柱扱いで to 2。
	const std::vector<double> bottoms = {590.0, 3165.0};
	const std::vector<double> tops = {830.0, 3300.0};
	CHECK(near(resolveColumnToLevel(0, 3200.0, bottoms, tops), 2.0));
}

// ---------------------------------------------------------------------------
// collectColumnSpans / collectColumnLayersByStory
// ---------------------------------------------------------------------------

TEST(collect_spans_distinct_and_sorted)
{
	const std::vector<ColumnCommand> columns = {
		columnOnLayer("2to3-柱"), columnOnLayer("1to2-柱"), columnOnLayer("2to2.5-柱"),
		columnOnLayer("2to3-柱"), columnOnLayer("3to3.5-柱")};

	const std::vector<ColumnSpan> spans = collectColumnSpans(columns);
	CHECK_EQ(spans.size(), std::size_t(4));
	if (spans.size() != 4)
		return;
	CHECK(near(spans[0].from, 1.0) && near(spans[0].to, 2.0));
	CHECK_EQ(spans[0].layer, std::string("1to2-柱"));
	CHECK(near(spans[1].from, 2.0) && near(spans[1].to, 2.5));
	CHECK_EQ(spans[1].layer, std::string("2to2.5-柱"));
	CHECK(near(spans[2].from, 2.0) && near(spans[2].to, 3.0));
	CHECK_EQ(spans[2].layer, std::string("2to3-柱"));
	CHECK(near(spans[3].from, 3.0) && near(spans[3].to, 3.5));
	CHECK_EQ(spans[3].layer, std::string("3to3.5-柱"));
}

TEST(collect_spans_ignores_non_span_layers)
{
	CHECK(collectColumnSpans({columnOnLayer("R-軒高")}).empty());
}

TEST(collect_layers_by_story_groups_by_base_index)
{
	const std::vector<ColumnCommand> columns = {
		columnOnLayer("1to2-柱"), columnOnLayer("2to2.5-柱"), columnOnLayer("2to3-柱"),
		columnOnLayer("3to3.5-柱")};
	const std::map<int, std::vector<std::string>> byStory = collectColumnLayersByStory(columns);
	CHECK_EQ(byStory.size(), std::size_t(3));
	CHECK(sameVec(byStory.at(0), {"1to2-柱"}));
	CHECK(sameVec(byStory.at(1), {"2to2.5-柱", "2to3-柱"}));
	CHECK(sameVec(byStory.at(2), {"3to3.5-柱"}));
}

// ---------------------------------------------------------------------------
// buildColumnCommands（合成 IFC）
// ---------------------------------------------------------------------------

TEST(build_empty_ifc_returns_empty)
{
	CHECK(buildColumnCommands(loadIfcFromText("")).empty());
}

TEST(build_command_per_column)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	{
		ColumnSpec spec;
		makeColumn(step, storey, spec);
	}
	{
		ColumnSpec spec;
		spec.ox = 1000.0;
		makeColumn(step, storey, spec);
	}
	makeStorey(step, "RFL", 6300.0);

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(2));
	for (const ColumnCommand& command : commands)
	{
		// 柱は span レイヤ "{from}to{to}-柱" に置く。base（1 階＝1 始まりレベル 1）起点
		// なので from=1。
		CHECK(command.layer.starts_with("1to"));
		CHECK(command.layer.ends_with("-柱"));
		CHECK_EQ(command.memberId, std::string("105×105 - 管柱"));
	}
}

TEST(build_top_story_column_uses_half_level_span)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.oz = -100.0;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	// 単一ストーリ＝レベル 1（1 始まり）。上に階が無いため屋根束扱いで 1to1.5。
	CHECK_EQ(commands[0].layer, std::string("1to1.5-柱"));
	// 下端高さ ＝ ストーリ高さ + ローカル Z。
	CHECK(near(commands[0].elevation, 6200.0));
}

TEST(build_column_binds_bottom_current_top_upper_floor)
{
	// 柱（管柱・通し柱）は下端を当階、上端を上階の横架材天端へバインドする。この IFC には
	// 柱以外に負の配置 Z を持つ要素が無いので横架材天端オフセットは 0（＝ FL 高さ）。
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "2FL", 3500.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.height = 2718.0;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	const ColumnCommand& command = commands[0];
	CHECK(near(command.elevation, 600.0));
	CHECK(near(command.height, 2718.0));
	// 下端は当階（storyOffset=0）の横架材天端、offset = 600 − 600 = 0。
	CHECK_EQ(command.bottomBound.storyOffset, 0);
	CHECK_EQ(command.bottomBound.level, std::string("横架材天端"));
	CHECK(near(command.bottomBound.offset, 0.0));
	// 上端は上階（storyOffset=1）の横架材天端、offset = (600+2718) − 3500。
	CHECK_EQ(command.topBound.storyOffset, 1);
	CHECK_EQ(command.topBound.level, std::string("横架材天端"));
	CHECK(near(command.topBound.offset, 600.0 + 2718.0 - 3500.0));
}

TEST(build_koyazuka_binds_both_ends_to_current_eaves)
{
	// 小屋束は上下端とも当階の横架材天端（最上階は軒高）へバインドし、offset にはそれぞれ
	// 実際の下端／上端 Z までの距離を入れる（＝**バウンドの差が柱高さ**になる。同値にすると
	// 高さ 0 の柱になる。parse/Column.h の「Python 版との差異」）。
	StepText step;
	makeStorey(step, "1FL", 600.0);
	const int storey = makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.oz = -100.0;
		spec.height = 800.0;
		spec.objectType = "STANDCOLUMN";
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	const ColumnCommand& command = commands[0];
	CHECK_EQ(command.bottomBound.storyOffset, 0);
	CHECK_EQ(command.bottomBound.level, std::string("軒高"));
	CHECK(near(command.bottomBound.offset, -100.0));
	CHECK_EQ(command.topBound.storyOffset, 0);
	CHECK_EQ(command.topBound.level, std::string("軒高"));
	// 下端 6300 − 100 = 6200、上端 6200 + 800 = 7000 → 軒高 6300 からの距離は 700。
	CHECK(near(command.topBound.offset, 700.0));
	CHECK(near(command.topBound.offset - command.bottomBound.offset, command.height));
}

TEST(build_assigns_span_layer_per_story)
{
	StepText step;
	const int first = makeStorey(step, "1FL", 600.0);
	const int second = makeStorey(step, "2FL", 3500.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		makeColumn(step, first, spec);
	}
	{
		ColumnSpec spec;
		makeColumn(step, second, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(2));
	CHECK(std::ranges::any_of(commands,
							  [](const ColumnCommand& c) { return c.layer.starts_with("1to"); }));
	CHECK(std::ranges::any_of(commands,
							  [](const ColumnCommand& c) { return c.layer.starts_with("2to"); }));
}

TEST(build_elevation_is_story_plus_local_z)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.oz = -174.0;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(near(commands[0].elevation, 426.0));
}

TEST(build_applies_grid_center_offset)
{
	StepText step;
	// 通り芯 X=0〜2000・Y=0〜2000 → 中心 (1000, 1000)。柱 (1500, 1500) → (500, 500)。
	makeGridAxis(step, "X1", 0.0, 0.0, 2000.0, 0.0);
	makeGridAxis(step, "Y1", 0.0, 0.0, 0.0, 2000.0);
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.ox = 1500.0;
		spec.oy = 1500.0;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(near(commands[0].position.x, 500.0));
	CHECK(near(commands[0].position.y, 500.0));
}

TEST(build_sets_dimensions)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.width = 105.0;
		spec.depth = 120.0;
		spec.height = 2844.0;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(near(commands[0].width, 105.0));
	CHECK(near(commands[0].depth, 120.0));
	CHECK(near(commands[0].height, 2844.0));
}

TEST(build_standcolumn_is_koyazuka)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.objectType = "STANDCOLUMN";
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK_EQ(commands[0].memberId, std::string("105×105 - 小屋束"));
	CHECK_EQ(commands[0].drawClass, std::string(CLASS_KOYAZUKA));
	// 小屋束の構造用途は "5"（柱用途だと VW の柱高さモデルで上端高さが崩れる）。
	CHECK_EQ(commands[0].structuralUse, std::string("5"));
}

TEST(build_general_column_structural_use_is_column)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "2FL", 3500.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK_EQ(commands[0].structuralUse, std::string("4"));
}

TEST(build_single_story_column_class_is_kudabashira)
{
	// 上端 ≈ 600 + 2844 = 3444 < 2FL(3500) → 管柱。
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "2FL", 3500.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.height = 2844.0;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK_EQ(commands[0].drawClass, std::string(CLASS_KUDABASHIRA));
}

TEST(build_through_column_class_is_toshibashira)
{
	// 上端 ≈ 600 + 5700 = 6300 で 2FL(3500) を貫く → 通し柱。
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "2FL", 3500.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.height = 5700.0;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK_EQ(commands[0].drawClass, std::string(CLASS_TOSHIBASHIRA));
}

TEST(build_skips_column_without_placement)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.withGeometry = false;
		makeColumn(step, storey, spec);
	}

	CHECK(buildColumnCommands(step.build()).empty());
}

TEST(build_hardware_defaults_to_empty)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(commands[0].topHardware.empty());
	CHECK(commands[0].bottomHardware.empty());
}

TEST(build_matches_hardware_by_position)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.ox = 1000.0;
		spec.oy = 2000.0;
		makeColumn(step, storey, spec);
	}
	makeHardware(step, storey, 1000.0, 2000.0, "柱No.4:柱頭金物", "柱頭金物:(ろ)");
	makeHardware(step, storey, 1000.0, 2000.0, "柱No.4:柱脚金物", "柱脚金物:(い)");

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK_EQ(commands[0].topHardware, std::string("柱頭金物:(ろ)"));
	CHECK_EQ(commands[0].bottomHardware, std::string("柱脚金物:(い)"));
	// 金物仕様は構造材 ID にも連結される。
	CHECK_EQ(commands[0].memberId, std::string("105×105 - 管柱 / 柱頭金物:(ろ) / 柱脚金物:(い)"));
}

TEST(build_does_not_match_hardware_at_other_position)
{
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		makeColumn(step, storey, spec);
	}
	makeHardware(step, storey, 5000.0, 5000.0, "柱No.9:柱頭金物", "柱頭金物:(ろ)");

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build());
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(commands[0].topHardware.empty());
	CHECK(commands[0].bottomHardware.empty());
}

TEST(build_koyazuka_width_matches_moya_on_top)
{
	// 小屋束（下端 6200・高さ 800 → 上端 7000。IFC の断面 105×105 は適当な値）の真上に
	// 幅 90 の母屋が乗る → 断面は 90×90 に置き換わり、構造材 ID も補正後の寸法で作られる。
	StepText step;
	const int storey = makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.oz = -100.0;
		spec.height = 800.0;
		spec.objectType = "STANDCOLUMN";
		makeColumn(step, storey, spec);
	}
	TopMemberSpec spec;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 7090.0;
	const MemberCommand moya = topMember(spec);

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build(), {moya});
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(near(commands[0].width, 90.0));
	CHECK(near(commands[0].depth, 90.0));
	CHECK_EQ(commands[0].memberId, std::string("90×90 - 小屋束"));
}

TEST(build_koyazuka_keeps_ifc_size_without_member_on_top)
{
	StepText step;
	const int storey = makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.oz = -100.0;
		spec.height = 800.0;
		spec.objectType = "STANDCOLUMN";
		makeColumn(step, storey, spec);
	}

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build(), {});
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(near(commands[0].width, 105.0));
	CHECK(near(commands[0].depth, 105.0));
}

TEST(build_general_column_not_resized_by_member)
{
	// 管柱（小屋束でない柱）は真上に材があっても断面を変えない。
	StepText step;
	const int storey = makeStorey(step, "1FL", 600.0);
	makeStorey(step, "2FL", 3500.0);
	makeStorey(step, "RFL", 6300.0);
	{
		ColumnSpec spec;
		spec.width = 105.0;
		spec.depth = 120.0;
		spec.height = 2718.0;
		makeColumn(step, storey, spec);
	}
	TopMemberSpec spec;
	spec.start = Vec2{-1000.0, 0.0};
	spec.end = Vec2{1000.0, 0.0};
	spec.topZ = 3400.0;
	const MemberCommand moya = topMember(spec);

	const std::vector<ColumnCommand> commands = buildColumnCommands(step.build(), {moya});
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(near(commands[0].width, 105.0));
	CHECK(near(commands[0].depth, 120.0));
}

// ---------------------------------------------------------------------------
// 柱頭・柱脚金物の異常系（型を辿れない金物は仕様が空 ＝ 対応付けない）
// ---------------------------------------------------------------------------

namespace
{
	// 金物を「型の辿り方」だけ変えて置く。typeRelBody は IfcRelDefinesByType の本体で、
	// 空文字なら型そのものを作らない。placement=false なら配置を持たない金物にする。
	std::vector<ColumnCommand> buildWithHardware(const std::string& typeRelKind, bool placement)
	{
		StepText step;
		const int storey = makeStorey(step, "1FL", 600.0);
		makeStorey(step, "RFL", 6300.0);
		{
			ColumnSpec spec;
			makeColumn(step, storey, spec);
		}

		std::string placementRef = "$";
		if (placement)
		{
			const int location = point3(step, 0.0, 0.0, 0.0);
			const int axis = step.add("IFCAXIS2PLACEMENT3D(" + ref(location) + ",$,$)");
			placementRef = ref(step.add("IFCLOCALPLACEMENT($," + ref(axis) + ")"));
		}
		const int fastener = step.add("IFCMECHANICALFASTENER('f',$,'柱No.1:柱頭金物',$,$," +
									  placementRef + ",$,$,$,$)");
		if (typeRelKind == "empty-related")
		{
			// 金物を（RelatedObjects ではなく）RelatingType 側から参照する壊れた rel。
			// 逆参照には現れるが RelatedObjects がリストでないので型として採らない。
			step.add("IFCRELDEFINESBYTYPE('d',$,$,$,$," + ref(fastener) + ")");
		}
		else if (typeRelKind == "other-object")
		{
			// 同じく RelatingType 側から参照する壊れた rel で、RelatedObjects は別の要素。
			// 逆参照には現れるが、この金物を定義していないので型として採らない。
			step.add("IFCRELDEFINESBYTYPE('d',$,$,$,(" + ref(storey) + ")," + ref(fastener) + ")");
		}
		else if (typeRelKind == "missing-type")
		{
			// RelatingType が $（型が無い）。
			step.add("IFCRELDEFINESBYTYPE('d',$,$,$,(" + ref(fastener) + "),$)");
		}
		else if (typeRelKind == "unnamed-type")
		{
			// 型はあるが Name が $ → 仕様が空文字になる。
			const int type = step.add("IFCMECHANICALFASTENERTYPE('t',$,$,$,$,$,$,$,$)");
			step.add("IFCRELDEFINESBYTYPE('d',$,$,$,(" + ref(fastener) + ")," + ref(type) + ")");
		}
		contain(step, storey, fastener);
		return buildColumnCommands(step.build());
	}
} // namespace

TEST(build_ignores_hardware_whose_type_cannot_be_resolved)
{
	// 型を辿れない金物（rel が無い／RelatedObjects がリストでない／別要素を定義している／
	// RelatingType が無い／型名が空）はいずれも仕様が空になり、柱へ対応付けない。
	for (const std::string& kind :
		 {std::string("none"), std::string("empty-related"), std::string("other-object"),
		  std::string("missing-type"), std::string("unnamed-type")})
	{
		const std::vector<ColumnCommand> commands = buildWithHardware(kind, true);
		CHECK_EQ(commands.size(), std::size_t(1));
		if (commands.empty())
			continue;
		CHECK(commands[0].topHardware.empty());
		CHECK_EQ(commands[0].memberId, std::string("105×105 - 管柱"));
	}
}

TEST(build_ignores_hardware_without_placement)
{
	// 配置を持たない金物は平面座標で対応付けられないのでスキップする。
	const std::vector<ColumnCommand> commands = buildWithHardware("named", false);
	CHECK_EQ(commands.size(), std::size_t(1));
	if (commands.empty())
		return;
	CHECK(commands[0].topHardware.empty());
}

// ---------------------------------------------------------------------------
// 実フィクスチャ
// ---------------------------------------------------------------------------

TEST(reads_sample_house_fixture)
{
	bool ok = false;
	const Model& model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);
	const std::vector<ColumnCommand> commands = buildColumnCommands(model);
	CHECK(!commands.empty());

	// 配置先は必ず span 柱レイヤで、from はその柱が立つ階（1 始まり）。
	for (const ColumnCommand& command : commands)
	{
		double from = 0.0;
		double to = 0.0;
		CHECK(HomeskzIfcImport::parse::parseSpanLayer(command.layer, from, to));
		CHECK(to > from);
		CHECK(command.width > 0.0 && command.depth > 0.0 && command.height > 0.0);
		CHECK(!command.memberId.empty());
		CHECK(!command.drawClass.empty());
		CHECK(!command.bottomBound.level.empty());
		CHECK(!command.topBound.level.empty());
	}

	// 3 階建て相当（1FL/2FL/RFL）で、1 階には 2 階止まりの管柱と 3 階床まで届く通し柱、
	// 屋根階には主屋根の小屋束（屋根面で止まる半整数）が立つ。
	const std::map<int, std::vector<std::string>> byStory = collectColumnLayersByStory(commands);
	CHECK(sameVec(byStory.at(0), {"1to2-柱", "1to3-柱"}));
	CHECK(sameVec(byStory.at(1), {"2to2.5-柱", "2to3-柱"}));
	CHECK(sameVec(byStory.at(2), {"3to3.5-柱"}));

	// 通し柱は 1 階の "1to3-柱" にだけ現れる（クラスも通し柱）。
	for (const ColumnCommand& command : commands)
	{
		if (command.drawClass == CLASS_TOSHIBASHIRA)
			CHECK_EQ(command.layer, std::string("1to3-柱"));
	}
}

TEST(all_fixtures_bounds_span_the_column_height)
{
	// **バウンドの差が描かれる高さを支配する**（鉛直パスはその高さで実体を作るために要る。
	// parse/Column.h）。したがって全フィクスチャの全柱で次が成り立たなければならない:
	//   * 下端バウンドの絶対 Z ＝ elevation（柱下端）
	//   * 上端バウンドの絶対 Z ＝ elevation + height（柱上端）
	// 小屋束の上端 offset を下端と同値にしていた頃はここが崩れ、実機で高さ 0 の小屋束に
	// なっていた（M8 のローカル確認 3 周目）。
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);
		if (!ok)
			continue;

		// 各階の横架材天端（最上階は軒高）の絶対 Z ＝ バウンド先レベルの高さ。
		const std::vector<HomeskzIfcImport::parse::StoryInfo> stories =
			HomeskzIfcImport::parse::collectStories(model);
		std::vector<double> levelZ;
		levelZ.reserve(stories.size());
		for (const HomeskzIfcImport::parse::StoryInfo& story : stories)
			levelZ.push_back(story.isTop ? story.elevation : story.elevation + story.beamOffset);

		for (const ColumnCommand& command : buildColumnCommands(model))
		{
			double from = 0.0;
			double to = 0.0;
			CHECK(HomeskzIfcImport::parse::parseSpanLayer(command.layer, from, to));
			const auto base = static_cast<std::size_t>(from) - 1;
			const std::size_t top = base + static_cast<std::size_t>(command.topBound.storyOffset);
			CHECK(base < levelZ.size() && top < levelZ.size());
			if (base >= levelZ.size() || top >= levelZ.size())
				continue;

			CHECK(near(levelZ[base] + command.bottomBound.offset, command.elevation));
			CHECK(near(levelZ[top] + command.topBound.offset, command.elevation + command.height));
		}
	}
}

TEST(all_fixtures_build_columns_deterministically)
{
	for (const std::string& name : allFixtures())
	{
		bool ok = false;
		const Model& model = fixture(name, ok);
		CHECK(ok);
		if (!ok)
			continue;

		const std::vector<ColumnCommand> first = buildColumnCommands(model);
		CHECK(!first.empty());
		const std::vector<ColumnCommand> second = buildColumnCommands(model);
		CHECK_EQ(first.size(), second.size());
		for (std::size_t i = 0; i < first.size() && i < second.size(); ++i)
		{
			CHECK_EQ(first[i].layer, second[i].layer);
			CHECK_EQ(first[i].memberId, second[i].memberId);
			CHECK(near(first[i].position.x, second[i].position.x));
			CHECK(near(first[i].position.y, second[i].position.y));
			CHECK(near(first[i].elevation, second[i].elevation));
		}
	}
}

TEST_MAIN();
