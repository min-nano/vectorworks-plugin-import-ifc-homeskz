//
//	parse/FloorPost.h
//
//	Phase 1（IFC 解析）の床束モジュール。Python 版 ifc/floor_post.py に対応する
//	（ROADMAP.md M11「シンボル置換系」）。床束をハイブリッドシンボル "床束" として
//	配置する core::SymbolCommand を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・横架材の配置／断面（parse/Member）・構造クラス
//	（parse/StructuralClass）・基礎の有無（parse/Footing）だけで完結する。
//
//	解析の要点（Python 版 CLAUDE.md「床束」節）:
//	  * **ホームズ君 EX の IFC に床束は出力されない**（オブジェクト・型・プロパティの
//	    いずれにも現れない）。したがって IFC から抽出できず、要件どおり**大引の下に
//	    一定間隔（910mm＝半間）で決め打ち配置**する。
//	  * **継手の統合**: ホームズ君 IFC の大引は継手（支持材の上での継目）で分断されて
//	    いるが、継手は実際の大引の端ではない。同一直線上で継手（すき間 ≤ 半モジュール）に
//	    分断された大引は 1 連に統合してから間隔を割り付ける（mergeCollinearOhbiki）。
//	    同一直線上でも 1 モジュール以上離れた別々の大引は統合しない。
//	  * **端部は実部材端ではなく「支持材芯」**。ホームズ君 IFC の大引は端部が支持材芯より
//	    半支持材厚だけ内側に納まって描かれており（単モジュール＝910mm 区間で実長 805mm＝
//	    910−105）、実部材端を基準にすると床束が実際より内側へ寄る。各端で大引の芯線と交わる
//	    支持材（土台・**他の大引**）の芯線の交点を端部として扱う（shinReference）。他の大引を
//	    含めるのは、二次大引（他の大引に載る大引）の端も受ける大引の芯を基準にするため。
//	    どの支持材にも受けられていない端（基礎に直接載る端等）は実部材端へフォールバックする。
//	  * **配置**: 始点側の支持材芯から 910mm・1820mm・… と並べる。最後の床束と終点側の
//	    支持材芯との間隔は 910mm 未満の半端でよい。支持材芯そのものには置かない（端部は
//	    支持材が受ける）ので、支持材芯区間が 910mm 以下の大引には床束が 0 本になる。
//	  * **高さは命令に持たせない**——基準は基礎底盤上端（底盤天端）で、配置先レイヤ
//	    "F-床束" のストーリレベルが担う。
//	  * **基礎が無いモデルでは空**（parse/Footing の hasFoundation）。配置先レイヤも
//	    高さ基準も定まらないため。
//
//	【配置先レイヤ】"F-床束" は**基礎ストーリの床束レベル**（底盤天端に揃えた高さ）に紐づく
//	デザインレイヤで、これを作るのは parse/Footing の buildFoundationStoryCommand（M9 の
//	基礎ストーリに M11 で床束レベルを足した）。レイヤ名の定数は parse/Footing.h に 1 つだけ
//	置き、配置先を名乗る側（ここ）とレベルを作る側の両方がそれを通る。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <optional>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 置換するハイブリッドシンボル名（Python 版 SYMBOL_FLOOR_POST）。
	inline constexpr const char* kSymbolFloorPost = "床束";

	// 床束の配置間隔（mm）。IFC に床束が無いための決め打ち値（半間＝910mm。Python 版
	// _POST_INTERVAL）。
	inline constexpr double kFloorPostInterval = 910.0;

	// 支持材芯の探索許容値（mm。Python 版 _PARALLEL_TOL / _SEG_TOL / _SHIN_MARGIN）。
	inline constexpr double kFloorPostParallelTol = 1e-9; // 芯線がほぼ平行な支持材は対象外
	inline constexpr double kFloorPostSegTol = 1.0; // 交点が支持材区間からはみ出す余裕
	inline constexpr double kFloorPostShinMargin = 1.0; // 大引端が支持材 footprint に載る余裕

	// 同一直線上の大引の継手（継目）判定の許容値（Python 版 _COLLINEAR_ANGLE_TOL /
	// _COLLINEAR_PERP_TOL / _JOINT_GAP_TOL）。
	inline constexpr double kCollinearAngleTol = 1e-6; // 方向の外積（sin 角）がこれ以下なら平行
	inline constexpr double kCollinearPerpTol = 1.0; // 相手端の芯線からの直交距離
	// 継手のすき間（支持材幅 ≈ 105mm）は半モジュール（455mm）を大きく下回り、別々の大引の
	// 間隔（≥ 1 モジュール ≈ 1000mm）を大きく下回るので、この値で継手と別材を切り分けられる。
	inline constexpr double kJointGapTol = kFloorPostInterval / 2.0;

	// 支持材 1 本の平面芯線（Python 版 _SupportLine）。
	//   origin    … 芯線の始点
	//   direction … 単位方向
	//   length    … 芯線長
	//   width     … 断面幅（大引端がこの半分＋余裕以内なら「載っている」とみなす）
	struct SupportLine
	{
		core::Vec2 origin;
		core::Vec2 direction;
		double length = 0.0;
		double width = 0.0;
	};

	// 大引 1 本（または継手で統合した 1 連）の平面芯線（Python 版 _OhbikiRun）。
	struct OhbikiRun
	{
		core::Vec2 start;
		core::Vec2 end;
	};

	// 支持材芯区間 1 つに沿った床束の配置位置（始点側の支持材芯からの距離）を返す
	// （Python 版 _post_offsets）。910mm・1820mm・… と並べ、終点ちょうど以遠には置かない。
	std::vector<double> floorPostOffsets(double length);

	// 大引を受ける支持材（土台・大引）の平面芯線を集める（Python 版
	// _collect_support_lines）。座標はグリッド中心オフセット**前**の生値。土台だけでなく
	// 他の大引も含めることで、二次大引の端も支持材芯を基準にできる（自身の芯線・同一直線上の
	// 大引は shinReference の平行判定で除外される）。
	std::vector<SupportLine> collectSupportLines(const Model& model);

	// 大引（CLASS_OOBIKI）の平面芯線を集める（Python 版 _collect_ohbiki_lines）。
	// 座標はグリッド中心オフセット**前**の生値。
	std::vector<OhbikiRun> collectOhbikiLines(const Model& model);

	// 大引端 point を受けている支持材（土台・大引）の芯（交点）を返す（Python 版
	// _shin_reference）。大引の芯線（点 point・方向 direction）と各支持材の芯線の交点のうち、
	// 交点が支持材の区間内にあり、かつ大引端から半支持材厚（＋ kFloorPostShinMargin）以内に
	// あるものの中で最も近い交点を返す。平行な支持材（自身の芯線・同一直線上の大引を含む）は
	// 交点が定まらないため除外する。受けている支持材が無ければ nullopt。
	std::optional<core::Vec2> shinReference(const core::Vec2& point, const core::Vec2& direction,
											const std::vector<SupportLine>& supports);

	// 大引 a・b が同一直線上にあるとき、区間のすき間（重なり／接触は 0）を返す（Python 版
	// _collinear_gap）。平行でない／別の直線上にある（直交距離が大きい）ときは nullopt。
	std::optional<double> collinearGap(const OhbikiRun& first, const OhbikiRun& second);

	// 同一直線上で継手（すき間 ≤ kJointGapTol）の大引を 1 連に統合する（Python 版
	// _merge_collinear_ohbiki）。Union-Find で連結成分にまとめ、各成分を先頭の芯線方向へ
	// 全端点を射影した最小〜最大区間の 1 本にする。統合は入力順に依存しない（代表は最小
	// インデックス、出力は代表インデックス昇順）。
	std::vector<OhbikiRun> mergeCollinearOhbiki(const std::vector<OhbikiRun>& lines);

	// STEP Model から床束のシンボル配置命令を組み立てる（Python 版
	// build_floor_post_commands）。基礎が無いモデルでは空を返す。並びは大引の連ごと
	// （collectOhbikiLines の #id 昇順に由来）→ 連内は始点からの距離順で決定的。
	std::vector<core::SymbolCommand> buildFloorPostCommands(const Model& model);

	// 同上。共有コンテキストのセンタリング中心を使う（parse/Context.h）。
	std::vector<core::SymbolCommand> buildFloorPostCommands(Context& context);
} // namespace HomeskzIfcImport::parse
