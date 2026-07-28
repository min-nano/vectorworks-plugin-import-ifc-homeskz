//
//	parse/IfcGeometry.h
//
//	IFC の配置・断面・押し出しを自前で解決する幾何ユーティリティ（ROADMAP.md M2
//	「幾何の土台」）。Python 版が ifcopenshell の行列計算に頼らず手計算している部分
//	（ifc/member.py の _get_placement_3d / _get_profile_dims、ifc/footing.py の
//	_world_solid 相当）を C++ へ移植する。M3 以降のほぼ全要素がここを共有するので、
//	描画を伴わずに先に固めて de-risk する。
//
//	扱う IFC エンティティ（ホームズ君 IFC の既知サブセット。IFC2X3 / IFC4 共通の
//	座標系ジオメトリ）:
//	  * IfcDirection / IfcCartesianPoint      … 方向・点
//	  * IfcAxis2Placement3D / …2D             … ローカル座標系（原点＋基底）
//	  * IfcLocalPlacement                     … 親を辿るワールド配置（再帰合成）
//	  * IfcRectangleProfileDef                … 矩形断面（XDim / YDim）
//	  * IfcArbitraryClosedProfileDef          … 任意閉断面（外形ポリライン）
//	  * IfcExtrudedAreaSolid                  … 押し出しソリッド
//	  * IfcBooleanResult / …ClippingResult    … 差演算の第 1 オペランド（素ソリッド）
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md
//	「Phase 1」）。STEP グラフ（parse/Step）と自前幾何型（core/Geometry）だけに依存。
//
//	寛容さ（CLAUDE.md「エラーハンドリング」）: 解決できない参照・欠損属性・想定外の型は
//	既定値（単位行列など）へフォールバックするか false を返し、1 要素の欠損で全体を
//	止めない。数値は Step の asReal を通すので変換失敗も 0 に落ちる。
//

#pragma once

#include "core/Geometry.h"
#include "parse/Step.h"

#include <vector>

namespace HomeskzIfcImport::parse
{
	using core::Mat4;
	using core::Vec2;
	using core::Vec3;

	// IfcDirection の DirectionRatios を Vec3 で返す（Z が無ければ 0）。参照が解決
	// できない・座標が足りないときは false（out は変更しない）。
	bool resolveDirection(const Model& model, const Value& ref, Vec3& out);

	// IfcCartesianPoint の Coordinates を Vec3 で返す（Z が無ければ 0、2D 点も可）。
	// 参照が解決できない・座標が 2 つ未満のときは false。
	bool resolvePoint(const Model& model, const Value& ref, Vec3& out);

	// IfcAxis2Placement3D → ローカル変換行列。Axis(=局所 Z)・RefDirection(≈局所 X)から
	// Gram-Schmidt で正規直交基底を作る（X = 正規化(RefDir − (RefDir·Z)Z)、Y = Z×X）。
	// Axis 省略時は Z=(0,0,1)、RefDirection 省略時は X=(1,0,0)。placement が nullptr /
	// 型不一致 / 基底が縮退するときは単位行列（原点のみ反映）を返す。
	Mat4 resolveAxis2Placement3D(const Model& model, const Entity* placement);

	// IfcAxis2Placement2D → ローカル変換行列（Z=(0,0,1) 固定の 2D 回転＋平行移動）。
	// RefDirection(≈局所 X) から X を正規化し、Y=(−X.y, X.x)（左 90°回転）を取る。
	// 省略時は X=(1,0)。断面プロファイルの Position に使う。
	Mat4 resolveAxis2Placement2D(const Model& model, const Entity* placement);

	// IfcLocalPlacement → ワールド変換行列。PlacementRelTo を親として再帰合成する
	// （ワールド = 親ワールド × RelativePlacement）。循環参照・過大な深さは深さ上限で
	// 打ち切り、それ以降は単位行列とみなす（無限再帰を防ぐ）。placement が nullptr /
	// 型不一致なら単位行列。
	Mat4 resolveLocalPlacement(const Model& model, const Entity* placement);

	// 断面プロファイル（2D、プロファイル定義のローカル座標系）。outer は閉じた外形の
	// 頂点列で、末尾に始点を重複させない（N 頂点なら N 点）。矩形は rectangle=true と
	// xDim/yDim を併せて持つ（描画側が PIO 寸法に使えるように）。頂点の周り方向
	// （時計 / 反時計）は入力のまま保持し、正規化は各要素側に委ねる。
	struct Profile
	{
		std::vector<Vec2> outer;
		bool rectangle = false; // IfcRectangleProfileDef のとき true
		double xDim = 0.0;		// rectangle のときの XDim（幅）
		double yDim = 0.0;		// rectangle のときの YDim（高さ）
	};

	// IfcProfileDef（IfcRectangleProfileDef / IfcArbitraryClosedProfileDef）を解決して
	// 2D 外形を得る。矩形は Position(2D) を適用した 4 隅を outer に並べ、任意断面は
	// OuterCurve(IfcPolyline) の点列をそのまま並べる（始点と終点が一致する場合は重複を
	// 1 つ落として閉ループにする）。未対応の型・欠損・点数不足は false。
	bool resolveProfile(const Model& model, const Entity* profileDef, Profile& out);

	// 押し出しソリッド（ワールド座標）。base は底面ループ、extrusion は押し出しベクトル
	// （方向×深さ）。天面は base[i] + extrusion で得られる（top() が返す）。
	struct WorldSolid
	{
		std::vector<Vec3> base;
		Vec3 extrusion;

		// 天面ループ（base の各点に extrusion を加えたもの）。
		std::vector<Vec3> top() const;
	};

	// IfcExtrudedAreaSolid をワールド座標のソリッドへ変換する。
	//   placement … 対象要素の ObjectPlacement（IfcLocalPlacement）から得たワールド行列。
	//               solid.Position はこの上に合成される（ワールド = placement × Position）。
	//               要素配置を持たない単体テストでは単位行列を渡せばオブジェクト座標系で
	//               得られる。
	// 押し出し方向（ExtrudedDirection、単位化して Depth を掛ける）も同じ行列で世界系へ
	// 変換する。SweptArea の解決に失敗した・型が押し出しでないときは false。
	bool resolveExtrudedAreaSolid(const Model& model, const Entity* solid, const Mat4& placement,
								  WorldSolid& out);

	// IfcBooleanResult / IfcBooleanClippingResult の FirstOperand を素のソリッドまで
	// 辿る（差演算で削られる前の基のソリッドを取り出す。Python 版 footing の
	// 「第 1 オペランドを辿る」に対応）。boolean でない要素はそれ自身を返す。参照が
	// 解決できない・深さ上限に達したときは nullptr。
	const Entity* resolveBaseSolid(const Model& model, const Entity* item);
} // namespace HomeskzIfcImport::parse
