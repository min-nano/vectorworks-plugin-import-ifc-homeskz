//
//	Extensions/ExtFeedbackPalette.h
//
//	**実機フィードバックの往復を回すモードレスなパレット**（M24。docs/DEV-NOTES.md）。
//	取り込み結果を PR へ送った（＝往復に入った）あとに開き、図面を操作したまま
//
//	    新しいビルドを見つける → 入れる → 同じ条件で取り込む → 投稿する
//
//	を、**Claude が「もう要らない」と合図する**（`<!-- homeskz-ifc-feedback v1 control=stop -->`）
//	か、人がここの「往復を止める」を押すまで繰り返す。M23 では待つとモーダルのダイアログが
//	図面を塞いだので、次の周は人がメニューを押して始めていた——待機をモードレスへ載せ替え
//	たのがこの拡張である（駆動そのものは src/FeedbackLoop.h、殻の実物は
//	src/FeedbackLoopHost.h）。
//
//	【SDK の 5 つ目の拡張種別】`IExtensionWebPalette`（基底は VWFC の
//	`VWExtensionWebPalette`）。中身は HTML/JS で、`.vwr` の `html/index.html` を埋め込み
//	ブラウザに出す（`GetStandardURL` の既定）。JS 側から C++ へは、`OnInit` で登録した
//	関数（`homeskz.tick` 等）を呼ぶと `OnFunction` に届き、Promise を `Resolve` で返す。
//	タイマーは JS の `setInterval`——**SDK には遅延実行の口が無い**ので、周期的に動く唯一の
//	手立てがこれである（SDK リファレンス Findings「Layers and Stories」の注記）。
//
//	【実機未確認】この拡張種別は SDK リファレンス側で**ヘッダ根拠と構文チェックまで**しか
//	確かめられていない（[Findings「モードレス（非モーダル）なパレット」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)）。
//	実機で確かめること: (1) パレットが開き、図面を操作しながら見えるか (2) JS のタイマー
//	から `homeskz.tick` が届き続けるか (3) `SetWebPaletteVisibility` で表示・非表示が
//	切り替わるか (4) 取り込み（進捗ダイアログの DoYield）の最中に届く Tick が無視されるか。
//	**効かなくても手動の周は壊れない**——メニューからの取り込みは M23 のまま動く。
//
//	【殻に置く理由】拡張の登録は Vectorworks に番地を握られる（PIO と同じ）。降ろせる
//	本体に置くと、降ろした瞬間に JS からの呼び出し先が消えて落ちる（Findings「Plug-in
//	Modules」）。ここに置くのは**登録と取り次ぎ**だけで、往復の記憶と取り込みは本体、
//	駆動は殻の自動アップデートの一部（src/FeedbackLoop.h）。
//
//	【開発版だけ】往復するのは PR のビルドなので、登録は VW_DEV_BUILD のときだけ
//	（src/ModuleMain.cpp）。安定版はこのクラスを持つがどこにも登録しない。
//

#pragma once

#include "VectorworksSDK.h"

#include <vector>

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// JS からの呼び出しを受ける側（`IWebJavaScriptProvider` の VWFC 実装を継承）。
	class CFeedbackPaletteJS : public VWExtensionPaletteJSProvider
	{
	public:
		CFeedbackPaletteJS(IVWUnknown* parent);
		~CFeedbackPaletteJS() override;

		// JS へ関数を登録する（homeskz.tick / stop / checkNow / hide）。
		void OnInit(VectorWorks::Extension::IWebJavaScriptProvider::IInitContext* context) override;

		// objName / functionName で分岐する（基底の純粋仮想）。**SDK のディスパッチ表の
		// マクロ（DEFINE_/BEGIN_/ADD_WebPalette_…）は使わない**——宣言に override が無く
		// -Winconsistent-missing-override を出し、本体の側は行末の書き方に癖がある。手で
		// 書けば 4 行の if で済む。
		void OnFunctionCall(const TXString& objName, const TXString& functionName,
							const std::vector<nlohmann::json>& args,
							VectorWorks::UI::IJSFunctionCallbackContext* context) override;

	protected:
		// **登録した名前そのままで受ける。** VWFC の OnFunction が名前をどう objName /
		// functionName に割るかは実機で未確認なので、ここで自分で "." の前後に割ってから
		// OnFunctionCall へ渡す（割り方が一致していれば二重に同じことをするだけ）。
		void OnFunction(const TXString& name, const std::vector<nlohmann::json>& args,
						VectorWorks::UI::IJSFunctionCallbackContext* context) override;

	private:
		// 1 関数 1 ハンドラ。いずれも駆動へ取り次いで、見え方（JSON）を Promise で返す。
		void OnTick(const TXString& objName, const TXString& functionName,
					const std::vector<nlohmann::json>& args,
					VectorWorks::UI::IJSFunctionCallbackContext* context);
		void OnStop(const TXString& objName, const TXString& functionName,
					const std::vector<nlohmann::json>& args,
					VectorWorks::UI::IJSFunctionCallbackContext* context);
		void OnCheckNow(const TXString& objName, const TXString& functionName,
						const std::vector<nlohmann::json>& args,
						VectorWorks::UI::IJSFunctionCallbackContext* context);
		void OnHide(const TXString& objName, const TXString& functionName,
					const std::vector<nlohmann::json>& args,
					VectorWorks::UI::IJSFunctionCallbackContext* context);
	};

	// ------------------------------------------------------------------------
	// パレットそのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtFeedbackPalette : public VWExtensionWebPalette
	{
		DEFINE_VWPaletteExtension;

	public:
		// REGISTER_Extension は T(cbp) で作る。基底（VWExtensionWebPalette）は cbp を
		// 受け取らないので、ここで受けて捨てる。
		CExtFeedbackPalette(CallBackPtr cbp);
		~CExtFeedbackPalette() override;

		// JS 受け口の登録（VWFC の DefineSink<T>）。
		void DefineSinks() override;

		// IExtensionWebPalette
		TXString VCOM_CALLTYPE GetTitle() override;
		bool VCOM_CALLTYPE GetInitialSize(ViewCoord& outCX, ViewCoord& outCY) override;
		bool VCOM_CALLTYPE GetMinimalSize(ViewCoord& outCX, ViewCoord& outCY) override;
	};
} // namespace HomeskzIfcImport
