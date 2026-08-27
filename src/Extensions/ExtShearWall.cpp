//
//	Extensions/ExtShearWall.cpp
//
//	耐力壁 PIO の実装（意図は ExtShearWall.h 参照）。
//
//	リセット（Recalculate）のたびに、
//	  1. 自分の両端（線分 PIO の 2 点＝柱芯）をローカル座標へ落とし、
//	  2. パラメータの対象レイヤ（";" 区切り）から**両端の柱**を探して内側面を求め、
//	  3. その内側面と下端・上端で決まる「軸組内法」へ、伏図の記号（2D）と軸組図の面（3D）を
//	     描く
//	という流れで作図する。**内法は毎回実物の柱から導く**ので、柱を動かしても（リセットが
//	走れば）耐力壁が追随して伸縮する。柱が見つからなければ控えの内法（ClearSpan）を両端の
//	中央へ置く。
//
//	使用する SDK API は ci-debug の sdk-grep で実在を確認したもの:
//	  gSDK->GetNamedLayer / FirstMemberObj / NextObject / GetObjectBounds / CreateLine /
//	  CreateOval / AddObjectToContainer、VWParametricObj（GetLinearObjectPos /
//	  GetObjectToWorldTransform / パラメータの読み）、VWPolygon2DObj、VWPolygon3D ＋
//	  VWPolygon3DObj。自分自身のハンドルは基底 VWParametric_EventSink の protected メンバ
//	  fhObject から取る（柱記号 PIO と同じ。ExtColumnMark.cpp 冒頭）。
//
//	【座標系】PIO のジオメトリは**PIO 自身のローカル座標**で持たれる。線分 PIO なので
//	ローカル X が壁の向き（始点→終点）、ローカル Y が壁面の法線（**+Y が表**）、
//	ローカル Z が高さになる。柱はワールド座標で見つかるので、描く前に必ず
//	InversePointTransform でローカルへ落とす（落とさないと PIO を動かした量だけ絵がずれ、
//	リセットしても同じ相対位置に描き直すので直らない。ExtColumnMark.cpp で実証済み）。
//
//	【実際の見え方はローカルで確認する】記号の大きさ・ハッチングの向き・断面ビューポートで
//	3D の面がどう出るかは CI では検証できない（CLAUDE.md「テスト方針」）。純計算に落とせる
//	部分——筋かいの帯を内法で切る形——は core::shearWallBracePolygon に置いて無 SDK で
//	テストしてある。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtShearWall.h"
#include "draw/DrawUtil.h"

#include "core/Document.h"
#include "core/Geometry.h"

#include "VWFC/Math/VWPolygon3D.h"
#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"
#include "VWFC/VWObjects/VWPolygon3DObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace HomeskzIfcImport
{
	namespace
	{
		// 柱を「この耐力壁の端の柱」とみなす許容（mm）。壁の軸から法線方向にこれ以上
		// 離れた柱は別の通りのものなので採らない。半柱幅（〜75）＋モデリング誤差を見込む。
		constexpr double kColumnOffAxisTol = 300.0;

		// 端の柱とみなす軸方向の許容（mm）。柱芯は耐力壁の端点そのものなので本来 0 だが、
		// 手で動かした耐力壁でも拾えるだけの余裕を取る。
		constexpr double kColumnAlongTol = 300.0;

		// 伏図記号が内法に対して大きくなりすぎない上限（内法の何分の 1 まで許すか）。
		constexpr double kMarkSpanFraction = 3.0;

		// PIO の定義。**関数ローカル static** で持つ理由は ExtMenu の menuDef と同じ
		// （SDK の非ローカル static を名前空間スコープの初期化子から参照しない）。
		// **線分 PIO**（両端の 2 点で置く）。
		//
		// 【移動・回転でリセットする】絵は対象レイヤの柱の**ワールド位置**から導くので、
		// PIO 自体を動かしたら描き直さないと柱と食い違ったまま残る（柱記号 PIO と同じ理由）。
		const SParametricDef& parametricDef()
		{
			static const SParametricDef def = {/*LocalizedName*/ {PLUGIN_VWR_ID, "shearWallName"},
											   /*SubType*/ kParametricSubType_Linear,
											   /*ResetOnMove*/ true,
											   /*ResetOnRotate*/ true,
											   /*WallInsertOnEdge*/ false,
											   /*WallInsertNoBreak*/ false,
											   /*WallInsertHalfBreak*/ false,
											   /*WallInsertHideCaps*/ false};
			return def;
		}

		// パラメータ。種別・掛け方・面は文字列、寸法は**長さフィールド**（kFieldCoordDisp）
		// にして OIP で単位付きに見えるようにする。既定値は「何も分からない耐力壁」＝
		// 描かない状態（内法 0・下端＝上端）になるので、パラメータ未設定の PIO が
		// でたらめな絵を描くことはない。
		const SParametricParamDef* paramDefs()
		{
			static const SParametricParamDef defs[] = {
				{kParamShearTargetLayers,
				 {PLUGIN_VWR_ID, "shearWallTargetLayers"},
				 "",
				 "",
				 kFieldText,
				 0},
				{kParamShearKind,
				 {PLUGIN_VWR_ID, "shearWallKind"},
				 kShearKindBrace,
				 kShearKindBrace,
				 kFieldText,
				 0},
				{kParamShearBraceStyle,
				 {PLUGIN_VWR_ID, "shearWallBraceStyle"},
				 kShearBraceSingle,
				 kShearBraceSingle,
				 kFieldText,
				 0},
				{kParamShearBraceRise,
				 {PLUGIN_VWR_ID, "shearWallBraceRise"},
				 kShearRiseEnd,
				 kShearRiseEnd,
				 kFieldText,
				 0},
				{kParamShearPanelSide,
				 {PLUGIN_VWR_ID, "shearWallPanelSide"},
				 kShearSideFront,
				 kShearSideFront,
				 kFieldText,
				 0},
				{kParamShearWidth, {PLUGIN_VWR_ID, "shearWallWidth"}, "0", "0", kFieldCoordDisp, 0},
				{kParamShearThickness,
				 {PLUGIN_VWR_ID, "shearWallThickness"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamShearPanelOffset,
				 {PLUGIN_VWR_ID, "shearWallPanelOffset"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamShearClearSpan,
				 {PLUGIN_VWR_ID, "shearWallClearSpan"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamShearBottom,
				 {PLUGIN_VWR_ID, "shearWallBottom"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamShearTop, {PLUGIN_VWR_ID, "shearWallTop"}, "0", "0", kFieldCoordDisp, 0},
				{kParamShearMarkSize,
				 {PLUGIN_VWR_ID, "shearWallMarkSize"},
				 "300",
				 "300",
				 kFieldCoordDisp,
				 0},
				{"", {}, "", "", kFieldText, 0}}; // 終端
			return defs;
		}

		// PIO の実数パラメータを読む（読めなければ fallback）。**長さフィールドでも
		// 文字列で保持されていることがある**ので、実数で 0 が返ったら文字列でも試す
		// （書き手側の SetParamRealChecked が実数／文字列のどちらでも入れるのと対。
		// draw/DrawUtil.h）。
		double ParamReal(const VWParametricObj& pio, const char* name, double fallback = 0.0)
		{
			try
			{
				const double value = pio.GetParamReal(TXString(name));
				if (value != 0.0)
					return value;
			}
			catch (...)
			{
				// 実数として読めないパラメータ。下の文字列読みへ回す。
			}
			const std::string text = draw::PioParamString(pio, name);
			if (text.empty())
				return fallback;
			try
			{
				return std::stod(text);
			}
			catch (...)
			{
				return fallback;
			}
		}

		// ";" 区切りのレイヤ名を分解する（空の要素は落とす）。
		std::vector<std::string> SplitLayers(const std::string& joined)
		{
			std::vector<std::string> names;
			std::size_t begin = 0;
			while (begin <= joined.size())
			{
				const std::size_t end = joined.find(';', begin);
				const std::string name = joined.substr(
					begin, end == std::string::npos ? std::string::npos : end - begin);
				if (!name.empty())
					names.push_back(name);
				if (end == std::string::npos)
					break;
				begin = end + 1;
			}
			return names;
		}

		// 見つけた柱 1 本の、PIO ローカル座標での広がり。
		struct ColumnRange
		{
			double loX = 0.0;	  // 軸方向の最小（＝始点側の面）
			double hiX = 0.0;	  // 軸方向の最大（＝終点側の面）
			double offAxis = 0.0; // 軸からの法線方向の離れ（中心）
		};

		// 柱のワールド外接矩形を PIO ローカルへ落として広がりを返す。
		ColumnRange LocalRangeOf(MCObjectHandle column, const VWTransformMatrix& toWorld)
		{
			WorldRect bounds;
			gSDK->GetObjectBounds(column, bounds);

			ColumnRange range;
			range.loX = std::numeric_limits<double>::max();
			range.hiX = std::numeric_limits<double>::lowest();
			double loY = std::numeric_limits<double>::max();
			double hiY = std::numeric_limits<double>::lowest();
			// 外接矩形の 4 隅をローカルへ落とす（壁が斜めでも広がりを取り違えない）。
			const std::array<double, 2> xs = {bounds.left, bounds.right};
			const std::array<double, 2> ys = {bounds.bottom, bounds.top};
			for (const double x : xs)
			{
				for (const double y : ys)
				{
					const VWPoint2D local = toWorld.InversePointTransform(VWPoint2D(x, y));
					range.loX = std::min(range.loX, local.x);
					range.hiX = std::max(range.hiX, local.x);
					loY = std::min(loY, local.y);
					hiY = std::max(hiY, local.y);
				}
			}
			range.offAxis = (loY + hiY) / 2.0;
			return range;
		}

		// 対象レイヤを走査して、軸の両端に最も近い柱の**内側面**を求める。両端とも
		// 見つかれば true（outStart < outEnd）。
		bool ClearSpanFromColumns(const std::vector<std::string>& layers,
								  const VWTransformMatrix& toWorld, double startX, double endX,
								  double& outStart, double& outEnd)
		{
			bool haveStart = false;
			bool haveEnd = false;
			double bestStart = kColumnAlongTol;
			double bestEnd = kColumnAlongTol;

			for (const std::string& name : layers)
			{
				const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
				if (layer == nil)
					continue; // その階の柱レイヤが生成されていない

				for (MCObjectHandle h = gSDK->FirstMemberObj(layer); h != nil;
					 h = gSDK->NextObject(h))
				{
					if (draw::StructuralUseOf(h) != core::kStructuralUseColumn)
						continue; // 柱（構造用途 4）だけを見る（小屋束・梁は取らない）

					const ColumnRange range = LocalRangeOf(h, toWorld);
					if (std::abs(range.offAxis) > kColumnOffAxisTol)
						continue; // 別の通りの柱

					const double centre = (range.loX + range.hiX) / 2.0;
					if (const double toStart = std::abs(centre - startX); toStart < bestStart)
					{
						bestStart = toStart;
						outStart = range.hiX; // 始点側の柱の**終点寄りの面**が内法の始まり
						haveStart = true;
					}
					if (const double toEnd = std::abs(centre - endX); toEnd < bestEnd)
					{
						bestEnd = toEnd;
						outEnd = range.loX; // 終点側の柱の**始点寄りの面**が内法の終わり
						haveEnd = true;
					}
				}
			}
			return haveStart && haveEnd && outEnd > outStart;
		}

		// 2D の閉じたポリゴンを PIO のジオメトリとして置く。
		//
		// **VWFC で作ったオブジェクトはどのコンテナにも入らない**ので、AddObjectToContainer
		// で PIO（host）へ入れ直す。外すと静かに消える（draw/Symbol.cpp の 1 番目の作法）。
		void AddPolygon2D(MCObjectHandle host, const std::vector<core::Vec2>& points,
						  const char* className)
		{
			if (points.size() < 3)
				return;
			VWPolygon2DObj poly;
			for (const core::Vec2& point : points)
				poly.AddVertex(point.x, point.y);
			const MCObjectHandle handle = poly.GetThisObject();
			if (handle == nil)
				return;
			gSDK->AddObjectToContainer(handle, host);
			draw::SetClassByName(handle, className);
			draw::SetAllAttributesByClass(handle);
		}

		// 壁面内の 2D 点列（x＝軸方向・y＝高さ）を、法線方向 offset の鉛直面へ置いた
		// 3D ポリゴンとして PIO のジオメトリに加える。className が空ならクラスを与えない
		// （PIO 本体のクラスがそのまま効く）。
		void AddPolygon3D(MCObjectHandle host, const std::vector<core::Vec2>& points, double offset,
						  const char* className)
		{
			if (points.size() < 3)
				return;
			VWPolygon3D shape;
			for (const core::Vec2& point : points)
				shape.AddVertex(point.x, offset, point.y);
			VWPolygon3DObj poly(shape);
			const MCObjectHandle handle = poly.GetThisObject();
			if (handle == nil)
				return;
			poly.SetClosed(true);
			gSDK->AddObjectToContainer(handle, host);
			if (className != nullptr && className[0] != '\0')
			{
				draw::SetClassByName(handle, className);
				draw::SetAllAttributesByClass(handle);
			}
		}

		// 伏図の三角記号 1 つ。底辺は内法の端（atStart なら始点側）で壁に直交し、頂点が
		// **材が高くなる側**を指す。
		void AddBraceTriangle(MCObjectHandle host, double clearStart, double clearEnd, double size,
							  bool atStart)
		{
			const double base = atStart ? clearStart : clearEnd;
			const double apex = atStart ? clearStart + size : clearEnd - size;
			const double half = size / 2.0;
			AddPolygon2D(host,
						 {core::Vec2{base, half}, core::Vec2{base, -half}, core::Vec2{apex, 0.0}},
						 kShearMarkClass);
		}

		// 伏図の面材記号 1 つ。壁に平行な線と、その中央の丸印。
		void AddPanelMark(MCObjectHandle host, double clearStart, double clearEnd, double offset,
						  double size)
		{
			const MCObjectHandle line =
				gSDK->CreateLine(WorldPt(clearStart, offset), WorldPt(clearEnd, offset));
			if (line != nil)
			{
				draw::SetClassByName(line, kShearMarkClass);
				draw::SetAllAttributesByClass(line);
			}

			const double radius = size / 4.0;
			const double centre = (clearStart + clearEnd) / 2.0;
			WorldRect bounds;
			bounds.left = centre - radius;
			bounds.right = centre + radius;
			bounds.bottom = offset - radius;
			bounds.top = offset + radius;
			const MCObjectHandle circle = gSDK->CreateOval(bounds);
			if (circle != nil)
			{
				draw::SetClassByName(circle, kShearMarkClass);
				draw::SetAllAttributesByClass(circle);
			}
		}
	} // namespace

	// --------------------------------------------------------------------------
	// - NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
	// UUID: 8dc146c6-dcac-4c5e-957d-036756b40f88  (dev build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtShearWall,
		/*Event sink*/ CShearWall_EventSink,
		/*Universal name*/ kShearWallUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0x8dc146c6, 0xdcac, 0x4c5e, 0x95, 0x7d, 0x03, 0x67, 0x56, 0xb4, 0x0f, 0x88);
#else
	// UUID: a75b320e-72bb-4428-98b3-92ec09febc8c  (stable build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtShearWall,
		/*Event sink*/ CShearWall_EventSink,
		/*Universal name*/ kShearWallUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0xa75b320e, 0x72bb, 0x4428, 0x98, 0xb3, 0x92, 0xec, 0x09, 0xfe, 0xbc, 0x8c);
#endif
	// NOLINTEND(misc-const-correctness)

	// ---------------------------------------------------------------------------
	CExtShearWall::CExtShearWall(CallBackPtr cbp)
		: VWExtensionParametric(cbp, parametricDef(), paramDefs())
	{
	}

	CExtShearWall::~CExtShearWall() = default;

	// ---------------------------------------------------------------------------
	CShearWall_EventSink::CShearWall_EventSink(IVWUnknown* parent) : VWParametric_EventSink(parent)
	{
	}

	CShearWall_EventSink::~CShearWall_EventSink() = default;

	// ---------------------------------------------------------------------------
	EObjectEvent CShearWall_EventSink::OnInitXProperties(CodeRefID objectID)
	{
		const EObjectEvent result = VWParametric_EventSink::OnInitXProperties(objectID);

		// 生成のたびに「オブジェクトの設定」ダイアログを出さない（柱記号 PIO と同じ理由）。
		// 値は Sint8 なので **SetObjectPropertyChar** を使う（ISDK.h:1545-1546）。
		gSDK->SetObjectPropertyChar(objectID, kObjXPropShowPrefDialogWhen,
									static_cast<unsigned char>(kCustomObjectPrefNever));

		// 印刷・書き出しの直前にリセットする。図面として外へ出る瞬間に必ず実物（柱の位置）と
		// 一致させるための最後の砦（柱記号 PIO と同じ。docs/DEV-NOTES.md M12「追随の契機」）。
		gSDK->SetObjectProperty(objectID, kObjXPropResetBeforeExport, true);
		return result;
	}

	EObjectEvent CShearWall_EventSink::Recalculate()
	{
		// 自分自身のハンドルは基底の protected メンバ（VWFC が Execute で詰める）。
		// リセット以外の経路で空のまま呼ばれても落とさないよう nil を見ておく。
		if (this->fhObject == nil)
			return kObjectEventNoErr;

		try
		{
			VWParametricObj self(this->fhObject);

			const double bottom = ParamReal(self, kParamShearBottom);
			const double top = ParamReal(self, kParamShearTop);
			if (top <= bottom)
				return kObjectEventNoErr; // 内法の高さが無い＝まだ何も設定されていない

			// 両端（柱芯）をローカルへ落とす。線分 PIO のローカル X が壁の向き、
			// +Y が表側になる（ヘッダ「座標系」）。
			VWTransformMatrix toWorld;
			self.GetObjectToWorldTransform(toWorld);
			VWPoint2D worldStart;
			VWPoint2D worldEnd;
			self.GetLinearObjectPos(worldStart, worldEnd);
			const VWPoint2D localStart = toWorld.InversePointTransform(worldStart);
			const VWPoint2D localEnd = toWorld.InversePointTransform(worldEnd);
			const double startX = localStart.x;
			const double endX = localEnd.x;
			if (endX - startX < core::kPointEps)
				return kObjectEventNoErr; // 両端が同じ＝向きが決まらない

			// 軸組内法。**実物の柱から引くのが本筋**で、見つからないときだけ控えを使う。
			double clearStart = 0.0;
			double clearEnd = 0.0;
			if (!ClearSpanFromColumns(
					SplitLayers(draw::PioParamString(self, kParamShearTargetLayers)), toWorld,
					startX, endX, clearStart, clearEnd))
			{
				const double span = ParamReal(self, kParamShearClearSpan);
				if (span <= 0.0)
					return kObjectEventNoErr;
				const double centre = (startX + endX) / 2.0;
				clearStart = centre - (span / 2.0);
				clearEnd = centre + (span / 2.0);
			}

			const double span = clearEnd - clearStart;
			if (span <= 0.0)
				return kObjectEventNoErr;

			// 伏図記号の大きさ。内法に対して大きすぎると図が潰れるので頭打ちにする。
			const double markSize =
				std::min(ParamReal(self, kParamShearMarkSize, kShearMarkSizeDefault),
						 span / kMarkSpanFraction);

			if (draw::PioParamString(self, kParamShearKind) == kShearKindPanel)
			{
				// 面材。表＝+Y・裏＝−Y（ヘッダ「座標系」）。離れが分からなければ記号の
				// 大きさで代用する（伏図で線が壁芯に重なって読めなくなるのを避ける）。
				const std::string side = draw::PioParamString(self, kParamShearPanelSide);
				const bool front = side != kShearSideBack;
				const bool back = side == kShearSideBack || side == kShearSideBoth;
				double offset = ParamReal(self, kParamShearPanelOffset);
				if (offset <= 0.0)
					offset = markSize / 2.0;

				if (front)
				{
					AddPanelMark(this->fhObject, clearStart, clearEnd, offset, markSize);
					AddPolygon3D(this->fhObject,
								 {core::Vec2{clearStart, bottom}, core::Vec2{clearEnd, bottom},
								  core::Vec2{clearEnd, top}, core::Vec2{clearStart, top}},
								 offset, kShearPanelFrontClass);
				}
				if (back)
				{
					AddPanelMark(this->fhObject, clearStart, clearEnd, -offset, markSize);
					AddPolygon3D(this->fhObject,
								 {core::Vec2{clearStart, bottom}, core::Vec2{clearEnd, bottom},
								  core::Vec2{clearEnd, top}, core::Vec2{clearStart, top}},
								 -offset, kShearPanelBackClass);
				}
				return kObjectEventNoErr;
			}

			// 筋かい。帯は内法の対角線に沿い、はみ出した角は内法で切る
			// （形は core::shearWallBracePolygon が決める＝無 SDK でテスト済み）。
			const bool risesToEnd =
				draw::PioParamString(self, kParamShearBraceRise) != kShearRiseStart;
			const bool doubleBrace =
				draw::PioParamString(self, kParamShearBraceStyle) == kShearBraceDouble;
			const double width = ParamReal(self, kParamShearWidth);

			// 伏図: 材の**足元**へ三角記号を置く。たすき掛けは両端に 1 つずつ。
			AddBraceTriangle(this->fhObject, clearStart, clearEnd, markSize, risesToEnd);
			if (doubleBrace)
				AddBraceTriangle(this->fhObject, clearStart, clearEnd, markSize, !risesToEnd);

			// 軸組図: 形状どおりの帯。
			AddPolygon3D(
				this->fhObject,
				core::shearWallBracePolygon(clearStart, clearEnd, bottom, top, width, risesToEnd),
				0.0, "");
			if (doubleBrace)
				AddPolygon3D(this->fhObject,
							 core::shearWallBracePolygon(clearStart, clearEnd, bottom, top, width,
														 !risesToEnd),
							 0.0, "");
		}
		catch (...)
		{
			// 1 枚の異常で耐力壁全体を落とさない（CLAUDE.md「エラーハンドリング」）。
			// kObjectEventHadError を返すと VW がオブジェクトをエラー表示にするので、
			// ここまでに描けたものを残したまま正常終了として抜ける（柱記号 PIO と同じ）。
			return kObjectEventNoErr;
		}
		return kObjectEventNoErr;
	}
} // namespace HomeskzIfcImport
