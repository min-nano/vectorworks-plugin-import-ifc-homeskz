//
//	Extensions/ExtMcpPalette.h
//
//	**MCP ブリッジを常駐させるモードレスなパレット**（M30。docs/DEV-NOTES.md）。中身の
//	HTML（`resources/common.vwr/html/mcp.html`）の JS タイマーが数百 ms ごとに
//	`vwmcp.serve` を呼び、そのたびに本体の draw::serveMcpBridge（src/draw/McpBridge.h）が
//	スプールに置かれた要求を捌いて**すぐ戻る**。戻っている間は Vectorworks が自由に動くので、
//	**人は図面を触ったまま、Claude は図面を読める**。
//
//	【なぜパレットなのか】SDK に「常時開けておく口」（アイドルコールバック）は無く
//	（[SDK リファレンス「レイヤ・ストーリ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layers%20and%20Stories.md)）、
//	周期的に動く手立ては**パレットの JS タイマー**だけである。これは実機フィードバックの
//	往復（Extensions/ExtFeedbackPalette.h）で実機に確かめてある——JS のタイマーから C++ へ
//	メインスレッドで届き、**パレットを隠しても時計は止まらない**（docs/DEV-NOTES.md M24
//	「隠れたパレットは止まらない」）。往復ではこの性質が事故の元だったが、ここでは
//	**常駐がほしいのでそのまま使う**。
//
//	【隠れていても受け付ける理由】往復では「隠せるものに、隠れている間の権限を持たせない」
//	と決めた（無人の取り込みが見えないところで走り続けたため）。ここで受け付けるのは
//	**図面を読むだけの道具**（draw/McpBridge.cpp の kTools）で、相手は**同じ計算機の、
//	同じ利用者の** Claude だけ（スプールは 0700。core/Bridge.h）。書く道具を足すときは、
//	隠れている間も受け付けてよいかをもう一度考えること。
//
//	【取り込みの最中は見送る】進捗ダイアログは DoYield で Vectorworks へ制御を返すので、
//	その間にも時計は届く。本体のコードがスタックに載っている間（PayloadInUse）は本体へ
//	入り直さず、「見送った」とだけ返す——描きかけの図面を読ませないため。
//
//	【いつ動き出すか】パレットのページが読み込まれたとき。メニュー「MCP ブリッジを表示…」
//	（Extensions/ExtMcpMenu.h）が表示を頼む。**Vectorworks を起動し直したときにパレットが
//	自動で開き直されるか**（＝メニューを押さなくても受け付けるか）は実機未確認で、開き直され
//	なければ起動のたびにメニューを 1 回押すことになる。
//
//	【殻に置く理由】拡張の登録は Vectorworks に番地を握られる（PIO と同じ）。ここに置くのは
//	**登録と取り次ぎ**だけで、受け付けそのものは本体（CLAUDE.md「殻と本体」）。
//
//	【両方のビルドに】往復のパレットと違って、安定版にも登録する（MCP ブリッジのメニューは
//	安定版にもあるため）。
//

#pragma once

#include "VectorworksSDK.h"

#include <vector>

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// JS からの呼び出しを受ける側（Extensions/ExtFeedbackPalette.h の CFeedbackPaletteJS と
	// 同じ作り。ディスパッチ表のマクロを使わない理由もそちら）。
	class CMcpPaletteJS : public VWExtensionPaletteJSProvider
	{
	public:
		CMcpPaletteJS(IVWUnknown* parent);
		~CMcpPaletteJS() override;

		// JS へ関数を登録する（vwmcp.serve）。
		void OnInit(VectorWorks::Extension::IWebJavaScriptProvider::IInitContext* context) override;

		void OnFunctionCall(const TXString& objName, const TXString& functionName,
							const std::vector<nlohmann::json>& args,
							VectorWorks::UI::IJSFunctionCallbackContext* context) override;

	protected:
		// 登録した名前そのままで受け、"." の前後に割って OnFunctionCall へ渡す
		// （ExtFeedbackPalette.h の OnFunction と同じ理由）。
		void OnFunction(const TXString& name, const std::vector<nlohmann::json>& args,
						VectorWorks::UI::IJSFunctionCallbackContext* context) override;
	};

	// ------------------------------------------------------------------------
	// パレットそのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtMcpPalette : public VWExtensionWebPalette
	{
		DEFINE_VWPaletteExtension;

	public:
		// REGISTER_Extension は T(cbp) で作る。基底は cbp を受け取らないので捨てる。
		CExtMcpPalette(CallBackPtr cbp);
		~CExtMcpPalette() override;

		void DefineSinks() override;

		// IExtensionWebPalette
		TXString VCOM_CALLTYPE GetTitle() override;
		bool VCOM_CALLTYPE GetInitialSize(ViewCoord& outCX, ViewCoord& outCY) override;
		bool VCOM_CALLTYPE GetMinimalSize(ViewCoord& outCX, ViewCoord& outCY) override;
	};

	// パレットを表示する（メニュー「MCP ブリッジを表示…」から）。
	void ShowMcpPalette();
} // namespace HomeskzIfcImport
