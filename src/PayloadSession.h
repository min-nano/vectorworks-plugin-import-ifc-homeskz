//
//	PayloadSession.h
//
//	**殻が持つ「いま載っている本体」1 つ。** 読み込みは重い（本体が SDK の静的ライブラリを
//	丸ごと抱えるので 0.3〜0.4 秒。[SDK リファレンスの実測](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）
//	ので、**一度読んだら載せたままにする**。降ろすのは入れ替えのときだけで、その判定を
//	入口のたびに行うのがこのファイルの仕事である。
//
//	【なぜ「毎回読んで毎回降ろす」にしないか】SDK リファレンスの実機確認プラグインは
//	メニュー 1 つしか入口が無いのでその作りでよいが、本プラグインは**PIO のリセットが
//	同じ本体を使う**。取り込み直後は記号・耐力壁が数百個リセットされるので、そのたびに
//	0.3 秒を払うと現実的な速さにならない。
//
//	【いつ載せ替えるか】**入口に入った時点で、他に本体のコードが走っていなければ**
//	（＝入れ子の深さが 0）、同梱ファイルの印（大きさ・更新時刻。PayloadHost.h の
//	PayloadStamp）を見て、読んだときと違っていたら降ろして読み直す。深さが 0 という
//	条件が肝で、**本体のコードがスタックに 1 つでも載っている間は決して降ろさない**
//	（降ろした瞬間にそのコードと静的データが消える）。
//
//	  * 自動アップデートが新しい本体を置く → 次の取り込み・次の PIO リセットから新しい
//	    コードが動く。**Vectorworks の再起動は要らない。**
//	  * 置き換えが検出できなくても（印が取れない等）壊れはしない——古いまま動き続け、
//	    次の入口でもう一度見るだけ。
//
//	【スレッド】Vectorworks はメニューコマンドも PIO のリセットもメインスレッドから
//	呼ぶので、ここでは排他を持たない（SDK 側も同じ前提で書かれている）。
//

#pragma once

#include "PayloadHost.h"

#include <string>

namespace HomeskzIfcImport
{
	// 殻が plugin_module_main で受け取った SDK の CallBackPtr を預ける。本体は自分の
	// gSDK / gCBP を持たないまま読み込まれるので、これを渡して初期化させる
	// （PayloadAbi.h の VwPayloadHost::callbacks）。
	void RememberSdkCallbacks(void* callbacks);

	// -----------------------------------------------------------------------
	// **本体を使う区間 1 つ。** 入口（メニューコマンド・PIO のリセット）の頭で 1 つ作り、
	// 抜けるまで生かす。作った時点で必要なら載せ替え、生きている間は降ろさせない。
	//
	//	    PayloadUse use;
	//	    if (!use.ok()) { …use.error() を見せる／黙って諦める… }
	//	    use->runImport(err);
	//
	class PayloadUse
	{
	public:
		PayloadUse();
		~PayloadUse();

		PayloadUse(const PayloadUse&) = delete;
		PayloadUse& operator=(const PayloadUse&) = delete;

		// 本体が使える状態か。false のときだけ error() に理由が入る。
		bool ok() const
		{
			return fPayload != nullptr;
		}
		const std::string& error() const
		{
			return fError;
		}

		Payload* operator->() const
		{
			return fPayload;
		}

	private:
		Payload* fPayload = nullptr;
		std::string fError;
	};

	// 載っている本体を明示的に降ろす。**入れ子の深さが 0 のときだけ効く**（走っている
	// 途中では降ろさない）。降ろせたら true。自動アップデートが「入れ替えたので次から
	// 新しいほうを使う」と言い切るために使う。
	bool ReleaseLoadedPayload();
} // namespace HomeskzIfcImport
