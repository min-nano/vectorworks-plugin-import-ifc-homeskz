//
//	ParseShearWallTests.cpp
//
//	耐力壁解析（src/parse/ShearWall）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。**期待値は手書きで持
//	つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M19）: 筋かい・面材の判別（Name 接頭辞＋エンティティ型）・
//	壁面座標への落とし込み（軸＝押し出し方向の直交・軸の向きの決定性・材厚・見付け幅・
//	傾きの向き）・鉛直押し出しを除外すること・たすき掛けの同名まとめ・表裏の面材のまとめ・
//	配置先レイヤ・レイヤ平面からの相対高さ・決定性・全フィクスチャの通し、そして
//	耐力壁レベル（"n-耐力壁"）と伏図の表示レイヤに載ること。実フィクスチャのパスは CMake が
//	HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/BuildDocument.h"
#include "parse/Context.h"
#include "parse/Loader.h"
#include "parse/ShearWall.h"
#include "parse/PlanLevel.h"
#include "parse/Story.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

using namespace HomeskzIfcImport;
using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::ShearWallBraceStyle;
using HomeskzIfcImport::core::ShearWallCommand;
using HomeskzIfcImport::core::ShearWallKind;
using HomeskzIfcImport::core::ShearWallPanelSide;
using HomeskzIfcImport::parse::anyShearWallOnLayer;
using HomeskzIfcImport::parse::buildShearWallCommands;
using HomeskzIfcImport::parse::fitShearWallsToMembers;
using HomeskzIfcImport::parse::isDoubleBrace;
using HomeskzIfcImport::parse::isShearBrace;
using HomeskzIfcImport::parse::isShearPanel;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::resolveShearWallPiece;
using HomeskzIfcImport::parse::ShearWallPiece;
using HomeskzIfcImport::parse::StoryInfo;
using HomeskzIfcTests::allFixtures;
using HomeskzIfcTests::fixture;
using HomeskzIfcTests::fixtureDocument;
using HomeskzIfcTests::near;

namespace
{
	// 合成の筋かい 1 本。断面は 3000×90 の矩形（プロファイル u=0…3000・v=±45）を 45mm
	// 押し出したもので、要素配置で
	//   * 材長方向（局所 X）= (0.6, 0, 0.8) …… 走り 1800・立ち上がり 2400 の 3:4:5
	//   * 押し出し方向（局所 Z）= (0, −1, 0) … 壁面に直交＝材厚 45 の向き
	// へ倒してある。位置は y = +22.5 で、押し出し（−Y へ 45）と合わせて壁芯 y = 0 に載る。
	const char* kBraceText = "#1=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,0.);\n"
							 "#10=IFCCARTESIANPOINT((0.,-45.));\n"
							 "#11=IFCCARTESIANPOINT((3000.,-45.));\n"
							 "#12=IFCCARTESIANPOINT((3000.,45.));\n"
							 "#13=IFCCARTESIANPOINT((0.,45.));\n"
							 "#14=IFCPOLYLINE((#10,#11,#12,#13));\n"
							 "#15=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#14);\n"
							 "#16=IFCCARTESIANPOINT((0.,0.,0.));\n"
							 "#17=IFCDIRECTION((0.,0.,1.));\n"
							 "#18=IFCDIRECTION((1.,0.,0.));\n"
							 "#19=IFCAXIS2PLACEMENT3D(#16,#17,#18);\n"
							 "#20=IFCDIRECTION((0.,0.,1.));\n"
							 "#21=IFCEXTRUDEDAREASOLID(#15,#19,#20,45.);\n"
							 "#22=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#21));\n"
							 "#23=IFCPRODUCTDEFINITIONSHAPE($,$,(#22));\n"
							 "#24=IFCCARTESIANPOINT((0.,22.5,0.));\n"
							 "#25=IFCDIRECTION((0.,-1.,0.));\n"
							 "#26=IFCDIRECTION((0.6,0.,0.8));\n"
							 "#27=IFCAXIS2PLACEMENT3D(#24,#25,#26);\n"
							 "#28=IFCLOCALPLACEMENT($,#27);\n"
							 "#29=IFCMEMBER('m',$,'筋かい:1FL_1',$,$,#28,#23,$);\n"
							 "#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#29),#1);\n";

	// 合成の面材 1 枚。910×2700 の矩形を 12mm 押し出したもので、押し出し方向（−Y）が
	// 壁面の法線。壁芯 y = 0 の裏側（−Y）に張ってある。
	const char* kPanelText = "#1=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,0.);\n"
							 "#10=IFCCARTESIANPOINT((455.,0.));\n"
							 "#11=IFCCARTESIANPOINT((-455.,0.));\n"
							 "#12=IFCCARTESIANPOINT((-455.,2700.));\n"
							 "#13=IFCCARTESIANPOINT((455.,2700.));\n"
							 "#14=IFCPOLYLINE((#10,#11,#12,#13));\n"
							 "#15=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#14);\n"
							 "#16=IFCCARTESIANPOINT((0.,0.,0.));\n"
							 "#17=IFCDIRECTION((0.,0.,1.));\n"
							 "#18=IFCDIRECTION((1.,0.,0.));\n"
							 "#19=IFCAXIS2PLACEMENT3D(#16,#17,#18);\n"
							 "#20=IFCDIRECTION((0.,0.,1.));\n"
							 "#21=IFCEXTRUDEDAREASOLID(#15,#19,#20,12.);\n"
							 "#22=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#21));\n"
							 "#23=IFCPRODUCTDEFINITIONSHAPE($,$,(#22));\n"
							 "#24=IFCCARTESIANPOINT((0.,0.,0.));\n"
							 "#25=IFCDIRECTION((0.,-1.,0.));\n"
							 "#26=IFCDIRECTION((1.,0.,0.));\n"
							 "#27=IFCAXIS2PLACEMENT3D(#24,#25,#26);\n"
							 "#28=IFCLOCALPLACEMENT($,#27);\n"
							 "#29=IFCWALL('w',$,'面材:1_0_1',$,'STANDARD',#28,#23,$);\n"
							 "#30=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#29),#1);\n";

	// 合わせ込みの試験用の 1 階（Elevation 600・横架材天端 −174 ＝レイヤ平面 426）と
	// 最上階。1 階の耐力壁レイヤは "1-耐力壁"。
	std::vector<StoryInfo> fitStories()
	{
		return {StoryInfo{1, 600.0, -174.0, false, "1FL"}, StoryInfo{2, 3500.0, 0.0, true, "RFL"}};
	}

	// 軸 (0,0)→(1820,0) の耐力壁。IFC の高さはレイヤ平面から 0〜2700（絶対 426〜3126）。
	ShearWallCommand fitWall()
	{
		ShearWallCommand wall;
		wall.layer = "1-耐力壁";
		wall.drawClass = "筋かい";
		wall.start = core::Vec2{0.0, 0.0};
		wall.end = core::Vec2{1820.0, 0.0};
		wall.kind = ShearWallKind::Brace;
		wall.width = 90.0;
		wall.thickness = 45.0;
		wall.clearSpan = 1715.0;
		wall.bottomHeight = 0.0;
		wall.topHeight = 2700.0;
		wall.topHeightEnd = 2700.0;
		return wall;
	}

	// 両端の柱（105 角。1 階を base とする span 柱）。内法は s = 52.5〜1767.5。
	std::vector<core::ColumnCommand> fitColumns()
	{
		std::vector<core::ColumnCommand> columns(2);
		columns[0].layer = "1to2-柱";
		columns[0].position = core::Vec2{0.0, 0.0};
		columns[0].width = 105.0;
		columns[1].layer = "1to2-柱";
		columns[1].position = core::Vec2{1820.0, 0.0};
		columns[1].width = 105.0;
		return columns;
	}

	// 横架材 1 本（天端中央線 start→end・天端 Z・せい）。幅は 105。
	core::MemberCommand fitBeam(core::Vec2 start, core::Vec2 end, double elevation,
								double endElevation, double height)
	{
		core::MemberCommand member;
		member.start = start;
		member.end = end;
		member.width = 105.0;
		member.height = height;
		member.elevation = elevation;
		member.endElevation = endElevation;
		return member;
	}

	// 文字列が接尾辞で終わるか。
	bool endsWith(const std::string& text, const std::string& suffix)
	{
		return text.size() >= suffix.size() &&
			   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}
} // namespace

// --- 要素の判別 -------------------------------------------------------------

TEST(shear_wall_elements_are_matched_by_name_and_type)
{
	const Model model = loadIfcFromText("#1=IFCMEMBER('m',$,'筋かい:1FL_1',$,$,$,$,$);\n"
										"#2=IFCMEMBER('m',$,'筋かいダブル:1FL_2',$,$,$,$,$);\n"
										"#3=IFCBEAM('b',$,'筋かい:1FL_3',$,$,$,$,$);\n"
										"#4=IFCWALL('w',$,'面材:1_0_1',$,$,$,$,$);\n"
										"#5=IFCWALL('w',$,'基礎梁:1',$,$,$,$,$);\n"
										"#6=IFCMEMBER('m',$,'火打:0_1',$,$,$,$,$);\n");
	CHECK(isShearBrace(*model.entity(1)));
	CHECK(!isDoubleBrace(*model.entity(1)));
	// たすき掛けも「筋かい」始まりなので筋かいとして抽出し、種別だけを別に判定する。
	CHECK(isShearBrace(*model.entity(2)));
	CHECK(isDoubleBrace(*model.entity(2)));
	CHECK(!isShearBrace(*model.entity(3))); // 名前は筋かいでも IfcBeam は対象外
	CHECK(isShearPanel(*model.entity(4)));
	CHECK(!isShearPanel(*model.entity(5))); // 基礎梁の壁は面材ではない
	CHECK(!isShearBrace(*model.entity(6))); // 火打（parse/FireBrace の担当）
}

// --- 壁面座標への落とし込み -------------------------------------------------

TEST(shear_wall_brace_piece_is_measured_in_the_wall_plane)
{
	const Model model = loadIfcFromText(kBraceText);
	ShearWallPiece piece;
	CHECK(resolveShearWallPiece(model, *model.entity(29), true, piece));

	// 押し出しは −Y なので法線は ±Y、軸はそれに直交する ±X。軸は (x, y) の辞書順で
	// 正の向きへ揃うので (1, 0)、法線はそれを +90 度回した (0, 1) になる。
	CHECK(near(piece.axis.x, 1.0) && near(piece.axis.y, 0.0));
	CHECK(near(piece.normal.x, 0.0) && near(piece.normal.y, 1.0));

	// 軸方向の広がりは走り 1800 に見付け幅の張り出し（±36）を足した 1872。
	CHECK(near(piece.sMin, -36.0, 1e-6));
	CHECK(near(piece.sMax, 1836.0, 1e-6));
	// 中心面は壁芯（y = 0）に載り、材厚は押し出し長 45。
	CHECK(near(piece.offset, 0.0, 1e-6));
	CHECK(near(piece.thickness, 45.0, 1e-6));
	// 高さは立ち上がり 2400 に見付け幅の張り出し（±27）を足した −27〜2427。
	CHECK(near(piece.zBottom, -27.0, 1e-6));
	CHECK(near(piece.zTop, 2427.0, 1e-6));
	// 見付け幅は矩形断面の短辺（90）。**最も離れた 2 点は対角線（3001）なので、
	// そちらを軸と見なすと 2 倍近くの値になる**——回転キャリパの最小幅で測る。
	CHECK(near(piece.width, 90.0, 1e-6));
	// 材は +X 側で高くなる（局所 X が (0.6, 0, 0.8)）。
	CHECK(piece.risesToMax);
}

TEST(shear_wall_panel_piece_is_measured_in_the_wall_plane)
{
	const Model model = loadIfcFromText(kPanelText);
	ShearWallPiece piece;
	CHECK(resolveShearWallPiece(model, *model.entity(29), false, piece));

	CHECK(near(piece.axis.x, 1.0) && near(piece.axis.y, 0.0));
	CHECK(near(piece.sMin, -455.0, 1e-6));
	CHECK(near(piece.sMax, 455.0, 1e-6));
	CHECK(near(piece.zBottom, 0.0, 1e-6));
	CHECK(near(piece.zTop, 2700.0, 1e-6));
	CHECK(near(piece.thickness, 12.0, 1e-6));
	// 面材は壁芯の裏（法線の負側）に張ってあるので、中心面は −6。
	CHECK(near(piece.offset, -6.0, 1e-6));
	// 面材では見付け幅を測らない（使わない枠を埋めない）。
	CHECK(near(piece.width, 0.0));
}

TEST(shear_wall_vertical_extrusion_is_rejected)
{
	// 押し出しが鉛直（＝壁面に直交しない）ものは耐力壁として解釈できない。火打が
	// この形で、名前が違うので抽出はしないが、幾何の関門としても閉じておく。
	const std::string text =
		std::string(kBraceText)
			.replace(std::string(kBraceText).find("#25=IFCDIRECTION((0.,-1.,0.));"),
					 std::string("#25=IFCDIRECTION((0.,-1.,0.));").size(),
					 "#25=IFCDIRECTION((0.,0.,1.));");
	const Model model = loadIfcFromText(text);
	ShearWallPiece piece;
	CHECK(!resolveShearWallPiece(model, *model.entity(29), true, piece));
	CHECK(buildShearWallCommands(model).empty());
}

TEST(shear_wall_degenerate_solid_is_rejected)
{
	// 押し出し長 0 のソリッドは「厚みの無い壁」で、材厚も表裏も決まらない。壁面座標へ
	// 落とした時点で除外する（1 枚の異常で全体を止めないための関門）。
	const std::string zero =
		std::string(kBraceText)
			.replace(std::string(kBraceText).find("#21=IFCEXTRUDEDAREASOLID(#15,#19,#20,45.);"),
					 std::string("#21=IFCEXTRUDEDAREASOLID(#15,#19,#20,45.);").size(),
					 "#21=IFCEXTRUDEDAREASOLID(#15,#19,#20,0.);");
	const Model model = loadIfcFromText(zero);
	ShearWallPiece piece;
	CHECK(!resolveShearWallPiece(model, *model.entity(29), true, piece));
	CHECK(buildShearWallCommands(model).empty());
}

// --- 命令の組み立て ---------------------------------------------------------

TEST(shear_wall_brace_command_from_synthetic_model)
{
	const Model model = loadIfcFromText(kBraceText);
	const std::vector<ShearWallCommand> walls = buildShearWallCommands(model);
	CHECK_EQ(walls.size(), std::size_t{1});

	const ShearWallCommand& wall = walls.front();
	CHECK(wall.kind == ShearWallKind::Brace);
	CHECK(wall.braceStyle == ShearWallBraceStyle::Single);
	CHECK(wall.braceRisesToEnd);
	// ストーリが 1 つだけなら最上階＝屋根なので "R-耐力壁"。
	CHECK_EQ(wall.layer, std::string("R-耐力壁"));
	// 柱が 1 本も無いモデルなので、端は要素自身の広がり・探索先レイヤは空。
	CHECK(wall.targetLayers.empty());
	CHECK(near(wall.start.x, -36.0, 1e-6) && near(wall.start.y, 0.0, 1e-6));
	CHECK(near(wall.end.x, 1836.0, 1e-6) && near(wall.end.y, 0.0, 1e-6));
	CHECK(near(wall.clearSpan, 1872.0, 1e-6));
	CHECK(near(wall.width, 90.0, 1e-6));
	CHECK(near(wall.thickness, 45.0, 1e-6));
	// 最上階のレイヤ平面は軒高（オフセット 0）なので、高さはそのまま。
	CHECK(near(wall.bottomHeight, -27.0, 1e-6));
	CHECK(near(wall.topHeight, 2427.0, 1e-6));
	// 解析の段階では水平（上下の横架材への合わせ込みは後処理）。
	CHECK(near(wall.topHeightEnd, wall.topHeight, 1e-6));
	Document document;
	document.shearWalls = walls;
	CHECK(core::validateDocument(document));
}

TEST(shear_wall_ignores_columns_outside_span_layers)
{
	// 柱を探すのは span 柱レイヤ（"{from}to{to}-柱"）に載る柱だけ。名前が span でない
	// レイヤの柱は、壁端のすぐそばにあっても端の柱にしない（どの階を通るか決まらない）。
	const Model model = loadIfcFromText(kBraceText);
	parse::Context context(model);
	std::vector<core::ColumnCommand> columns(2);
	columns[0].layer = "柱"; // span でない（始端から 64mm）
	columns[0].position = core::Vec2{-100.0, 0.0};
	columns[0].width = 105.0;
	columns[1].layer = "1to2-柱"; // 1 階を通る（終端から 64mm）
	columns[1].position = core::Vec2{1900.0, 0.0};
	columns[1].width = 105.0;

	const std::vector<ShearWallCommand> walls = buildShearWallCommands(context, columns);
	CHECK_EQ(walls.size(), std::size_t{1});
	const ShearWallCommand& wall = walls.front();
	CHECK_EQ(wall.targetLayers, std::string("1to2-柱"));
	CHECK(near(wall.start.x, -36.0, 1e-6)); // 要素自身の端のまま
	CHECK(near(wall.end.x, 1900.0, 1e-6));	// span の柱の芯へ寄る
}

TEST(shear_wall_double_brace_is_grouped_by_name)
{
	// たすき掛けは**同じ Name の 2 要素**として出るので、1 枚の耐力壁にまとまる。
	std::string text = kBraceText;
	text += "#40=IFCCARTESIANPOINT((1800.,22.5,0.));\n"
			"#41=IFCDIRECTION((0.,-1.,0.));\n"
			"#42=IFCDIRECTION((-0.6,0.,0.8));\n"
			"#43=IFCAXIS2PLACEMENT3D(#40,#41,#42);\n"
			"#44=IFCLOCALPLACEMENT($,#43);\n"
			"#45=IFCMEMBER('m2',$,'筋かい:1FL_1',$,$,#44,#23,$);\n"
			"#46=IFCRELCONTAINEDINSPATIALSTRUCTURE('r2',$,$,$,(#45),#1);\n";
	const std::vector<ShearWallCommand> walls = buildShearWallCommands(loadIfcFromText(text));
	CHECK_EQ(walls.size(), std::size_t{1});
	CHECK(walls.front().braceStyle == ShearWallBraceStyle::Double);
}

TEST(shear_wall_panel_command_from_synthetic_model)
{
	const std::vector<ShearWallCommand> walls = buildShearWallCommands(loadIfcFromText(kPanelText));
	CHECK_EQ(walls.size(), std::size_t{1});

	const ShearWallCommand& wall = walls.front();
	CHECK(wall.kind == ShearWallKind::Panel);
	CHECK(near(wall.thickness, 12.0, 1e-6));
	CHECK(near(wall.clearSpan, 910.0, 1e-6));
	CHECK(near(wall.topHeight, 2700.0, 1e-6));
	// 面材が 1 枚だけ・柱も無いので、軸の線は面材自身が通る＝表側（オフセット 0）。
	CHECK(wall.panelSide == ShearWallPanelSide::Front);
	CHECK(near(wall.panelOffset, 0.0, 1e-6));
	// 柱の無い端も軸の線に載るので、両端の法線方向は揃う（斜めの軸にならない）。
	CHECK(near(wall.start.y, wall.end.y, 1e-6));
}

TEST(shear_wall_double_sided_panels_are_one_wall)
{
	// 同じ軸・同じ区間で法線の正負に 1 枚ずつ張った面材は 1 枚の耐力壁（両面）になる。
	std::string text = kPanelText;
	// 105 角の柱を挟んだ裏面（y = 105 から +Y へ 12mm）。局所 X を −X に取ることで
	// 局所 Y（＝断面の高さ方向）が +Z のままになる。
	text += "#40=IFCCARTESIANPOINT((0.,105.,0.));\n"
			"#41=IFCDIRECTION((0.,1.,0.));\n"
			"#42=IFCDIRECTION((-1.,0.,0.));\n"
			"#43=IFCAXIS2PLACEMENT3D(#40,#41,#42);\n"
			"#44=IFCLOCALPLACEMENT($,#43);\n"
			"#45=IFCWALL('w2',$,'面材:1_0_2',$,'STANDARD',#44,#23,$);\n"
			"#46=IFCRELCONTAINEDINSPATIALSTRUCTURE('r2',$,$,$,(#45),#1);\n";
	const std::vector<ShearWallCommand> walls = buildShearWallCommands(loadIfcFromText(text));
	CHECK_EQ(walls.size(), std::size_t{1});
	CHECK(walls.front().panelSide == ShearWallPanelSide::Both);
	// 中心面は y = −6 と y = 111。柱が無いので軸の線はその中点（y = 52.5）になり、
	// 表裏はそこから 58.5 ずつ離れている（＝半柱幅 52.5 ＋ 板厚の半分 6）。
	CHECK(near(walls.front().panelOffset, 58.5, 1e-6));
}

TEST(shear_wall_parallel_walls_on_the_same_line_are_not_merged)
{
	// 同じ通りに並ぶ 2 枚の壁は軸も軸方向の区間も一致しうる。法線方向に離れていれば
	// 別々の耐力壁でなければならない（表裏のまとめが誤って適用されないこと）。
	std::string text = kPanelText;
	text += "#40=IFCCARTESIANPOINT((0.,4000.,0.));\n"
			"#41=IFCDIRECTION((0.,-1.,0.));\n"
			"#42=IFCDIRECTION((1.,0.,0.));\n"
			"#43=IFCAXIS2PLACEMENT3D(#40,#41,#42);\n"
			"#44=IFCLOCALPLACEMENT($,#43);\n"
			"#45=IFCWALL('w2',$,'面材:1_0_2',$,'STANDARD',#44,#23,$);\n"
			"#46=IFCRELCONTAINEDINSPATIALSTRUCTURE('r2',$,$,$,(#45),#1);\n";
	CHECK_EQ(buildShearWallCommands(loadIfcFromText(text)).size(), std::size_t{2});
}

TEST(shear_wall_without_storeys_is_empty)
{
	// FL ストーリが 1 つも無いモデルは配置先レイヤが決まらないので空。
	CHECK(buildShearWallCommands(loadIfcFromText("#1=IFCMEMBER('m',$,'筋かい:1_1',$,$,$,$,$);\n"))
			  .empty());
}

TEST(shear_wall_without_solid_is_skipped)
{
	// 押し出しソリッドを解決できない要素（形状表現なし）は命令を出さない。
	CHECK(buildShearWallCommands(
			  loadIfcFromText("#1=IFCBUILDINGSTOREY('s',$,'1FL',$,$,$,$,$,.ELEMENT.,0.);\n"
							  "#2=IFCMEMBER('m',$,'筋かい:1FL_1',$,$,$,$,$);\n"
							  "#3=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#2),#1);\n"))
			  .empty());
}

TEST(shear_wall_layer_predicate)
{
	std::vector<ShearWallCommand> walls(1);
	walls.front().layer = "1-耐力壁";
	CHECK(anyShearWallOnLayer(walls, "1-耐力壁"));
	CHECK(!anyShearWallOnLayer(walls, "2-耐力壁"));
	CHECK(!anyShearWallOnLayer({}, "1-耐力壁"));
}

// --- 実フィクスチャ ---------------------------------------------------------

TEST(shear_wall_fixture_count_and_layers)
{
	// サンプル1: 1 階 43 枚（筋かい 29・面材 14）／2 階 36 枚（筋かい 20・面材 16）。
	// 解析を変えたときに件数が動けば気付けるようにするための固定値。
	bool ok = false;
	const Model& model = fixture("サンプル1 (住木邸新築工事).ifc", ok);
	CHECK(ok);

	const std::vector<ShearWallCommand> walls = buildShearWallCommands(model);
	CHECK_EQ(walls.size(), std::size_t{79});

	std::size_t first = 0;
	std::size_t braces = 0;
	for (const ShearWallCommand& wall : walls)
	{
		CHECK(endsWith(wall.layer, "耐力壁"));
		CHECK(!wall.drawClass.empty());
		CHECK(wall.thickness > 0.0);
		CHECK(wall.clearSpan > 0.0);
		CHECK(wall.topHeight > wall.bottomHeight);
		if (wall.layer == "1-耐力壁")
			++first;
		if (wall.kind == ShearWallKind::Brace)
		{
			++braces;
			CHECK(wall.width > 0.0);
		}
	}
	CHECK_EQ(first, std::size_t{43});
	CHECK_EQ(braces, std::size_t{49});
}

TEST(shear_wall_fixture_ends_sit_on_column_centres)
{
	// 端は柱芯へ寄せてある。柱の命令と照合して、少なくとも大半の端が柱の位置に
	// 一致することを確かめる（開口部の側柱が無い端はそのままなので全数一致は求めない）。
	const Document& document = fixtureDocument("サンプル1 (住木邸新築工事).ifc");
	CHECK(!document.shearWalls.empty());

	std::size_t matched = 0;
	for (const ShearWallCommand& wall : document.shearWalls)
	{
		for (const core::Vec2& point : {wall.start, wall.end})
		{
			const bool onColumn =
				std::ranges::any_of(document.columns, [&point](const core::ColumnCommand& column)
									{ return core::samePoint(column.position, point); });
			if (onColumn)
				++matched;
		}
	}
	CHECK(matched >= document.shearWalls.size()); // 平均 1 端以上は柱に乗る
}

TEST(shear_wall_fixture_kinds_and_sides_are_all_seen)
{
	// 実データには片掛け・たすき掛け・表／裏／両面がひととおり出る。どれかの経路が
	// 機能していなかったら気付けるように、まとめて確かめる。
	bool sawSingle = false;
	bool sawDouble = false;
	bool sawFront = false;
	bool sawBack = false;
	bool sawBoth = false;
	for (const std::string& name : allFixtures())
	{
		for (const ShearWallCommand& wall : fixtureDocument(name).shearWalls)
		{
			if (wall.kind == ShearWallKind::Brace)
			{
				sawSingle = sawSingle || wall.braceStyle == ShearWallBraceStyle::Single;
				sawDouble = sawDouble || wall.braceStyle == ShearWallBraceStyle::Double;
				continue;
			}
			sawFront = sawFront || wall.panelSide == ShearWallPanelSide::Front;
			sawBack = sawBack || wall.panelSide == ShearWallPanelSide::Back;
			sawBoth = sawBoth || wall.panelSide == ShearWallPanelSide::Both;
		}
	}
	CHECK(sawSingle);
	CHECK(sawDouble);
	CHECK(sawFront);
	CHECK(sawBack);
	CHECK(sawBoth);
}

TEST(shear_wall_fixture_target_layers_name_real_span_layers)
{
	// 柱を探すレイヤ名は ";" 区切りの span 柱レイヤ。実在する柱のレイヤだけを挙げる。
	const Document& document = fixtureDocument("伏図次郎【2階】.ifc");
	CHECK(!document.shearWalls.empty());

	for (const ShearWallCommand& wall : document.shearWalls)
	{
		CHECK(!wall.targetLayers.empty());
		std::size_t begin = 0;
		while (begin <= wall.targetLayers.size())
		{
			const std::size_t end = wall.targetLayers.find(';', begin);
			const std::string name = wall.targetLayers.substr(
				begin, end == std::string::npos ? std::string::npos : end - begin);
			CHECK(!name.empty());
			CHECK(std::ranges::any_of(document.columns, [&name](const core::ColumnCommand& column)
									  { return column.layer == name; }));
			if (end == std::string::npos)
				break;
			begin = end + 1;
		}
	}
}

TEST(shear_wall_fixture_target_layers_include_through_columns)
{
	// 柱を探すレイヤは**その階を通る** span 柱レイヤすべて。2 階の壁端の通し柱は 1 階を
	// base とするレイヤ（"1to3-柱"）に載るので、base だけで絞ると検出できない（実機で
	// 耐力壁 PIO が壁端の通し柱を認識しなかった不具合）。逆に 2 階の床で止まる管柱
	// （"1to2-柱"）は 2 階の壁の端には立たないので挙げない。
	//
	// 「その階」は耐力壁が立つ**伏図レベル**（parse/PlanLevel）。span の番号も伏図レベルの
	// 通し番号なので、壁のレイヤ（"2-耐力壁" / "2-耐力壁(FL-872)"）からその通し番号を引く
	// （どの階も高さが 1 つなら階の番号と同じ）。
	std::size_t throughEnds = 0;
	for (const auto& name : allFixtures())
	{
		const Document& document = fixtureDocument(name);
		bool ok = false;
		parse::Context context(fixture(name, ok));
		CHECK(ok);
		std::map<std::string, double> ordinalOfLayer;
		for (const parse::PlanLevel& planLevel : context.planLevels())
			ordinalOfLayer[parse::planLevelLayer(planLevel, context.stories()[planLevel.story],
												 parse::kLevelShearWall)] = planLevel.ordinal;
		for (const ShearWallCommand& wall : document.shearWalls)
		{
			// 最上階（"R-…"）には床の上に立つ柱が無いので確認しない。
			if (wall.layer.starts_with("R-"))
				continue;
			const auto found = ordinalOfLayer.find(wall.layer);
			CHECK(found != ordinalOfLayer.end());
			if (found == ordinalOfLayer.end())
				continue;
			const double level = found->second;
			const std::string targets = ";" + wall.targetLayers + ";";
			for (const core::ColumnCommand& column : document.columns)
			{
				double from = 0.0;
				double to = 0.0;
				if (!parse::parseSpanLayer(column.layer, from, to))
					continue;
				const bool covers = from <= level && to > level;
				const bool listed = targets.find(";" + column.layer + ";") != std::string::npos;
				CHECK_EQ(listed, covers);
				// 下の階から通っている柱に端が載った壁を数える。
				if (covers && from < level &&
					(core::samePoint(column.position, wall.start) ||
					 core::samePoint(column.position, wall.end)))
					++throughEnds;
			}
		}
	}
	CHECK(throughEnds > 0); // 通し柱に端が載る上階の耐力壁がフィクスチャに実在する
}

TEST(shear_wall_fixture_is_deterministic)
{
	bool ok = false;
	const Model& model = fixture("グレー本モデルプラン1【3階】.ifc", ok);
	CHECK(ok);

	const std::vector<ShearWallCommand> first = buildShearWallCommands(model);
	const std::vector<ShearWallCommand> second = buildShearWallCommands(model);
	CHECK_EQ(first.size(), second.size());
	for (std::size_t i = 0; i < first.size(); ++i)
	{
		CHECK_EQ(first[i].layer, second[i].layer);
		CHECK_EQ(first[i].targetLayers, second[i].targetLayers);
		CHECK(near(first[i].start.x, second[i].start.x));
		CHECK(near(first[i].start.y, second[i].start.y));
		CHECK(near(first[i].end.x, second[i].end.x));
		CHECK(near(first[i].end.y, second[i].end.y));
		CHECK(first[i].kind == second[i].kind);
	}
}

TEST(shear_wall_all_fixtures_produce_valid_commands)
{
	for (const std::string& name : allFixtures())
	{
		const Document& document = fixtureDocument(name);
		CHECK(!document.shearWalls.empty());
		CHECK(core::validateDocument(document));
	}
}

// --- レイヤと伏図 -----------------------------------------------------------

TEST(shear_wall_stories_carry_the_shear_wall_level)
{
	// 耐力壁の命令がある階にだけ "n-耐力壁" レベルができる（空レイヤを作らない）。
	const Document& document = fixtureDocument("サンプル1 (住木邸新築工事).ifc");
	for (const core::StoryCommand& story : document.stories)
	{
		const bool hasLevel =
			std::ranges::any_of(story.levels, [](const core::LevelCommand& level)
								{ return level.type == std::string(core::kLevelShearWall); });
		const bool hasCommand = std::ranges::any_of(
			document.shearWalls,
			[&story](const ShearWallCommand& wall) {
				return endsWith(wall.layer, "耐力壁") && wall.layer.starts_with(story.suffix + "-");
			});
		CHECK_EQ(hasLevel, hasCommand);
	}
}

TEST(shear_wall_floor_plan_shows_its_own_storey)
{
	// 伏図には**その階自身**の耐力壁が載る（1 階床＝土台伏図 → "1-耐力壁"、
	// 2 階床伏図 → "2-耐力壁"）。「n 階の耐力壁」は n 階の伏図で読む、という図面の
	// 呼び方に合わせた規約（parse/Sheet の M19 のコメント）。
	const Document& document = fixtureDocument("サンプル1 (住木邸新築工事).ifc");
	std::size_t checked = 0;
	for (const core::SheetCommand& sheet : document.sheets)
	{
		const auto* const expected = sheet.title == "1階床伏図"	  ? "1-耐力壁"
									 : sheet.title == "2階床伏図" ? "2-耐力壁"
																  : nullptr;
		if (expected == nullptr)
			continue;
		++checked;
		CHECK(std::ranges::find(sheet.viewport.layers, std::string(expected)) !=
			  sheet.viewport.layers.end());
		// 他の階の耐力壁は載せない（下の階のものを載せていたときは、上の階の横架材に
		// 必ず隠れていた）。
		const auto* const other = sheet.title == "1階床伏図" ? "2-耐力壁" : "1-耐力壁";
		CHECK(std::ranges::find(sheet.viewport.layers, std::string(other)) ==
			  sheet.viewport.layers.end());
	}
	CHECK_EQ(checked, static_cast<std::size_t>(2));
}

// --- 上下の横架材への合わせ込み ---------------------------------------------

TEST(shear_wall_fit_to_horizontal_beams)
{
	// 下＝土台（天端 426）、上＝胴差（天端 3420・せい 240 → 下端 3180）。高さはレイヤ平面
	// （426）からの相対なので、下端 0・上端 2754（両端とも）。
	std::vector<ShearWallCommand> walls{fitWall()};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 426.0, 426.0, 105.0),
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 3420.0, 3420.0, 240.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());
	CHECK(near(walls[0].bottomHeight, 0.0, 1e-6));
	CHECK(near(walls[0].topHeight, 2754.0, 1e-6));
	CHECK(near(walls[0].topHeightEnd, 2754.0, 1e-6));
}

TEST(shear_wall_fit_trims_a_panel_that_overlaps_the_beams)
{
	// 面材は IFC では横架材に掛かる板の広がりで出る（下 −50・上は梁の天端まで）。軸組内法へ
	// 縮める。
	ShearWallCommand panel = fitWall();
	panel.kind = ShearWallKind::Panel;
	panel.width = 0.0;
	panel.bottomHeight = -50.0;
	panel.topHeight = 2994.0;
	panel.topHeightEnd = 2994.0;
	std::vector<ShearWallCommand> walls{panel};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 426.0, 426.0, 105.0),
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 3420.0, 3420.0, 150.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());
	CHECK(near(walls[0].bottomHeight, 0.0, 1e-6));
	CHECK(near(walls[0].topHeight, 2844.0, 1e-6));
	CHECK(near(walls[0].topHeightEnd, 2844.0, 1e-6));
}

TEST(shear_wall_fit_follows_a_sloped_beam_at_the_clear_ends)
{
	// 上が登り梁: 天端中央線 (−500,0)→(2500,0)・天端 3400→4000（勾配 0.2）・せい 150。
	// 断面は材軸に直交するので、鉛直に測ったせいは 150/cosθ = 150·√(1+0.2²)。
	// 上端は**内法の両端**（s = 52.5 / 1767.5）で測る。
	std::vector<ShearWallCommand> walls{fitWall()};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 426.0, 426.0, 105.0),
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 3400.0, 4000.0, 150.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());

	const double depth = 150.0 * std::sqrt(1.0 + (0.2 * 0.2));
	const double atStart = 3400.0 + (0.2 * (52.5 + 500.0)) - depth - 426.0;
	const double atEnd = 3400.0 + (0.2 * (1767.5 + 500.0)) - depth - 426.0;
	CHECK(near(walls[0].bottomHeight, 0.0, 1e-6));
	CHECK(near(walls[0].topHeight, atStart, 1e-6));
	CHECK(near(walls[0].topHeightEnd, atEnd, 1e-6));
	CHECK(walls[0].topHeightEnd > walls[0].topHeight);
}

TEST(shear_wall_fit_follows_a_sloped_beam_running_the_other_way)
{
	// 同じ登り梁を逆向き（終端から始端へ）に持っても、軸の始点側・終点側の高さは同じ。
	std::vector<ShearWallCommand> walls{fitWall()};
	const std::vector<core::MemberCommand> members{
		fitBeam({2500.0, 0.0}, {-500.0, 0.0}, 426.0, 426.0, 105.0),
		fitBeam({2500.0, 0.0}, {-500.0, 0.0}, 4000.0, 3400.0, 150.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());

	const double depth = 150.0 * std::sqrt(1.0 + (0.2 * 0.2));
	CHECK(near(walls[0].topHeight, 3400.0 + (0.2 * 552.5) - depth - 426.0, 1e-6));
	CHECK(near(walls[0].topHeightEnd, 3400.0 + (0.2 * 2267.5) - depth - 426.0, 1e-6));
}

TEST(shear_wall_fit_levels_the_top_under_a_stepped_beam)
{
	// 上の梁が内法の途中（s = 612）でせいを変える（240 → 105）段差梁。上辺は 1 本の直線で
	// 表せないので、高い方（天端 3420 − 105 = 3315）で水平にそろえる（梁下に隙間を空けない）。
	// 実データ: グレー本モデルプラン1 の 2 階 (−5005, 455)→(−5005, 1820)。
	std::vector<ShearWallCommand> walls{fitWall()};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 426.0, 426.0, 105.0),
		fitBeam({-500.0, 0.0}, {612.0, 0.0}, 3420.0, 3420.0, 240.0),
		fitBeam({612.0, 0.0}, {2500.0, 0.0}, 3420.0, 3420.0, 105.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());
	CHECK(near(walls[0].topHeight, 2889.0, 1e-6));
	CHECK(near(walls[0].topHeightEnd, 2889.0, 1e-6));
}

TEST(shear_wall_fit_levels_the_top_under_a_beam_raised_mid_span)
{
	// 内法の両端では同じ高さでも、途中で梁の下端が上がる（両端の直線が梁から外れる）なら
	// 段差とみなし、水平にそろえるのは上がった梁の下端（梁下に隙間を空けない）。
	std::vector<ShearWallCommand> walls{fitWall()};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 426.0, 426.0, 105.0),
		fitBeam({-500.0, 0.0}, {700.0, 0.0}, 3420.0, 3420.0, 150.0),
		fitBeam({700.0, 0.0}, {1100.0, 0.0}, 3420.0, 3420.0, 105.0),
		fitBeam({1100.0, 0.0}, {2500.0, 0.0}, 3420.0, 3420.0, 150.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());
	CHECK(near(walls[0].topHeight, 3315.0 - 426.0, 1e-6));
	CHECK(near(walls[0].topHeightEnd, 3315.0 - 426.0, 1e-6));
}

TEST(shear_wall_fit_takes_the_lower_top_of_a_stepped_beam_below)
{
	// 下の梁が内法の途中で天端を変える（426 → 326。床の段差）。下端は 1 つしか持てない
	// ので、低い方の天端（326 → レイヤ平面から −100）にそろえる（梁との間に隙間を空けない）。
	std::vector<ShearWallCommand> walls{fitWall()};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 0.0}, {900.0, 0.0}, 426.0, 426.0, 105.0),
		fitBeam({900.0, 0.0}, {2500.0, 0.0}, 326.0, 326.0, 105.0),
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 3420.0, 3420.0, 240.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());
	CHECK(near(walls[0].bottomHeight, -100.0, 1e-6));
	CHECK(near(walls[0].topHeight, 2754.0, 1e-6));
	CHECK(near(walls[0].topHeightEnd, 2754.0, 1e-6));
}

TEST(shear_wall_fit_ignores_beams_off_the_axis_or_far_away)
{
	// 軸から外れた梁・直交する梁・はるか上の梁（母屋）は上下の材とみなさない。何も
	// 取れなければ IFC の高さのまま。
	std::vector<ShearWallCommand> walls{fitWall()};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 910.0}, {2500.0, 910.0}, 3420.0, 3420.0, 150.0), // 隣の通り
		fitBeam({910.0, -500.0}, {910.0, 500.0}, 3420.0, 3420.0, 150.0),  // 直交
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 6000.0, 6000.0, 105.0)};	  // はるか上
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());
	CHECK(near(walls[0].bottomHeight, 0.0, 1e-6));
	CHECK(near(walls[0].topHeight, 2700.0, 1e-6));
	CHECK(near(walls[0].topHeightEnd, 2700.0, 1e-6));
}

TEST(shear_wall_fit_leaves_other_layers_alone)
{
	// 配置先レイヤがどの階の耐力壁レイヤでもない命令は変更しない。
	ShearWallCommand wall = fitWall();
	wall.layer = "9-耐力壁";
	std::vector<ShearWallCommand> walls{wall};
	const std::vector<core::MemberCommand> members{
		fitBeam({-500.0, 0.0}, {2500.0, 0.0}, 3420.0, 3420.0, 240.0)};
	fitShearWallsToMembers(walls, fitStories(), {}, members, fitColumns());
	CHECK(near(walls[0].topHeight, 2700.0, 1e-6));
}

TEST(shear_wall_fit_braces_and_panels_share_the_frame_in_fixtures)
{
	// 同じ軸の筋かいと面材は同じ軸組に入るので、合わせ込んだ後の高さが一致する
	// （合わせ込む前の面材は横架材に掛かって上下にはみ出していた）。
	std::size_t pairs = 0;
	for (const std::string& name : allFixtures())
	{
		const Document& document = fixtureDocument(name);
		for (const ShearWallCommand& brace : document.shearWalls)
		{
			if (brace.kind != ShearWallKind::Brace)
				continue;
			for (const ShearWallCommand& panel : document.shearWalls)
			{
				if (panel.kind != ShearWallKind::Panel || panel.layer != brace.layer ||
					!core::samePoint(panel.start, brace.start) ||
					!core::samePoint(panel.end, brace.end))
					continue;
				++pairs;
				CHECK(near(panel.bottomHeight, brace.bottomHeight, 1e-6));
				CHECK(near(panel.topHeight, brace.topHeight, 1e-6));
				CHECK(near(panel.topHeightEnd, brace.topHeightEnd, 1e-6));
			}
		}
	}
	CHECK(pairs > 0); // 同じ軸の筋かいと面材が無いフィクスチャもある
}

TEST(shear_wall_fit_sample1_heights)
{
	// サンプル1 の 1 階 (2275,5460)→(3640,5460): 土台（天端 426）と軒桁（天端 3420・
	// せい 150）のあいだ。面材も筋かいも下端 0・上端 2844（レイヤ平面 426 から）。
	const Document& document = fixtureDocument("サンプル1 (住木邸新築工事).ifc");
	std::size_t found = 0;
	for (const ShearWallCommand& wall : document.shearWalls)
	{
		if (wall.layer != "1-耐力壁" || !core::samePoint(wall.start, core::Vec2{2275.0, 5460.0}) ||
			!core::samePoint(wall.end, core::Vec2{3640.0, 5460.0}))
			continue;
		++found;
		CHECK(near(wall.bottomHeight, 0.0, 1e-6));
		CHECK(near(wall.topHeight, 2844.0, 1e-6));
		CHECK(near(wall.topHeightEnd, 2844.0, 1e-6));
	}
	CHECK_EQ(found, static_cast<std::size_t>(2)); // 筋かいと面材
}

TEST_MAIN();
