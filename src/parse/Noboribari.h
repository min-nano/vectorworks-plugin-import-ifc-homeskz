//
//	parse/Noboribari.h
//
//	Phase 1（IFC 解析）の登り梁（傾斜梁）位置補正モジュール。Python 版 ifc/noboribari.py に
//	対応する（ROADMAP.md M7）。
//
//	【なぜ後処理なのか】ホームズ君 IFC の登り梁は**位置が正確でない**:
//	  * 軸の勾配が屋根版（垂木下面）より急で、天端が中央付近で屋根面と交わり両端で上下に
//	    ずれる（実データで屋根勾配 0.321 に対し登り梁 0.346／0.331）。
//	  * 端部（直切り＝鉛直面）が受ける材（横架材・母屋）の footprint へ数 mm 食い込む。
//	そこで parse/Member が組み立てた命令を、**屋根面を基準に**後から整える。屋根版への依存を
//	parse/Member へ持ち込まないための分離で、垂木・野地板が屋根版から導出されるのと同じく
//	「形状（屋根面）へ支持部材を合わせる」というロードマップの方針そのものになっている
//	（ROADMAP.md「実装順序の方針（形状先行）」）。
//
//	補正は 2 段（Python 版と同じ順序）:
//	  1. **端部の食い込み解消**: 端点を梁軸に沿って内側へ引き戻し、鉛直な端面を受ける材の
//	     手前の面に合わせる（parse/Member の memberPenetrationDepth を再利用）。
//	  2. **屋根勾配へのスナップ**: 天端中央線の両端（詰めた後の XY）を、その真上にある屋根版
//	     （parse/IfcGeometry の roofPlane。M6 で確定した屋根面と同じもの）の平面へ落とし、
//	     勾配・高さを屋根面＝垂木下面に一致させる（バインド offset も更新する）。屋根面が
//	     見つからない登り梁は parse/Member の直切りの幾何（フォールバック）のまま残す。
//
//	件数・並び順は保つ（後続 M のタグが命令インデックスで横架材を参照するため）。判定は
//	命令の並び順に依存しない決定的な結果になる。
//
//	【M7 のスコープ】受ける材は**横架材だけ**を見る。柱（M8）を参照する端部詰めの最終化は、
//	柱の導入時に行う（ROADMAP.md M7「端部詰めは…柱導入後（M8）に最終化する」）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/IfcGeometry.h"
#include "parse/Step.h"

#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 端部の食い込み解消・屋根スナップの許容値（mm）。Python 版 ifc/noboribari.py と同値。
	inline constexpr double kNoboribariZOverlapTol = 1.0; // これ以下の Z 重なりは取り合いでない
	inline constexpr double kNoboribariMinTrim = 0.5; // これ未満の食い込みは詰めない
	inline constexpr double kNoboribariMinLength = 1.0; // 詰めた後にこの長さ未満なら詰めない

	// 屋根面の法線水平成分と登り梁の勾配方向の内積（単位）の下限。屋根面の勾配方向が登り梁の
	// 勾配方向と平行（この値以上）な屋根版だけを、その登り梁の屋根面とみなす
	// （Python 版 _SLOPE_DIR_DOT）。
	inline constexpr double kNoboribariSlopeDirDot = 0.9;

	// 登り梁のスナップ先となる屋根面 1 面。勾配座標系（parse/IfcGeometry の RoofSlope）を
	// そのまま持つので、天端 Z の式と勾配方向は**垂木・野地板とまったく同じ計算**になる。
	//   plan            … 平面外形のワールド XY（RoofSlope::plan の結果）
	//   storeyElevation … その屋根版が属する階の Elevation（ストーリ相対 Z を絶対 Z にする）
	struct NoboribariRoofPlane
	{
		RoofSlope slope;
		std::vector<core::Vec2> plan;
		double storeyElevation = 0.0;

		// 平面上のワールド点 (x, y) の絶対 Z。
		double zAt(double x, double y) const;

		// ワールド XY が平面外形の内側か（走査線法）。
		bool contains(double x, double y) const;
	};

	// FL ストーリの屋根版（parse/Rafter の isRoofSlab）から屋根面を集める
	// （Python 版 _collect_roof_planes）。勾配方向が定まらない面（ほぼ水平／鉛直）は除く
	// （判定は roofSlope が担うので、垂木・野地板と同じ面だけが集まる）。
	std::vector<NoboribariRoofPlane> collectRoofPlanes(Context& context);

	// 登り梁の真上にある屋根面を返す（Python 版 _roof_plane_for）。勾配方向（始端→終端の
	// 水平単位ベクトル）と屋根面の勾配方向が平行（kNoboribariSlopeDirDot 以上）で、外形が
	// 登り梁の中点（取れなければ端点）を内包する最初の面。命令座標はセンタリング済みなので
	// center を足してワールドへ戻して判定する。見つからなければ nullptr。
	const NoboribariRoofPlane* roofPlaneFor(const core::MemberCommand& command,
											const std::vector<NoboribariRoofPlane>& planes,
											const core::Vec2& center);

	// 登り梁の端点 point・外向き outward を、受ける材（横架材）の面まで詰める量を返す
	// （Python 版 _end_trim。柱は M8 で足す）。Z 範囲 [zBottom, zTop] が重なる材だけを見て、
	// 食い込み量の最大値を返す。平行な材（継ぎ手・側並び）は 0 になる。
	double noboribariEndTrim(const core::Vec2& point, const core::Vec2& outward, double zBottom,
							 double zTop, const std::vector<core::MemberCommand>& receivers);

	// 登り梁命令 1 件を補正する（Python 版 _correct_one）。端部の食い込みを詰めてから、
	// 屋根面が見つかれば天端をその面へスナップする（バインド offset も更新）。平面投影長が
	// 極小の命令はそのまま返す。
	core::MemberCommand correctOneNoboribari(const core::MemberCommand& command,
											 const std::vector<NoboribariRoofPlane>& planes,
											 const std::vector<core::MemberCommand>& receivers,
											 const core::Vec2& center);

	// 横架材命令のうち登り梁だけを補正した新しいリストを返す（Python 版 correct_noboribari）。
	// 登り梁でない材は素通しし、件数・並び順は保つ。受ける材は「登り梁でない横架材」。
	std::vector<core::MemberCommand>
	correctNoboribari(Context& context, const std::vector<core::MemberCommand>& members);

	// 同上（コンテキストを内部で 1 つ作って捨てる。単体テスト用）。
	std::vector<core::MemberCommand>
	correctNoboribari(const Model& model, const std::vector<core::MemberCommand>& members);
} // namespace HomeskzIfcImport::parse
