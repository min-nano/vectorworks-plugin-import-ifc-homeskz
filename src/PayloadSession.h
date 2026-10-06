//
//	PayloadSession.h
//
//	**殻が保持する「いま読み込まれている本体」1 つ。** 読み込みは重い（本体が SDK の静的
//	ライブラリをすべて含むので 0.3〜0.4 秒。[SDK リファレンスの実測](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）
//	ので、**一度読み込んだら保持したままにする**。アンロードするのは入れ替えのときだけで、
//	その判定を入口のたびに行うのがこのファイルの役割である。
//
//	【いつ入れ替えるか】**入口に入った時点で、他に本体のコードが実行中でなければ**
//	（＝入れ子の深さが 0）、同梱ファイルの印（大きさ・更新時刻。PayloadHost.h の
//	PayloadStamp）を確認し、読み込んだときと違っていたらアンロードして再読み込みする。
//	深さが 0 という条件が要点で、**本体のコードがスタックに 1 つでも載っている間は決して
//	アンロードしない**（アンロードした瞬間にそのコードと静的データが消える）。
//
//	  * 自動アップデートが新しい本体を置く → 次の取り込み・次の PIO リセットから新しい
//	    コードが動く。**Vectorworks の再起動は要らない。**
//	  * 置き換えが検出できなくても（印が取れない等）壊れはしない——古いまま動き続け、
//	    次の入口でもう一度確認するだけ。
//
//	【なぜ「毎回読み込んで毎回アンロードする」にしないか】SDK リファレンスの実機確認
//	プラグインはメニュー 1 つしか入口が無いのでその作りでよいが、本プラグインは**PIO の
//	リセットが同じ本体を使う**。取り込み直後は記号・耐力壁が数百個リセットされるので、
//	そのたびに 0.3 秒かかると実用的な速度にならない。
//
//	【スレッド】Vectorworks はメニューコマンドも PIO のリセットもメインスレッドから
//	呼ぶので、ここでは排他を持たない（SDK 側も同じ前提で書かれている）。
//

#pragma once

#include "PayloadHost.h"

#include <string>

namespace HomeskzIfcImport
{
	// 殻が plugin_module_main で受け取った SDK の CallBackPtr を記録する。本体は自分の
	// gSDK / gCBP を持たないまま読み込まれるので、これを渡して初期化させる
	// （PayloadAbi.h の VwPayloadHost::callbacks）。
	void RememberSdkCallbacks(void* callbacks);

	// -----------------------------------------------------------------------
	// **本体を使う区間 1 つ。** 入口（メニューコマンド・PIO のリセット）の先頭で 1 つ作り、
	// 抜けるまで保持する。作った時点で必要なら入れ替え、存在する間はアンロードさせない。
	//
	//	    PayloadUse use;
	//	    if (!use.ok()) { …use.error() を表示する／何も表示せずに中断する… }
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

	// 読み込まれている本体を明示的にアンロードする。**入れ子の深さが 0 のときだけ機能する**
	// （実行中はアンロードしない）。アンロードできたら true。自動アップデートが「入れ替えた
	// ので次から新しいほうを使う」と確定させるために使う。
	bool ReleaseLoadedPayload();

	// **いま本体のコードがスタックに載っているか**（入れ子の深さが 0 でないか）。
	// 取り込みの進捗ダイアログは DoYield で Vectorworks へ制御を返すので、その間にも
	// パレットの時計は届く。そこで本体へ入り直すと**描画途中の図面を読む**ことになるので、
	// 時計で本体を呼ぶ側（MCP ブリッジのパレット。Extensions/ExtMcpPalette.cpp）はこれを
	// 確認し、載っている間は呼び出しを見送る。
	bool PayloadInUse();
} // namespace HomeskzIfcImport
