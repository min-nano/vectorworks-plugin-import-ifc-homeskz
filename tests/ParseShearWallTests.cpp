//
//	ParseShearWallTests.cpp
//
//	耐力壁解析（src/parse/ShearWall）の単体テスト。VectorWorks SDK を一切 include せず、無 SDK
//	のテストハーネス（TestFramework.h）で走る（CLAUDE.md「テスト方針」）。**期待値は手書きで持
//	つ**（他の実装の出力と機械的に突き合わせることはしない）。
//
//	検証項目（docs/DEV-NOTES.md M19）: 筋かい・面材の判別（Name 接頭辞＋エンティティ型）・
//	壁面座標への落とし込み（軸＝押し出し方向の直交・軸の向きの決定性・材厚・見付け幅・
//	傾きの向き）・鉛直押し出しを弾くこと・たすき掛けの同名まとめ・表裏の面材のまとめ・
//	配置先レイヤ・レイヤ平面からの相対高さ・決定性・全フィクスチャの通し、そして
//	耐力壁レベル（"n-耐力壁"）と伏図の表示レイヤに載ること。実フィクスチャのパスは CMake が
//	HOMESKZ_FIXTURES_DIR で渡す。
//

#include "Fixtures.h"
#include "TestFramework.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/BuildDocument.h"
#include "parse/Loader.h"
#include "parse/ShearWall.h"
#include "parse/Story.h"

#include <algorithm>
#include <cstddef>
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
using HomeskzIfcImport::parse::isDoubleBrace;
using HomeskzIfcImport::parse::isShearBrace;
using HomeskzIfcImport::parse::isShearPanel;
using HomeskzIfcImport::parse::loadIfcFromText;
using HomeskzIfcImport::parse::Model;
using HomeskzIfcImport::parse::resolveShearWallPiece;
using HomeskzIfcImport::parse::ShearWallPiece;
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
	// たすき掛けも「筋かい」始まりなので筋かいとして拾い、種別だけを別に見る。
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
	// そちらを軸と見なすと 2 倍近くに化ける**——回転キャリパの最小幅で測る。
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
	// この形で、名前が違うので拾いはしないが、幾何の関門としても閉じておく。
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
	// 落とした時点で弾く（1 枚の異常で全体を止めないための関門）。
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
	Document document;
	document.shearWalls = walls;
	CHECK(core::validateDocument(document));
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
	// 別々の耐力壁でなければならない（表裏のまとめが暴発しないこと）。
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
	// 端は柱芯へ寄せてある。柱の命令と突き合わせて、少なくとも大半の端が柱の位置に
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
	// 死んでいたら気付けるように、まとめて確かめる。
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

TEST(shear_wall_floor_plan_shows_the_storey_below)
{
	// 2 階床伏図（番号 3）は 1 階の耐力壁を映す——その図の梁を下から支える階のものが読みたい。
	const Document& document = fixtureDocument("サンプル1 (住木邸新築工事).ifc");
	bool checked = false;
	for (const core::SheetCommand& sheet : document.sheets)
	{
		if (sheet.title != "2階床伏図")
			continue;
		checked = true;
		CHECK(std::ranges::find(sheet.viewport.layers, std::string("1-耐力壁")) !=
			  sheet.viewport.layers.end());
		// 自分の階（2 階）の耐力壁は載せない（切断より上になる）。
		CHECK(std::ranges::find(sheet.viewport.layers, std::string("2-耐力壁")) ==
			  sheet.viewport.layers.end());
	}
	CHECK(checked);
}

TEST_MAIN();
