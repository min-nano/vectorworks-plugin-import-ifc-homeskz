//
//	Extensions/ExtShearWall.h
//
//	耐力壁（筋かい・面材）を描く PIO（パラメトリックオブジェクト）。柱記号（ExtColumnMark）と
//	同じく、耐力壁は素のジオメトリではなく PIO で描き、その PIO をこのプラグインへ同梱する
//	（理由は docs/DEV-NOTES.md M19）。
//
//	【何をするか】**線分 PIO**（両端の 2 点で置く）で、リセットのたびに
//	  1. パラメータの**対象レイヤ**（";" 区切りの span 柱レイヤ）から柱（構造用途 4）を探し、
//	  2. 両端の柱の**内側面**と、パラメータの下端・上端から「軸組内法」の矩形を決め、
//	  3. その矩形へ絵を描き直す
//	という流れで作図する。柱を動かせば（リセットが走れば）内法が引き直され、耐力壁が
//	**追随して伸縮する**。柱が見つからない図面では、解析時に IFC から測った控えの内法
//	（ClearSpan）を両端の中央へ置いて描く——柱が無いだけで耐力壁が消えるのは、図面として
//	黙って欠ける最悪の形なので避ける。
//
//	【伏図と軸組図で描き分ける】PIO は 2D と 3D の両方を描き、VW が図に応じて使い分ける
//	（伏図＝2D、軸組図＝3D）:
//	  * 伏図（2D）… 筋かいは**三角記号**（材の足元に置き、頂点が上がる側を指す。たすき掛けは
//	    両端に 1 つずつ）、面材は**壁に平行な線と丸印**（表・裏それぞれの側に 1 本ずつ）。
//	    **面や筋かいのポリゴンは描かない**——軸組材と重なって図が読めなくなる。
//	  * 軸組図（3D）… 筋かいは**形状どおりの帯**（実幅の帯を内法の矩形で切ったもの）、
//	    面材は**内法を埋める矩形**。表と裏はクラスを分けてあり、**ハッチングの向き**で
//	    見分ける（ハッチングそのものはクラス属性なので、テンプレート側が持つ。
//	    プラグインは図面リソースを作らない。CLAUDE.md「既存の図面リソースを作らない」）。
//
//	【絵を全部止めない】パラメータが 1 つ読めなくても、描けるところまでは描く。
//	伏図の記号は平面（両端の柱と内法）だけで決まるので、**高さが取れなくても描く**——
//	高さの取りこぼしで図面から耐力壁が丸ごと消えるのは、記号だけでも出ているより悪い。
//	3D（帯・面）は高さが要るのでそこだけ諦める。書き手側（draw/ShearWall）も同じ考え方で、
//	1 つ書けなかったパラメータのために残り全部とリセットを飛ばさない。
//
//	【診断】dev ビルド（と HOMESKZ_IFC_TRACE 指定時）は、PIO が実際に持っている
//	パラメータの一覧と、読めた内法・高さを診断ログへ書く（core/Trace）。パラメータが
//	登録されていなければ setter も getter も黙って通らないので、症状からは
//	「解析が値を出していない」のか「PIO に届いていない」のか区別できない——一覧を
//	1 行残しておけば、そのどちらかがすぐ分かる。
//
//	【登録名はこのプラグイン固有にする】同種の PIO を提供する別のプラグインと同じ名前で
//	登録すると、両方を入れた環境で衝突する。ユニバーサル名は "HomeskzShearWall"。
//

#pragma once

#include "PluginPrefix.h"

#include "VWFC/PluginSupport/VWExtensionParametric.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// PIO のユニバーサル名。**解析側が命令に載せる名前ではなく、描画側が
	// CreateCustomObject へ渡す名前**なので、draw/ShearWall と共有する。
	constexpr const char* kShearWallUniversalName = "HomeskzShearWall";

	// パラメータのユニバーサル名。**draw/ShearWall が書く名前とここが食い違うと setter は
	// 黙って無視される**ので、定義はここ 1 か所（柱記号 PIO と同じ作法）。
	constexpr const char* kParamShearTargetLayers = "TargetLayers"; // 柱を探すレイヤ（";" 区切り）
	constexpr const char* kParamShearKind = "WallKind";				// 種別（下記）
	constexpr const char* kParamShearBraceStyle = "BraceStyle"; // 筋かいの掛け方（下記）
	constexpr const char* kParamShearBraceRise = "BraceRise"; // 筋かいが高くなる側（下記）
	constexpr const char* kParamShearPanelSide = "PanelSide"; // 面材を設ける面（下記）
	constexpr const char* kParamShearWidth = "BraceWidth";	  // 筋かいの見付け幅（mm）
	constexpr const char* kParamShearPanelOffset = "PanelOffset"; // 面材の中心面の離れ（mm）
	constexpr const char* kParamShearClearSpan = "ClearSpan";	  // 控えの内法（mm）
	constexpr const char* kParamShearBottom = "BottomHeight"; // 内法の下端（mm・レイヤ基準）
	constexpr const char* kParamShearTop = "TopHeight";		  // 内法の上端（mm・同上）
	constexpr const char* kParamShearMarkSize = "MarkSize"; // 伏図記号の大きさ（mm）

	// 値。ユニバーサル名なので言語に依存しない綴りにする。
	constexpr const char* kShearKindBrace = "Brace";	// 筋かい
	constexpr const char* kShearKindPanel = "Panel";	// 面材
	constexpr const char* kShearBraceSingle = "Single"; // 片掛け
	constexpr const char* kShearBraceDouble = "Double"; // たすき掛け
	constexpr const char* kShearRiseStart = "Start";	// 始点側が高い
	constexpr const char* kShearRiseEnd = "End";		// 終点側が高い
	constexpr const char* kShearSideFront = "Front";	// 表（軸の左手側）
	constexpr const char* kShearSideBack = "Back";		// 裏
	constexpr const char* kShearSideBoth = "Both";		// 両面

	// 伏図記号の既定の大きさ（mm。三角記号の長さ・丸印の直径の基）。
	//
	// **柱記号（ExtColumnMark）と違って寸法をパラメータで持つ。** あちらは記号を柱の
	// **実断面**から描くので外から与える経路そのものが無かったが、耐力壁の伏図記号は
	// 実物の寸法とは無関係な**表記**（三角・丸）で、実物から導ける大きさが無い。既定値は
	// 1/50 の伏図で 6mm 前後になるように選んである（用紙の上で読める最小限）。
	constexpr double kShearMarkSizeDefault = 300.0;

	// PIO が**自分で描いたジオメトリ**へ与えるクラス。PIO 本体のクラス（命令の drawClass。
	// parse/StructuralClass の CLASS_BRACE / CLASS_SHEAR_PANEL）とは役割が違い、こちらは
	// 「その絵をどう見せるか」を決める:
	//   * 伏図の記号（三角・線・丸）… 記号の作図クラス。
	//   * 面材の表／裏 … **ハッチングの向きだけが違う 2 クラス**。向きの定義はクラス属性
	//     （テンプレート側）が持つので、プラグインはクラス名を与えるだけでよい。
	// 筋かいの帯にはクラスを与えない——PIO 本体のクラス（筋かい）がそのまま効く。
	constexpr const char* kShearMarkClass = "01作図-04記号-04構造-一般";
	constexpr const char* kShearPanelFrontClass = "04構造-02木造-06耐力面材-01壁-01表";
	constexpr const char* kShearPanelBackClass = "04構造-02木造-06耐力面材-01壁-02裏";

	// ------------------------------------------------------------------------
	// リセット時に耐力壁を描く本体。
	class CShearWall_EventSink : public VWParametric_EventSink
	{
	public:
		CShearWall_EventSink(IVWUnknown* parent);
		~CShearWall_EventSink() override;

		// 両端の柱を探して内法を求め、伏図の記号と軸組図の面を描き直す（PIO のリセット）。
		EObjectEvent Recalculate() override;

		// オブジェクトプロパティの初期化。**「生成時に設定ダイアログを出さない」を
		// ここで宣言する**（柱記号 PIO と同じ理由——インポートが自動生成するので、
		// 出るとその数だけインポートが止まる）。
		EObjectEvent OnInitXProperties(CodeRefID objectID) override;
	};

	// ------------------------------------------------------------------------
	// 拡張そのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtShearWall : public VWExtensionParametric
	{
		DEFINE_VWParametricExtension;

	public:
		CExtShearWall(CallBackPtr cbp);
		~CExtShearWall() override;
	};
} // namespace HomeskzIfcImport
