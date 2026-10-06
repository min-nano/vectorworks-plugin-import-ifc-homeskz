//
//	Extensions/ExtShearWall.cpp
//
//	耐力壁 PIO の実装（意図は ExtShearWall.h 参照）。
//
//	リセット（Recalculate）のたびに、
//	  1. 自分の両端（線分 PIO の 2 点＝柱芯）をローカル座標へ変換し、
//	  2. パラメータの対象レイヤ（";" 区切り）から**両端の柱**を探して内側面を求め、
//	  3. その内側面と下端・上端で決まる「軸組内法」へ、伏図の記号（2D）と軸組図の面（3D）を
//	     描画する
//	という流れで作図する。**内法は毎回実物の柱から求める**ので、柱を動かしても（リセットが
//	実行されれば）耐力壁が追随して伸縮する。柱が見つからなければ控えの内法（ClearSpan）を両端の
//	中央へ置く。
//
//	使用する SDK API は ci-debug の sdk-grep で実在を確認したもの:
//	  gSDK->GetNamedLayer / FirstMemberObj / NextObject / GetObjectBounds / CreateLine /
//	  CreateOval / AddObjectToContainer、VWParametricObj（GetLinearObjectPos /
//	  GetObjectToWorldTransform / パラメータの読み出し）、VWPolygon2DObj、VWPolygon3D ＋
//	  VWPolygon3DObj。自分自身のハンドルは基底 VWParametric_EventSink の protected メンバ
//	  fhObject から取る（柱記号 PIO と同じ。ExtColumnMark.cpp 冒頭）。
//
//	【座標系】PIO のジオメトリは**PIO 自身のローカル座標**で持たれる。線分 PIO なので
//	ローカル X が壁の向き（始点→終点）、ローカル Y が壁面の法線（**+Y が表**）、
//	ローカル Z が高さになる。柱はワールド座標で見つかるので、描画する前に必ず
//	InversePointTransform でローカルへ変換する（変換しないと PIO を動かした量だけ図形が
//	ずれ、リセットしても同じ相対位置に再描画するので直らない。ExtColumnMark.cpp で実証済み）。
//
//	【実際の表示はローカルで確認する】記号の大きさ・ハッチングの向き・断面ビューポートで
//	3D の面がどう表示されるかは CI では検証できない（CLAUDE.md「テスト方針」）。純計算に分離できる
//	部分——筋かいの帯を内法で切る形——は core::shearWallBracePolygon に置いて無 SDK で
//	テストしてある。
//
//	【ここに残るのは登録と取り次ぎだけ】描画の処理は本体（ペイロード）側の
//	draw::recalculateShearWall にある。PIO の登録は Vectorworks に番地を保持されるので殻に
//	残すほかないが、描画の処理は本体へ移せる——そうしておくと**耐力壁の修正が Vectorworks の
//	再起動なしに反映される**（src/PayloadAbi.h / src/PayloadSession.h）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtShearWall.h"
#include "PayloadAbi.h"
#include "PayloadSession.h"

#include <array>
#include <string>

namespace HomeskzIfcImport
{
	namespace
	{
		// PIO の定義。**関数ローカル static** で持つ理由は ExtMenu の menuDef と同じ
		// （SDK の非ローカル static を名前空間スコープの初期化子から参照しない）。
		// **線分 PIO**（両端の 2 点で配置する）。
		//
		// 【移動・回転でリセットする】図形は対象レイヤの柱の**ワールド位置**から求めるので、
		// PIO 自体を動かしたら再描画しないと柱と食い違ったまま残る（柱記号 PIO と同じ理由）。
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
		// 描画しない状態（内法 0・下端＝上端）になるので、パラメータ未設定の PIO が
		// 誤った図形を描画することはない。
		const SParametricParamDef* paramDefs()
		{
			// SDK は「番兵で終わる配列の先頭ポインタ」を受け取る（SParametricParamDef*）。
			// 入れ物を std::array にしても .data() で同じポインタを渡せるので、C 配列にする
			// 理由は無い（番兵は最後の要素としてそのまま残す）。
			static const std::array<SParametricParamDef, 12> defs = {
				{// **並びは「描画にとってどれだけ要るか」の順**。OIP の上から重要な順に読める
				 // うえに、万一 VW 側が一覧を途中までしか登録しなくても、欠けるのは
				 // 既定値で代用が利くもの（記号の大きさ・面材の離れ・見付け幅）から順になる。
				 {kParamShearBottom,
				  {PLUGIN_VWR_ID, "shearWallBottom"},
				  "0",
				  "0",
				  kFieldCoordDisp,
				  0},
				 {kParamShearTop, {PLUGIN_VWR_ID, "shearWallTop"}, "0", "0", kFieldCoordDisp, 0},
				 {kParamShearTopEnd,
				  {PLUGIN_VWR_ID, "shearWallTopEnd"},
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
				 {kParamShearWidth,
				  {PLUGIN_VWR_ID, "shearWallWidth"},
				  "0",
				  "0",
				  kFieldCoordDisp,
				  0},
				 {kParamShearMarkOffset,
				  {PLUGIN_VWR_ID, "shearWallMarkOffset"},
				  "4",
				  "4",
				  kFieldCoordDisp,
				  0},
				 {"", {}, "", "", kFieldText, 0}}}; // 終端
			return defs.data();
		}
	} // namespace

	// --------------------------------------------------------------------------
	// NOLINTBEGIN(misc-const-correctness)
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
		// 一致させるための最後の手段（柱記号 PIO と同じ。docs/DEV-NOTES.md M12「追随の契機」）。
		gSDK->SetObjectProperty(objectID, kObjXPropResetBeforeExport, true);

		// **レイヤの縮尺が変わったら再描画する。** いまの記号は図面 mm（縮尺に依らない）
		// なので必須ではないが、記号のシンボルを用紙基準に戻すなら要る印として残す。
		gSDK->SetObjectProperty(objectID, kObjXPropHasLayerScaleDeps, true);
		return result;
	}
	// ---------------------------------------------------------------------------
	// **描画するのは本体（ペイロード）。** ここでするのは「本体を（必要なら再読み込みして）
	// 確保し、自分のハンドルを渡す」だけ（PayloadSession.h）。読み込めなかったときは
	// **何も表示せずに何もしない**——柱記号 PIO と同じ理由（リセットは図面の耐力壁の数だけ
	// 実行される）。
	EObjectEvent CShearWall_EventSink::Recalculate()
	{
		// 自分自身のハンドルは基底の protected メンバ（VWFC が Execute で設定する）。
		if (this->fhObject == nil)
			return kObjectEventNoErr;

		const PayloadUse use;
		if (!use.ok())
			return kObjectEventNoErr;

		int event = kObjectEventNoErr;
		std::string error;
		// 境界はオブジェクトのハンドルを void* で受け渡す（src/PayloadAbi.h「SDK を include
		// しない」）。MCObjectHandle は char** なので、この変換は
		// bugprone-multi-level-implicit-pointer-conversion の警告対象になる——**意図した変換**
		// （受け取った本体が同じ型へ戻す）であり、明示キャストにしても警告は消えないので、
		// この 1 行だけ規則を無効にする。
		// NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
		void* const handle = this->fhObject;
		if (!use->recalculate(kVwPayloadPioShearWall, handle, event, error))
			return kObjectEventNoErr;
		return static_cast<EObjectEvent>(event);
	}
} // namespace HomeskzIfcImport
