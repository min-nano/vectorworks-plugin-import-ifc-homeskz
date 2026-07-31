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

	// 同上を平面座標で返す（3D 点でも Z は捨てる）。通り芯のように平面だけで決まる要素が
	// 使う（resolvePoint の薄いラッパーで、判定規則は完全に同じ）。
	bool resolvePoint2D(const Model& model, const Value& ref, Vec2& out);

	// IfcAxis2Placement3D → ローカル変換行列。Axis(=局所 Z)・RefDirection(≈局所 X)から
	// Gram-Schmidt で正規直交基底を作る（X = 正規化(RefDir − (RefDir·Z)Z)、Y = Z×X）。
	// Axis 省略時は Z=(0,0,1)、RefDirection 省略時は X=(1,0,0)。placement が nullptr /
	// 型不一致なら単位行列（原点のみ反映）を返す。Python 版 footing._axis_placement と一致。
	//
	// ［Python 版との差異・意図的］RefDirection が Axis と平行で基底が縮退する場合、Python は
	// X=(1,0,0) に固定する（Axis が Z でないと基底が非直交になる）。本実装は Axis に直交する
	// X へフォールバックして正規直交を保つ。ホームズ君 IFC では RefDirection は常に Axis と
	// 直交（多くは省略）するため縮退は起きず、実データでの出力は完全に一致する。
	Mat4 resolveAxis2Placement3D(const Model& model, const Entity* placement);

	// 要素（IfcProduct）の ObjectPlacement から配置行列を返す。
	//
	// ★重要（Python 版と一致させるための設計）: ObjectPlacement(IfcLocalPlacement) の
	//   RelativePlacement（要素自身の IfcAxis2Placement3D）だけを使い、**親 PlacementRelTo は
	//   合成しない**。ホームズ君 IFC は要素座標を（親＝階/建物の配置ではなく）階基準で直接
	//   与えており、階の高さ（親配置の Z オフセット）は描画フェーズがストーリバウンドで別途
	//   反映する。親を合成すると階高が二重計上される（実測: ある柱で要素 Z=−174 に親階の
	//   Z=+600 が乗り +426 になってしまう）。Python 版は member/footing/story/column の全てで
	//   要素自身の RelativePlacement のみを読み、PlacementRelTo を一切辿らない。これに揃える。
	//
	// ObjectPlacement は IfcProduct の属性 5（GlobalId, OwnerHistory, Name, Description,
	// ObjectType, ObjectPlacement, Representation, …）。解決できない・型不一致なら単位行列。
	//
	// ［M7/M8 への注意］最終的な要素高さ（elevation）は「ストーリ高さ ＋ ローカル配置 Z」で
	// 決まる（Python 版 column.py 等）。本行列の Z はローカル配置 Z のみを表すので、階の高さは
	// 各要素の描画側で別途足す。また Python 版 _get_placement_3d は Location が 2 座標のとき Z を
	// 「未設定（レイヤ基準高さへフォールバック）」として扱い、梁軸方向に Axis を使う。これらの
	// 要素固有の解釈は M7 横架材・M8 柱の要素解析で行い、本関数は純粋な配置行列だけを返す。
	Mat4 resolveObjectPlacement(const Model& model, const Entity* element);

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
	// 2D 外形を得る。Python 版 footing._profile_points と一致させる:
	//   * 矩形は中心原点の 4 隅（−hx,−hy）(hx,−hy)(hx,hy)(−hx,hy) に Position の**平行移動
	//     のみ**（Location 座標）を足す。RefDirection の回転は反映しない（Python 版に合わせる。
	//     ホームズ君 IFC の矩形断面 Position は RefDirection を持たないので実データでは同一）。
	//   * 任意断面は OuterCurve(IfcPolyline) の点列をそのまま。始点＝終点の重複は 1 つ落とす。
	// 未対応の型・欠損・点数不足は false。
	bool resolveProfile(const Model& model, const Entity* profileDef, Profile& out);

	// 押し出しソリッドのワールド情報（Python 版 footing._Solid に対応）。配置基底
	// （origin/xAxis/yAxis/zAxis = Python の (origin, lX, lY, lZ)）・押し出し単位方向
	// （extrudeDir = Python の extrude）・押し出し長（depth）・プロファイル 2D 頂点
	// （profile = Python の pts）・矩形寸法（rectangle/xDim/yDim = Python の dims）を保持する。
	// ワールド底面点はプロファイル頂点 (u,v) を origin + xAxis·u + yAxis·v で写して得る
	// （base() が返す。M9 基礎の _footprint / _z_top_and_thickness、M6/M7 の傾斜部材
	// （_sloped_member_geometry）をこの情報から直接移植できるよう、2D プロファイルと基底を
	// 分けて残す）。
	struct WorldSolid
	{
		Vec3 origin;	 // 配置原点（ワールド）
		Vec3 xAxis;		 // 局所 X 軸（ワールド。Python lX）
		Vec3 yAxis;		 // 局所 Y 軸（ワールド。Python lY）
		Vec3 zAxis;		 // 局所 Z 軸（ワールド。Python lZ）
		Vec3 extrudeDir; // 押し出し単位方向（ワールド。Python extrude）
		double depth = 0.0;
		std::vector<Vec2> profile; // プロファイル 2D 頂点（Python pts）
		bool rectangle = false;
		double xDim = 0.0;
		double yDim = 0.0;

		// プロファイル頂点をワールド底面へ写す（origin + xAxis·u + yAxis·v）。
		std::vector<Vec3> base() const;
		// 押し出しベクトル（extrudeDir · depth）。
		Vec3 extrusion() const;
		// 天面ループ（base の各点に extrusion を加えたもの）。
		std::vector<Vec3> top() const;
	};

	// IfcExtrudedAreaSolid をワールド座標のソリッド情報へ変換する。
	//   placement … 対象要素の配置行列（resolveObjectPlacement の戻り。要素自身の
	//               RelativePlacement のみ＝親非合成）。solid.Position はこの上に合成される
	//               （Python 版 _compose(element_pl, item_pl) に対応）。要素配置を持たない
	//               単体テストでは単位行列を渡せばオブジェクト座標系で得られる。
	// 押し出し方向（ExtrudedDirection、単位化）を同じ基底で世界系へ変換し extrudeDir に、
	// Depth を depth に入れる。SweptArea の解決に失敗した・型が押し出しでないときは false。
	bool resolveExtrudedAreaSolid(const Model& model, const Entity* solid, const Mat4& placement,
								  WorldSolid& out);

	// IfcBooleanResult / IfcBooleanClippingResult の FirstOperand を素のソリッドまで
	// 辿る（差演算で削られる前の基のソリッドを取り出す。Python 版 footing の
	// 「第 1 オペランドを辿る」に対応）。boolean でない要素はそれ自身を返す。参照が
	// 解決できない・深さ上限に達したときは nullptr。
	const Entity* resolveBaseSolid(const Model& model, const Entity* item);

	// 要素（IfcProduct）の形状表現から最初の IfcExtrudedAreaSolid を返す（Python 版
	// footing._first_extruded_solid 相当）。Representation → Representations →
	// Items を順に辿り、各アイテムは resolveBaseSolid で差演算を剥がしてから押し出し
	// かどうかを見る。見つからなければ nullptr（1 要素の欠損で全体を止めない）。
	const Entity* firstExtrudedSolid(const Model& model, const Entity* element);

	// 要素の押し出しソリッドをワールド座標へ変換する（Python 版 footing._world_solid
	// 相当）。firstExtrudedSolid ＋ resolveObjectPlacement ＋ resolveExtrudedAreaSolid の
	// 組み合わせで、要素配置とアイテム配置（solid.Position）を合成した結果を返す。
	// 押し出しが無い・解決できないときは false。
	bool resolveElementWorldSolid(const Model& model, const Entity* element, WorldSolid& out);

	// ソリッドのワールド最上端 Z と Z 方向の厚みを返す（Python 版 footing の
	// _z_top_and_thickness 相当）。底面ループと天面ループの Z の最大／最大−最小。
	// 床板は「最下端 Z = top − thickness」を床下端（絶対 Z）として使う。
	void zTopAndThickness(const WorldSolid& solid, double& outTop, double& outThickness);

	// 屋根面（屋根版＝IfcSlab "屋根版" の勾配した平面）。Python 版 rafter._roof_plane の
	// 戻り値 (verts, normal) に対応する。
	//   vertices … ワールド座標の平面外形頂点列（末尾に始点を重複させない）。Z は要素配置
	//              基準＝**ストーリ相対**（階高は要素側で Elevation を足して絶対値にする。
	//              parse/IfcGeometry の resolveObjectPlacement が親を合成しないため）。
	//   normal   … 面の単位法線。**必ず上向き**（z 成分 ≥ 0）に揃える（平面式・勾配方向は
	//              符号反転に対して不変だが、上向きに固定して勾配計算の分母 nz を正にする）。
	struct RoofPlane
	{
		std::vector<Vec3> vertices;
		Vec3 normal;
	};

	// 屋根面の法線から導かれる勾配の座標系。垂木（parse/Rafter）と野地板（parse/Roof）は
	// どちらも「勾配方向へ流す／軒方向へ掃引する／面上の点の天端 Z を引く」を行うため、
	// その計算をここに一本化する（両者に逐語的な複製があり、片方だけ直すとズレていた）。
	//
	//   down   … 勾配方向（最急降下＝法線の水平成分の向き）。+down へ進むと天端 Z が下がる
	//            ＝軒側へ向かう。単位ベクトル。
	//   along  … 軒・棟に平行な方向（勾配方向に直交）。垂木の掃引方向。単位ベクトル。
	//   rise   … 法線の水平成分の大きさ dh（勾配の rise）
	//   run    … 法線の鉛直成分 nz（勾配の run。slope = rise/run = tanθ）
	// 平面上の点の天端 Z は zAt() が返す（法線の符号反転に対して不変。RoofPlane の
	// normal は上向きに揃えてあるので run は正）。
	struct RoofSlope
	{
		Vec2 down;
		Vec2 along;
		double rise = 0.0;
		double run = 0.0;

		Vec3 origin; // 平面式の基準点（RoofPlane の頂点 0。Z はストーリ相対）

		// 平面上の点 (x, y) の天端 Z。elevationOffset にストーリ高さ（Elevation）を渡すと
		// 絶対 Z になる（RoofPlane の Z はストーリ相対のため。IfcGeometry.h の RoofPlane 参照）。
		double zAt(double x, double y, double elevationOffset = 0.0) const;

		// 平面外形の XY 射影（RoofPlane::vertices の Z を落としたもの）。
		// 掃引・クリップ・軒軸の算出はいずれもこの平面図形の上で行う。
		static std::vector<Vec2> plan(const RoofPlane& plane);

		// 点列を方向 dir へ射影した値の [最小, 最大] を返す（掃引方向・勾配方向の広がり）。
		static void projectionRange(const std::vector<Vec2>& points, const Vec2& dir,
									double& outMin, double& outMax);
	};

	// 屋根面の退化（ほぼ水平／鉛直）を判定する法線成分の許容（Python 版 _FLAT_TOL）。
	// 垂木（parse/Rafter）と野地板（parse/Roof）は同じ屋根面を共有するので、**同じ面を
	// 一方だけが退化と見なすことが無いよう**閾値はここに 1 つだけ置く。
	inline constexpr double kRoofFlatTol = 1e-6;

	// 屋根面から勾配の座標系を作る。ほぼ水平な面（法線の水平成分が flatTol 以下＝勾配方向が
	// 定まらない）と、鉛直な面（法線の鉛直成分が flatTol 以下＝平面式が 0 除算になる）は
	// false（out は変更しない）。頂点が空の面も false。
	bool roofSlope(const RoofPlane& plane, RoofSlope& out, double flatTol = kRoofFlatTol);

	// 屋根版（IfcSlab）から屋根面を取り出す（Python 版 rafter._roof_plane 相当）。屋根版は
	// 勾配した平面外形を鉛直に押し出したソリッド（押し出し＝屋根の厚み）なので、
	// resolveElementWorldSolid の配置基底＋プロファイル頂点（base()）がそのまま平面外形に、
	// 配置の局所 Z 軸（zAxis）が面法線になる。ソリッドを解決できない・頂点が 3 点未満
	// （面にならない）ときは false（out は変更しない）。**垂木（parse/Rafter）と野地板
	// （parse/Roof）が同じ面を共有する**ため、M7 の登り梁スナップも本関数を使う。
	bool roofPlane(const Model& model, const Entity* element, RoofPlane& out);

	// ソリッドの平面外形（XY 頂点列）を返す（Python 版 footing._footprint 相当）。
	//   * 鉛直押し出し（床版・底盤）: プロファイルがそのまま平面外形（底面ループの XY）。
	//   * 水平押し出し（立上り・地中梁）: プロファイルは鉛直面内にあるため、断面の水平
	//     方向の幅（プロファイル第 1 座標 u の範囲）を押し出し方向へ掃引した矩形を返す。
	// 鉛直とみなす押し出し Z 成分の閾値は Python 版 _VERTICAL_EXTRUDE_TOL（0.9）と同値。
	std::vector<Vec2> footprint(const WorldSolid& solid);
} // namespace HomeskzIfcImport::parse
