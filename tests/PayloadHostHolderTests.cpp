//
//	PayloadHostHolderTests.cpp
//
//	**本体が殻の記憶域を持ち続けないこと**（src/PayloadHostHolder.h）。
//
//	これは実機で Vectorworks ごと落ちた壊れ方の回帰テストである。殻が渡す
//	`const VwPayloadHost*` を本体がポインタのまま持つと、殻がそれをローカルに置いていた
//	場合に load から戻った時点で腐り、次に触った瞬間にスタックの番地へ分岐して落ちる
//	（[SDK リファレンス「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。
//	**コンパイルもリンクも CI の実ビルドも通る**——だから、写しているかどうかを
//	「渡した記憶域を後から塗り潰しても中身が生きているか」で確かめる。
//
//	SDK にもプラットフォームにも依存しないので、ここで完結する。
//

#include "TestFramework.h"
#include "PayloadHostHolder.h"

#include <cstring>

using namespace HomeskzIfcImport::payload;

namespace
{
	// 殻が渡す体の VwPayloadHost を 1 つ作る。
	VwPayloadHost MakeHost(void* callbacks)
	{
		VwPayloadHost host{};
		host.size = static_cast<unsigned int>(sizeof(VwPayloadHost));
		host.abiVersion = VW_PAYLOAD_ABI_VERSION;
		host.callbacks = callbacks;
		return host;
	}

	// 「殻の記憶域」の代わり。ここを塗り潰しても本体が無事なら、写せている。
	int gCallbackTarget = 0;
} // namespace

// ---------------------------------------------------------------------------

TEST(adopt_copies_the_struct_so_the_callers_storage_can_die)
{
	HostHolder holder;
	{
		// **わざとローカルに置く**（実機で落ちたときの殻がこの形だった）。
		VwPayloadHost host = MakeHost(&gCallbackTarget);
		CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadOk));
		// 呼び出し側の記憶域を塗り潰す（スコープを抜けた後の使い回しの模擬）。
		std::memset(&host, 0xAB, sizeof(host));
	}
	CHECK(holder.valid());
	CHECK_EQ(holder.callbacks(), static_cast<void*>(&gCallbackTarget));
}

TEST(adopt_rejects_a_null_host)
{
	HostHolder holder;
	CHECK_EQ(holder.adopt(nullptr), static_cast<int>(kVwPayloadErrHost));
	CHECK(!holder.valid());
	CHECK_EQ(holder.callbacks(), static_cast<void*>(nullptr));
}

TEST(adopt_rejects_a_different_abi_version)
{
	// 殻と本体は別々に配られるので、**版の食い違いは実行時にしか気付けない**。
	HostHolder holder;
	VwPayloadHost host = MakeHost(&gCallbackTarget);
	host.abiVersion = VW_PAYLOAD_ABI_VERSION + 1u;
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadErrAbi));
	CHECK(!holder.valid());
}

TEST(adopt_rejects_a_short_struct)
{
	// size は abiVersion と二重の歯止め——**短い構造体を長いつもりで読む**事故を防ぐ。
	HostHolder holder;
	VwPayloadHost host = MakeHost(&gCallbackTarget);
	host.size = static_cast<unsigned int>(sizeof(VwPayloadHost)) - 1u;
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadErrAbi));
	CHECK(!holder.valid());
}

TEST(adopt_accepts_a_longer_struct_from_a_newer_shell)
{
	// 殻のほうが新しく、後ろに知らない項目が付いていても構わない（こちらが知っている
	// 分だけ写す）。
	HostHolder holder;
	VwPayloadHost host = MakeHost(&gCallbackTarget);
	host.size = static_cast<unsigned int>(sizeof(VwPayloadHost)) + 16u;
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadOk));
	CHECK(holder.valid());
	CHECK_EQ(holder.callbacks(), static_cast<void*>(&gCallbackTarget));
}

TEST(adopt_rejects_a_missing_callback_pointer)
{
	// callbacks が無ければ GS_InitializeVCOM を呼べない＝本体は SDK を一切使えない。
	// 読み込んでから気付くより、ここで断るほうがよい。
	HostHolder holder;
	VwPayloadHost host = MakeHost(nullptr);
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadErrHost));
	CHECK(!holder.valid());
}

TEST(forget_drops_everything)
{
	// 降ろす直前に殻への参照を手放すのが本体の仕事（src/payload/PayloadMain.cpp の
	// vw_payload_shutdown）。
	HostHolder holder;
	VwPayloadHost host = MakeHost(&gCallbackTarget);
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadOk));
	holder.forget();
	CHECK(!holder.valid());
	CHECK_EQ(holder.callbacks(), static_cast<void*>(nullptr));
}

TEST(a_failed_adopt_forgets_what_was_there_before)
{
	// 途中まで受け取って壊れた状態を残さない。
	HostHolder holder;
	VwPayloadHost good = MakeHost(&gCallbackTarget);
	CHECK_EQ(holder.adopt(&good), static_cast<int>(kVwPayloadOk));
	CHECK_EQ(holder.adopt(nullptr), static_cast<int>(kVwPayloadErrHost));
	CHECK(!holder.valid());
}

// ---------------------------------------------------------------------------

TEST_MAIN();
