//
//	PayloadSession.cpp
//
//	PayloadSession.h の実装。**SDK の型は使わない**（PayloadHost.cpp と同じ理由）。
//

#include "PluginPrefix.h"
#include "PayloadSession.h"

#include <string>

namespace HomeskzIfcImport
{
	namespace
	{
		// 状態はすべて関数ローカル static で持つ（名前空間スコープの静的初期化順序に
		// 依存しない。ExtMenu の menuDef と同じ作法）。
		Payload& ThePayload()
		{
			static Payload sPayload;
			return sPayload;
		}

		// **本体のコードがスタックに載っている深さ。** 0 のときだけアンロードしてよい。
		int& Depth()
		{
			static int sDepth = 0;
			return sDepth;
		}

		void*& Callbacks()
		{
			static void* sCallbacks = nullptr;
			return sCallbacks;
		}
	} // namespace

	void RememberSdkCallbacks(void* callbacks)
	{
		Callbacks() = callbacks;
	}

	// -----------------------------------------------------------------------
	PayloadUse::PayloadUse()
	{
		Payload& payload = ThePayload();

		// **入れ替えの判定は深さ 0 のときだけ。** 本体のコードが 1 つでも実行中のときに
		// アンロードすると、そのコードと静的データが消える（PayloadSession.h）。
		if (Depth() == 0 && payload.isLoaded())
		{
			const PayloadStamp now = StampOf(BundledPayloadPath());
			// 印が取れなかったときは**何もしない**（古いまま動かすほうが安全で、次の
			// 入口でもう一度確認する機会がある）。
			if (now.valid && payload.stamp().valid && now != payload.stamp())
				payload.unload();
		}

		if (!payload.isLoaded())
		{
			// 再読み込みに失敗したら本体は読み込まれていない状態になる。**古いほうへ戻す手段は
			// 無い**（アンロードした時点でコードは消えている）ので、呼び出し側が理由を表示する。
			// アップデータは新しい本体を一時ファイルへダウンロードしてから置き換える約束なので、
			// 途中まで書かれたファイルを読み込むことは無い（scripts/vw-update.*）。
			if (!payload.load(Callbacks(), fError))
				return;
		}

		++Depth();
		fPayload = &payload;
	}

	PayloadUse::~PayloadUse()
	{
		if (fPayload != nullptr)
			--Depth();
	}

	// -----------------------------------------------------------------------
	bool ReleaseLoadedPayload()
	{
		if (Depth() != 0)
			return false;
		ThePayload().unload();
		return true;
	}

	bool PayloadInUse()
	{
		return Depth() != 0;
	}
} // namespace HomeskzIfcImport
