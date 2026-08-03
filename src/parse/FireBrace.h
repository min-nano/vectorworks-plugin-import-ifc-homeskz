//
//	parse/FireBrace.h
//
//	Phase 1（IFC 解析）の火打（火打梁）モジュール。Python 版 ifc/fire_brace.py に
//	対応する（ROADMAP.md M11「シンボル置換系」）。火打をハイブリッドシンボル "鋼製火打"
//	へ置換する core::SymbolCommand を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）だけで
//	完結する（CLAUDE.md「Phase 1」）。
//
//	解析の要点（Python 版 CLAUDE.md「火打」節）:
//	  * 火打は Name が "火打…" の IfcBeam / IfcMember。押し出し方向が**鉛直**
//	    （Axis=(0,0,1)）で断面が IfcArbitraryClosedProfileDef（平面外形が火打の
//	    footprint）なので、横架材（parse/Member）は**鉛直軸で先に除外**していて拾わない。
//	    そこで専用にここで処理する。
//	  * **基準点は「横架材接合部の内側面交点」**。火打の平面外形は 2 本の長辺（材の長さ
//	    方向に平行）と 2 つの端面（各梁に取り付く面）からなり、2 つの端面の直線を延長した
//	    交点が内角の点になる。端面は「プロファイル局所座標の第 2 成分 v（＝中心線からの
//	    符号付き距離）が始終点で符号反転する辺」で識別できる（長辺は v が一定＝±半幅）。
//	  * **回転角は内角の二等分方向**（基準点から火打本体の重心へ向かう向き）に、シンボルの
//	    基準姿勢のずれを補正する反時計方向 45 度を加えた値。
//	  * 配置先は火打が属する階の**横架材と同じレイヤ**（一般階 "n-横架材天端"、最上階は
//	    横架材天端が無いので "R-軒高"）。**高さは命令に持たせない**——配置先レイヤの
//	    ストーリレベルが担う。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 火打を識別する Name 接頭辞（"火打:0_1" / "火打:1_2" 等。Python 版 _FIRE_BRACE_PREFIX）。
	inline constexpr const char* kFireBracePrefix = "火打";

	// 置換するハイブリッドシンボル名（Python 版 SYMBOL_FIRE_BRACE）。
	inline constexpr const char* kSymbolFireBrace = "鋼製火打";

	// 鋼製火打シンボルの基準姿勢（0 度での向き）の補正（度。Python 版
	// _SYMBOL_ANGLE_OFFSET）。内角の二等分方向に対してシンボルの角度基準がずれているため、
	// 基準点まわりに反時計方向へ 45 度回す。
	inline constexpr double kFireBraceAngleOffset = 45.0;

	// 2 直線が平行（交点なし）とみなす行列式の閾値（Python 版 _PARALLEL_TOL）。
	inline constexpr double kFireBraceParallelTol = 1e-9;

	// 要素が火打（Name が "火打" 始まりの IfcBeam / IfcMember）か（Python 版 _is_fire_brace）。
	bool isFireBrace(const Entity& element);

	// 2D 線分（端点ペア）。火打の端面を表す。
	struct Segment2D
	{
		core::Vec2 a;
		core::Vec2 b;
	};

	// 2 線分を**無限直線として延長した**交点を返す（Python 版 _segment_intersection）。
	// 平行なら nullopt。
	std::optional<core::Vec2> segmentIntersection(const Segment2D& first, const Segment2D& second);

	// 火打 footprint の 2 つの端面（梁に取り付く面）のワールド線分を返す（Python 版
	// _end_faces）。world と local は同じ並びの頂点列で、local の第 2 成分 v が始終点で
	// 符号反転する辺（＝中心線をまたぐ辺）を端面とみなす。端面はちょうど 2 つのはずだが、
	// 判定はそのまま返し、個数の検査は fireBraceBasePoint が行う。
	std::vector<Segment2D> fireBraceEndFaces(const std::vector<core::Vec2>& world,
											 const std::vector<core::Vec2>& local);

	// 2 つの端面の直線を延長した交点（内角の点＝シンボルの基準点）を返す（Python 版
	// _base_point）。端面がちょうど 2 つでない・平行で交点が定まらないときは nullopt。
	std::optional<core::Vec2> fireBraceBasePoint(const std::vector<Segment2D>& faces);

	// 火打の向きに合わせた回転角（度）を返す（Python 版 _angle）。基準点（内角）から
	// 火打本体の重心へ向かう方向（内角の二等分方向）に kFireBraceAngleOffset を加える。
	// world が空なら 0（呼び出し側は空の外形をここへ渡さない）。
	double fireBraceAngle(const core::Vec2& base, const std::vector<core::Vec2>& world);

	// STEP Model から火打のシンボル配置命令を組み立てる（Python 版
	// build_fire_brace_commands）。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、各階に含まれる
	// 火打を解析する。配置先レイヤは一般階が "n-横架材天端"、最上階が "R-軒高"。座標は
	// 通り芯・横架材と同じグリッド中心オフセットで補正する。押し出しソリッド・端面交点を
	// 解決できない火打はスキップする（1 本の欠損で全体を止めない）。
	//
	// 並びは階（Elevation 昇順）→ 階内は要素の出現順（#id 昇順の rel 由来）で、エンティティ
	// 列挙順に依存しない決定的な結果になる。
	std::vector<core::SymbolCommand> buildFireBraceCommands(const Model& model);

	// 同上。共有コンテキストのストーリ一覧・センタリング中心・階の要素を使う（parse/Context.h）。
	std::vector<core::SymbolCommand> buildFireBraceCommands(Context& context);
} // namespace HomeskzIfcImport::parse
