//
//	Extensions/ExtMcpPalette.h
//
//	**MCP ブリッジを常駐させる**（M30 / M41）。殻の時計（OS のタイマー。StartMcpBridgeClock）が
//	数百 ms ごとに本体の draw::serveMcpBridge（src/draw/McpBridge.h）を呼び、スプールに置かれた
//	要求を捌いて**すぐ戻る**。戻っている間は Vectorworks が自由に動くので、**人は図面を触った
//	まま、Claude は図面を読める**。パレット（中身は `resources/common.vwr/html/mcp.html`）は
//	受け付けの様子を見せる窓で、出ている間はその JS タイマーも同じ受け付けを呼ぶ。
//
//	【なぜ OS のタイマーなのか】（M41）SDK に「常時開けておく口」（アイドルコールバック）は
//	無い。M30〜M40 はパレットの JS タイマーを時計にしていたが、埋め込みブラウザ（CEF）は
//	**パレットを隠す・Vectorworks が裏に回ると 60 秒に 1 回まで間引き**、**図面が 1 枚も
//	開いていない間はパレットそのものが出ない**——Claude から実機確認を回すとき、Vectorworks は
//	たいてい裏にいて、再起動の直後は図面が無い。OS のタイマーはどちらでも間引かれずに刻み、
//	そこから gSDK を読み書きできる（[SDK リファレンス「Timers and Notifications」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Timers%20and%20Notifications.md)。
//	docs/dev-notes/milestones/m41-bridge-os-timer.md）。
//
//	【殻に頼まれること】（M38）更新（`vw_update`）と再起動（`vw_restart`）は本体の中では
//	できない（本体は自分を降ろせない。src/PayloadSession.h）。本体は要求を引き取って見え方の
//	`action` に載せて返し、**ここが本体から戻ったあとで済ませ**、結末をもう一度本体へ渡して
//	応答を書いてもらう（入れ替えたなら新しい本体が書く）。入れ替えの流れそのものは
//	src/UpdaterFlow.cpp の RemoteDevUpdateWith で、**インストールの経路は 1 本のまま**。
//	再起動は応答を書いてから頼む（先に頼むと応える者がいなくなる）。
//
//	【隠れていても受け付ける理由】往復では「隠せるものに、隠れている間の権限を持たせない」
//	と決めた（無人の取り込みが見えないところで走り続けたため）。ここで受け付けるのは
//	**同じ計算機の、同じ利用者の** Claude からの要求だけで（スプールは 0700。core/Bridge.h）、
//	取り込み・更新・再起動は**Claude が頼んだときにしか起きない**——勝手に回る時計は持たない。
//	だから隠れていても受け付けてよい。**開発版にしか登録しない**（M38。下記）のも、この
//	権限を利用者向けの配布物に持たせないためである。
//
//	【取り込みの最中は見送る】進捗ダイアログは DoYield で Vectorworks へ制御を返すので、
//	その間にも時計は届く。本体のコードがスタックに載っている間（PayloadInUse）は本体へ
//	入り直さず、「見送った」とだけ返す——描きかけの図面を読ませないため。`vw_run_test` の
//	1 周も同じで、走っている間に届いた時計は見送る（その 1 周を頼んだ要求には、終わってから
//	応える）。
//
//	【いつ動き出すか】（M41）**Vectorworks の起動から**（plugin_module_main が殻の時計を
//	仕掛け、最初の刻みは起動の 10 秒後）。メニュー「MCP ブリッジを表示…」もパレットも
//	要らない。パレットは様子を見たいときに出す。
//
//	【殻に置く理由】拡張の登録は Vectorworks に番地を握られる（PIO と同じ）。ここに置くのは
//	**登録と取り次ぎ**だけで、受け付けそのものは本体（CLAUDE.md「殻と本体」）。
//
//	【開発版だけ】（M38）M37 までは安定版にも登録していた（読むだけの道具しか無かった）。
//	取り込み・更新・再起動を頼めるようになったので、開発版にだけ登録する（src/ModuleMain.cpp）。
//	安定版はクラスを持つがどこにも登録しない（UUID とユニバーサル名は据え置く）。
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
	// を出し、本体の側は行末の書き方に癖がある。手で書けば数行の if で済む（M24 の往復の
	// パレットから引き継いだ作り）。
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
		// functionName に割るかは実機で未確認なので、ここで自分で "." の前後に割ってから
		// OnFunctionCall へ渡す（割り方が一致していれば二重に同じことをするだけ）。
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

	// **殻の時計を動かす**（M41）。OS のタイマー（mac: CFRunLoopTimer / Windows: SetTimer）
	// で受け付けを刻む——パレットが隠れていても、図面が 1 枚も開いていなくても受け付ける。
	// plugin_module_main が開発版でだけ呼ぶ（何度呼んでも 1 度しか動かさない）。
	void StartMcpBridgeClock();
} // namespace HomeskzIfcImport
