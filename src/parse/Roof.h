//
//	parse/Roof.h
//
//	Phase 1（IFC 解析）の野地板モジュール。Python 版 ifc/roof.py に対応する。
//
//	野地板（屋根の下地合板）は屋根面そのものに沿う 1 枚の面材なので、垂木と同じ
//	**屋根版（Name が "屋根版" 始まりの IfcSlab）1 面ごと**に、その勾配・外形から単勾配の
//	屋根オブジェクトを作図する（**屋根版 1 面 = 野地板 1 枚**。垂木のように 455mm 間隔で
//	割らない）。要件により**厚みは 12mm 固定**（IFC に野地板固有の厚み情報が無いため）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。屋根面の取り出しは垂木と
//	同じ parse/IfcGeometry の roofPlane を共有する（いずれも屋根版から屋根面を得る）。
//
//	組み立てる内容（Python 版 CLAUDE.md「野地板」節。docs/DEV-NOTES.md M6）:
//	  * **軒（屋根軸）**… 屋根面の最も低い（最も軒側＝勾配方向 d への射影が最大の）頂点を
//	    通り、軒に平行な方向 e へ footprint の広がりぶん伸ばした線分。屋根オブジェクトは
//	    この軸から棟側（upslope）へ勾配なりに立ち上がるので、footprint 全体が軸の棟側に
//	    来るようにする。
//	  * **勾配**… 屋根面の単位法線の水平成分 dh を rise、鉛直成分 nz を run にする
//	    （slope = rise/run = tanθ）。
//	  * **高さ**… 軒（軸）の絶対 Z。**野地板は垂木の上に載る（野地板下端＝垂木上端）**ため、
//	    屋根版の平面（＝垂木下面。Python 版が VW 上の実測で確認）から**垂木せいを鉛直換算
//	    （÷cosθ＝単位法線の鉛直成分 nz）して持ち上げた値**にする（勾配があるため、屋根面に
//	    直交する寸法を鉛直へ勾配補正する）。描画フェーズは屋根の実測軸 Z との差分でこの
//	    高さへ移動する。
//	  * **配置先レイヤ**… 屋根版を含むストーリの垂木レイヤの直上に独立させた "n-野地板"
//	    （垂木と同じく屋根版の有無 storyHasRoofSlab で判定＝parse/Story が "野地板" レベルを
//	    作る条件と一致させる）。
//

#pragma once

#include "core/Document.h"
#include "parse/IfcGeometry.h"
#include "parse/Step.h"

#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 野地板の厚み（mm）。要件により 12mm 固定（Python 版 NOJIITA_THICKNESS と同値。IFC に
	// 野地板固有の厚み情報が無いための決め打ち）。
	inline constexpr double kNojiitaThickness = 12.0;

	// 野地板レベル・レイヤの名前（Python 版 LEVEL_NOJIITA）。配置先レイヤは "{接頭辞}-野地板"。
	// 文字列の定義は core/Document.h（命令セットの語彙）にあり、ここはその再公開
	// （レベル種別名の置き場所は parse/Story.h の kLevelFL ほかと同じ流儀）。
	inline constexpr const char* kLevelNojiita = core::kLevelNojiita;

	// 1 つの屋根面（parse/IfcGeometry の RoofPlane）から野地板（roof）命令を組み立てる
	// （Python 版 _roof_command_for_plane 相当）。
	//   plane           … 屋根面（平面外形頂点列＋上向き単位法線。Z はストーリ相対）
	//   layer           … 配置先デザインレイヤ名（"n-野地板"）
	//   storeyElevation … ストーリ高さ（mm）。軒の天端 Z をこれで絶対値にする
	//   center          … グリッド中心オフセット（通り芯・垂木と同じセンタリング）
	// ほぼ水平な面（勾配方向が定まらない）・法線が水平な鉛直面（勾配・天端 Z が定まらない）・
	// 広がりが極小の面は std::nullopt（屋根オブジェクトを作らない。垂木と同じ扱い）。
	std::optional<core::RoofCommand> roofCommandForPlane(const RoofPlane& plane,
														 const std::string& layer,
														 double storeyElevation,
														 const core::Vec2& center);

	// STEP Model から野地板の描画命令を組み立てる（Python 版 build_roof_commands 相当）。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、各階に含まれる
	// 屋根版（Name が "屋根版" 始まりの IfcSlab）1 面ごとに 1 枚の命令を作る。配置先レイヤは
	// その階の "n-野地板"。屋根面を解決できない／退化した屋根版はスキップする（1 面の欠損で
	// 全体を止めない。CLAUDE.md「エラーハンドリング」）。
	//
	// 並びは階（Elevation 昇順）→ 階内は要素の出現順で、エンティティ列挙順に依存しない
	// 決定的な結果になる。
	std::vector<core::RoofCommand> buildRoofCommands(const Model& model);

	// 同上。共有コンテキストのストーリ一覧・センタリング中心・屋根面を使う（parse/Context.h）。
	std::vector<core::RoofCommand> buildRoofCommands(Context& context);
} // namespace HomeskzIfcImport::parse
