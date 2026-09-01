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
//	  * 伏図（2D）… 筋かいは**直角三角形**（壁と平行な脚を底に、斜辺が足元→頂部の傾きを
//	    表す＝図から向きが読める。たすき掛けは同じ場所へ**反転して重ねる**）、面材は
//	    **壁に平行な線と丸印**（表・裏それぞれの側に 1 本ずつ）。
//	    **面や筋かいのポリゴンは描かない**——軸組材と重なって図が読めなくなる。
//	    記号は壁芯ではなく**壁芯から MarkOffset だけ離した線の上**に置く（面材なら
//	    その面材のある側）。壁芯には横架材（土台・胴差）が同じ太さで載っていて、記号を
//	    芯へ置くと必ず重なるため。
//	    大きさは**図面 mm**（1/50 の伏図で三角 6×3mm・丸 直径 3mm）。記号は実物の寸法を
//	    表さない表記なので、内法でも縮尺でも割らない。
//	    ※ シンボルで持たせる案は M19 で試して**断念した**——SDK からシンボル定義へ図形を
//	    入れる手段が見つからず、空のシンボルしか作れなかった（docs/DEV-NOTES.md）。
//	  * 軸組図（3D）… 筋かいは**形状どおりの帯**（実幅の帯を内法の矩形で切ったもの）、
//	    面材は**内法を埋める矩形**。どちらも**壁芯の鉛直面に置く**——軸組図は通り芯
//	    （＝壁芯）で切った断面で、切断面より奥は表示しないので、実物どおり板の位置へ
//	    外すと表も裏も図から消える（実機で確認。M19）。表と裏はクラスを分けてあり、
//	    **ハッチングの向き**で見分ける（ハッチングそのものはクラス属性なので、テンプレート側が持つ。
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
	constexpr const char* kParamShearClearSpan = "ClearSpan"; // 控えの内法（mm）
	constexpr const char* kParamShearBottom = "BottomHeight"; // 内法の下端（mm・レイヤ基準）
	constexpr const char* kParamShearTop = "TopHeight";		  // 内法の上端（mm・同上）
	constexpr const char* kParamShearMarkOffset = "MarkOffset"; // 伏図記号の壁芯からの離れ（mm）

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

	// 伏図記号を壁芯からどれだけ離すか（**図面 mm**）。壁芯には
	// 横架材（土台・胴差）が載っているので、その下へ潜らない位置へ寄せるための値。
	// **面材ならその面材のある側**（表＝+Y・裏＝−Y）へ、筋かいは表側へ寄せる（筋かいは
	// 壁の中心にあり寄せる根拠が無いので、決定性のために片側へ固定する）。
	constexpr double kShearMarkOffsetDefault = 200.0;

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

	// ------------------------------------------------------------------
	// 伏図記号のシンボル定義名と寸法。
	//
	// **このプラグインが図面へ登録する唯一の名前付きリソース**（CLAUDE.md「既存の図面
	// リソースを作らない」の例外。記号をシンボルにしたいというご要望による）。定義は
	// draw/ShearWall の EnsureMarkSymbols が**耐力壁を 1 枚でも描くときにだけ**作り、
	// PIO はそれを名前で置く。図面に同じ名前の定義が既にあれば**触らない**ので、
	// 記号の絵を差し替えたい人はシンボルを編集すればよい。
	//
	// ★**シンボル定義は中身を入れた後に ResetObject を呼ばないと外接が付かず、
	//   「中身はあるのに空に見える」定義になる**（M19。docs/DEV-NOTES.md）。
	//
	// 筋かいの三角は向きが 2 通り（斜辺が終端側へ上がるか始端側へ上がるか）あり、記号を
	// 壁芯の表側／裏側どちらへ寄せるかで上下も入れ替わるので組み合わせは 4 通りある。
	// **どれも 1 つの定義を軸ごとに反転すれば作れる**ので、定義は 1 つでよい。反転は
	// シンボルインスタンスの**負の倍率**で与える（VW が反転したシンボルを表す唯一の形。
	// `SetScaleType(kScaleTypeAsymmetric)` ＋ `SetScaleFactorX/Y`）。定義は
	// 「終端側へ上がる・表へ寄せる」姿——壁と平行な脚の中央が原点、直角が +X 側、
	// 頂点が +Y 側——で作り、置くときの倍率はこうなる:
	//
	//   | 斜辺 | 寄せる側 | 倍率 X | 倍率 Y |
	//   | --- | --- | ---: | ---: |
	//   | 終端側へ上がる | 表（+） |  1 |  1 |
	//   | 始端側へ上がる | 表（+） | −1 |  1 |
	//   | 終端側へ上がる | 裏（−） |  1 | −1 |
	//   | 始端側へ上がる | 裏（−） | −1 | −1 |
	constexpr const char* kShearMarkBraceSymbol = "耐力壁記号_筋かい";
	constexpr const char* kShearMarkPanelSymbol = "耐力壁記号_面材";

	// 記号の寸法（**用紙 mm**。どの縮尺の伏図でも紙の上でこの大きさに出る）。内法では
	// 割らない——455mm 幅の壁だけ記号が縮んで図が不揃いに見えた（M19）。シンボル定義の
	// 中身を作る draw/ShearWall だけが使う（置き場所を決める PIO 本体は使わない）。
	//
	// ★**用紙 mm で持てるのはシンボルが用紙基準（縮尺無視）だから。** 用紙基準の
	// 大きさは「この図形 × そのレイヤの縮尺」で決まるので、耐力壁レイヤの縮尺を伏図の
	// 縮尺へ揃える必要がある（draw/ShearWall の applyShearWallLayerScale）。
	constexpr double kShearMarkTriangleLength = 6.0; // 壁と平行な脚（＝斜辺の水平投影）
	constexpr double kShearMarkTriangleHeight = 3.0; // 壁に直交する脚（＝直角を立てる側）
	constexpr double kShearMarkCircleDiameter = 3.0;

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
