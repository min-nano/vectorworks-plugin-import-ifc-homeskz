//
//	Extensions/ExtColumnMark.cpp
//
//	柱・小屋束の記号 PIO の実装（意図は ExtColumnMark.h 参照）。
//
//	リセット（Recalculate）のたびに、
//	  1. パラメータの対象レイヤを名前で引き、
//	  2. そのレイヤの構造材（StructuralMember）を走査して構造用途 4/5 だけを採り、
//	  3. 1 本ごとに記号を描く（断面＝実断面の対角線／平面＝シンボル 1 つ）
//	という流れで作図する。**記号の大きさ・位置・本数は毎回実物から導く**ので、柱を
//	編集しても（リセットが走れば）記号が嘘にならない。
//
//	使用する SDK API は ci-debug の sdk-grep で実在を確認したもの:
//	  gSDK->GetNamedLayer / FirstMemberObj / NextObject / GetObjectBounds /
//	  ClassNameToID / GetObjectClass / CreateLine、VWSymbolObj、
//	  VWParametricObj（パラメータの読み）。自分自身のハンドルは基底
//	  VWParametric_EventSink の protected メンバ fhObject から取る
//	  （VWFC/PluginSupport/VWExtensionParametric.h:173。IParametricEventSink には
//	  ハンドルを返す公開 API が無く、Recalculate() も引数を取らないため、VWFC が
//	  Execute で詰めるこのメンバが唯一の入口）。
//	実際の見え方（線の太さ・シンボルの向き・リセットの契機）はローカルの VectorWorks で
//	目視確認する（docs/DEV-NOTES.md M12「ローカル確認」）。
//
//	【生成時にダイアログを出さない】PIO の既定は「作るたびに設定ダイアログを出す」
//	（`DefineCustomObject` の `prefWhen` 既定＝`kCustomObjectPrefAlways`）。記号は
//	インポートが自動生成するので、`OnInitXProperties` で `kCustomObjectPrefNever` を
//	宣言しておかないとインポートが記号の数だけ止まる（実機で確認）。
//
//	【ここに残るのは登録と取り次ぎだけ】絵を描くところは本体（ペイロード）側の
//	draw::recalculateColumnMark にある。PIO の登録は Vectorworks に番地を握られるので殻に
//	残すほかないが、描き方は本体へ出せる——そうしておくと**記号の直しが Vectorworks の
//	再起動なしに反映される**（src/PayloadAbi.h / src/PayloadSession.h）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtColumnMark.h"
#include "PayloadAbi.h"
#include "PayloadSession.h"

#include <array>
#include <string>

namespace HomeskzIfcImport
{
	namespace
	{
		// 記号が読む構造材ツールのフィールド名（draw::kField*・draw::kLocalized*）と
		// 構造用途の値（core::kStructuralUse*）は**定義を共有する**。ここで綴りを書き
		// 写すと、書き手側で名前や値を変えたときに記号だけが黙って何も描かなくなる。

		// PIO の定義。**関数ローカル static** で持つ理由は ExtMenu の menuDef と同じ
		// （SDK の非ローカル static を名前空間スコープの初期化子から参照しない）。
		// 点 PIO（シンボルのように 1 点で挿入する）。
		//
		// 【移動・回転でリセットする】記号の絵は対象レイヤの柱の**ワールド位置**に描くが、
		// PIO のジオメトリは挿入点からの相対で保持される。したがって PIO 自体を動かすと記号が
		// まるごとずれ、柱と食い違ったまま戻らない（実機で確認）。リセットすれば実物から描き
		// 直されて正しい位置へ戻るので、移動・回転を契機にしておく。
		const SParametricDef& parametricDef()
		{
			static const SParametricDef def = {/*LocalizedName*/ {PLUGIN_VWR_ID, "columnMarkName"},
											   /*SubType*/ kParametricSubType_Point,
											   /*ResetOnMove*/ true,
											   /*ResetOnRotate*/ true,
											   /*WallInsertOnEdge*/ false,
											   /*WallInsertNoBreak*/ false,
											   /*WallInsertHalfBreak*/ false,
											   /*WallInsertHideCaps*/ false};
			return def;
		}

		// パラメータ（すべて文字列）。既定値は空＝「対象レイヤ未指定なら何も描かない」。
		const SParametricParamDef* paramDefs()
		{
			// SDK は「番兵で終わる配列の先頭ポインタ」を受け取る（SParametricParamDef*）。
			// 器を std::array にしても .data() で同じポインタを渡せるので、C 配列にする
			// 理由は無い（番兵は最後の要素としてそのまま残す）。
			static const std::array<SParametricParamDef, 5> defs = {
				{{kParamTargetLayer,
				  {PLUGIN_VWR_ID, "columnMarkTargetLayer"},
				  "",
				  "",
				  kFieldText,
				  0},
				 {kParamTargetClass,
				  {PLUGIN_VWR_ID, "columnMarkTargetClass"},
				  "",
				  "",
				  kFieldText,
				  0},
				 {kParamMarkStyle,
				  {PLUGIN_VWR_ID, "columnMarkStyle"},
				  kMarkStyleSection,
				  kMarkStyleSection,
				  kFieldText,
				  0},
				 {kParamMarkSymbol, {PLUGIN_VWR_ID, "columnMarkSymbol"}, "", "", kFieldText, 0},
				 {"", {}, "", "", kFieldText, 0}}}; // 終端
			return defs.data();
		}

	} // namespace

	// --------------------------------------------------------------------------
	// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
	// UUID: 5c1f0a76-2d4e-4b93-9a11-7e6c8d240f31  (dev build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtColumnMark,
		/*Event sink*/ CColumnMark_EventSink,
		/*Universal name*/ kColumnMarkUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0x5c1f0a76, 0x2d4e, 0x4b93, 0x9a, 0x11, 0x7e, 0x6c, 0x8d, 0x24, 0x0f, 0x31);
#else
	// UUID: 8b3d29e4-61af-4c07-85d2-3f9b1c6a7e58  (stable build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtColumnMark,
		/*Event sink*/ CColumnMark_EventSink,
		/*Universal name*/ kColumnMarkUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0x8b3d29e4, 0x61af, 0x4c07, 0x85, 0xd2, 0x3f, 0x9b, 0x1c, 0x6a, 0x7e, 0x58);
#endif
	// NOLINTEND(misc-const-correctness)

	// ---------------------------------------------------------------------------
	CExtColumnMark::CExtColumnMark(CallBackPtr cbp)
		: VWExtensionParametric(cbp, parametricDef(), paramDefs())
	{
	}

	CExtColumnMark::~CExtColumnMark() = default;

	// ---------------------------------------------------------------------------
	CColumnMark_EventSink::CColumnMark_EventSink(IVWUnknown* parent)
		: VWParametric_EventSink(parent)
	{
	}

	CColumnMark_EventSink::~CColumnMark_EventSink() = default;

	// ---------------------------------------------------------------------------
	EObjectEvent CColumnMark_EventSink::OnInitXProperties(CodeRefID objectID)
	{
		const EObjectEvent result = VWParametric_EventSink::OnInitXProperties(objectID);

		// 生成のたびに「オブジェクトの設定」ダイアログを出さない（意図はヘッダ参照）。
		// 値は Sint8 なので **SetObjectPropertyChar** を使う（Boolean 版の
		// SetObjectProperty ではない。ISDK.h:1545-1546）。
		gSDK->SetObjectPropertyChar(objectID, kObjXPropShowPrefDialogWhen,
									static_cast<unsigned char>(kCustomObjectPrefNever));

		// 印刷・書き出しの直前にリセットする。**図面として外へ出る瞬間に必ず実物と
		// 一致させる**ための最後の砦で、柱を編集してから記号をリセットし忘れても、
		// 印刷／書き出したものは正しい（docs/DEV-NOTES.md M12「追随の契機」）。
		gSDK->SetObjectProperty(objectID, kObjXPropResetBeforeExport, true);
		return result;
	}

	// ---------------------------------------------------------------------------
	// **描くのは本体（ペイロード）。** ここでするのは「本体を（必要なら読み直して）確保し、
	// 自分のハンドルを渡す」だけ（PayloadSession.h）。読み込めなかったときは**黙って
	// 何もしない**——リセットは図面の記号の数だけ走るので、ここでダイアログを出しても
	// 使いものにならない。エラー表示（kObjectEventHadError）も返さない: 描けなかった
	// ことより、既に描いてある記号を消さずに残すほうが害が少ない。
	EObjectEvent CColumnMark_EventSink::Recalculate()
	{
		// 自分自身のハンドルは基底の protected メンバ（VWFC が Execute で詰める）。
		if (this->fhObject == nil)
			return kObjectEventNoErr;

		const PayloadUse use;
		if (!use.ok())
			return kObjectEventNoErr;

		int event = kObjectEventNoErr;
		std::string error;
		if (!use->recalculate(kVwPayloadPioColumnMark, static_cast<void*>(this->fhObject), event,
							  error))
			return kObjectEventNoErr;
		return static_cast<EObjectEvent>(event);
	}
} // namespace HomeskzIfcImport
