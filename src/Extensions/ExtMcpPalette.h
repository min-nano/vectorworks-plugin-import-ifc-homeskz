//
//	Extensions/ExtMcpPalette.h
//
//	**MCP ブリッジを常駐させるモードレスなパレット**（M30。docs/DEV-NOTES.md）。中身の
//	HTML（`resources/common.vwr/html/mcp.html`）の JS タイマーが数百 ms ごとに
//	`vwmcp.serve` を呼び、そのたびに本体の draw::serveMcpBridge（src/draw/McpBridge.h）が
//	スプールに置かれた要求を処理して**すぐ戻る**。戻っている間は Vectorworks が通常どおり
//	動くので、**人は図面を操作したまま、Claude は図面を読める**。
//
//	【なぜパレットなのか】SDK に「常時呼ばれる入口」（アイドルコールバック）は無く
//	（[SDK リファレンス「レイヤ・ストーリ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layers%20and%20Stories.md)）、
//	周期的に動く手段は**パレットの JS タイマー**だけである。JS のタイマーから C++ へ
//	メインスレッドで呼び出しが届き、**パレットを隠しても時計は止まらない**ことは M24 の往復の
//	パレットで実機に確かめてある（docs/DEV-NOTES.md M25「隠れたパレットは止まらない」）。
//	往復ではこの性質が不具合の原因だったが、ここでは**常駐が必要なのでそのまま使う**。
//
//	【殻に要求されること】（M38）更新（`vw_update`）と再起動（`vw_restart`）は本体の中では
//	実行できない（本体は自分をアンロードできない。src/PayloadSession.h）。本体は要求を
//	受け取って表示状態の `action` に載せて返し、**ここが本体から戻ったあとで実行し**、結末を
//	もう一度本体へ渡して応答を書いてもらう（入れ替えたなら新しい本体が書く）。入れ替えの
//	流れそのものは src/UpdaterFlow.cpp の RemoteDevUpdateWith で、**インストールの経路は
//	1 本のまま**。再起動は応答を書いてから要求する（先に要求すると応答する者がいなくなる）。
//
//	【隠れていても受け付ける理由】ここで受け付けるのは**同じ計算機の、同じ利用者の**
//	Claude からの要求だけで（スプールは 0700。core/Bridge.h）、取り込み・更新・再起動は
//	**Claude が要求したときにしか起きない**——自発的に処理を起こす時計は持たない。だから
//	隠れていても受け付けてよい。**開発版にしか登録しない**（M38。下記）のも、この権限を
//	利用者向けの配布物に持たせないためである。往復では「隠せるものに、隠れている間の権限を
//	持たせない」と決めた（無人の取り込みが見えないところで実行され続けたため）が、ここには
//	当たらない。
//
//	【取り込みの最中は見送る】本体のコードがスタックに載っている間（PayloadInUse）は本体へ
//	入り直さず、「見送った」とだけ返す——描画途中の図面を読ませないため。進捗ダイアログは
//	DoYield で Vectorworks へ制御を返すので、その間にも時計は届く。`vw_run_test` の 1 周も
//	同じで、実行中に届いた時計は見送る（その 1 周を求めた要求には、終わってから応答する）。
//
//	【いつ動き出すか】パレットのページが読み込まれたとき。メニュー「MCP ブリッジを表示…」
//	（Extensions/ExtMcpMenu.h）が表示を要求する。**Vectorworks を再起動してもパレットは
//	開いたまま**なので（実機で確認。docs/DEV-NOTES.md M30）、メニューを実行するのは最初の
//	1 回だけ。
//
//	【殻に置く理由】拡張の登録は Vectorworks に番地を保持される（PIO と同じ）。ここに置くのは
//	**登録と取り次ぎ**だけで、受け付けそのものは本体（CLAUDE.md「殻と本体」）。
//
//	【開発版だけ】（M38）開発版にだけ登録する（src/ModuleMain.cpp）。安定版はクラスを持つが
//	どこにも登録しない（UUID とユニバーサル名は変更しない）。取り込み・更新・再起動を要求
//	できるようになったためで、M37 までは安定版にも登録していた（読むだけの道具しか
//	無かった）。
//

#pragma once

#include "VectorworksSDK.h"

#include <vector>

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// JS からの呼び出しを受ける側。**SDK のディスパッチ表のマクロ（DEFINE_/BEGIN_/
	// ADD_WebPalette_…）は使わない**——宣言に override が無く -Winconsistent-missing-override
	// を出し、定義側のマクロは行末の書き方が特殊である。手で書けば数行の if で済む（M24 の
	// 往復のパレットから引き継いだ作り）。
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
		// **登録した名前そのままで受ける。** VWFC の OnFunction が名前をどう objName /
		// functionName に分割するかは実機で未確認なので、ここで自分で "." の前後に分割して
		// から OnFunctionCall へ渡す（分割の仕方が一致していれば二重に同じことをするだけ）。
		void OnFunction(const TXString& name, const std::vector<nlohmann::json>& args,
						VectorWorks::UI::IJSFunctionCallbackContext* context) override;
	};

	// ------------------------------------------------------------------------
	// パレットそのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtMcpPalette : public VWExtensionWebPalette
	{
		DEFINE_VWPaletteExtension;

	public:
		// REGISTER_Extension は T(cbp) で作る。基底は cbp を受け取らないので使わない。
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
