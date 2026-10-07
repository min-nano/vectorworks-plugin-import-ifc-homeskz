//
//	PayloadHostHolderTests.cpp
//
//	**本体が殻の記憶域を持ち続けないこと**（src/PayloadHostHolder.h）を確かめる。
//
//	複製しているかどうかは「渡した記憶域を後から上書きしても中身が有効なままか」で
//	確かめる。この壊れ方は**コンパイルもリンクも CI の実ビルドも通る**ので、こうして
//	確かめるしかない。
//
//	これは実機で Vectorworks ごと異常終了した壊れ方の回帰テストである。殻が渡す
//	`const VwPayloadHost*` を本体がポインタのまま持つと、殻がそれをローカルに置いていた
//	場合に load から戻った時点で無効になり、次にアクセスした瞬間にスタックの番地へ分岐して
//	異常終了する
//	（[SDK リファレンス「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)）。
//
//	SDK にもプラットフォームにも依存しないので、ここで完結する。
//

#include "TestFramework.h"
#include "PayloadHostHolder.h"

#include <cstring>
#include <string>
#include <vector>

using namespace HomeskzIfcImport::payload;

namespace
{
	// 殻が渡すものに見立てた VwPayloadHost を 1 つ作る。
	VwPayloadHost MakeHost(void* callbacks)
	{
		VwPayloadHost host{};
		host.size = static_cast<unsigned int>(sizeof(VwPayloadHost));
		host.abiVersion = VW_PAYLOAD_ABI_VERSION;
		host.callbacks = callbacks;
		return host;
	}

	// 「殻の記憶域」の代わり。ここを上書きしても本体が影響を受けなければ、複製できている。
	int gCallbackTarget = 0;

	// 殻が貸すものに見立てたスクリプト実行。呼ばれた引数を記録し、固定の出力を返す。
	// **戻す文字列は「殻が所有し、次の呼び出しまで有効」**という契約なので、
	// 実物と同じく static に置く（src/PayloadAbi.h）。
	std::string gScriptCall;
	std::string gScriptReply = "ok";
	int gScriptStatus = kVwPayloadOk;

	int FakeRunScript(const char* scriptName, const char* const* args, unsigned int argc,
					  const char** out)
	{
		static std::string sOut;
		gScriptCall = (scriptName != nullptr) ? scriptName : "";
		for (unsigned int i = 0; i < argc; ++i)
			gScriptCall +=
				std::string(" ") + ((args != nullptr && args[i] != nullptr) ? args[i] : "?");
		sOut = gScriptReply;
		if (out != nullptr)
			*out = sOut.c_str();
		return gScriptStatus;
	}
} // namespace

// ---------------------------------------------------------------------------

TEST(adopt_copies_the_struct_so_the_callers_storage_can_die)
{
	HostHolder holder;
	{
		// **わざとローカルに置く**（実機で異常終了したときの殻がこの形だった）。
		VwPayloadHost host = MakeHost(&gCallbackTarget);
		CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadOk));
		// 呼び出し側の記憶域を上書きする（スコープを抜けた後の使い回しの模擬）。
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
	// 分だけ複製する）。
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
	// アンロードする直前に殻への参照を手放すのが本体の仕事（src/payload/PayloadMain.cpp の
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
// 殻から借りるもの（M23）。**返ってきた文字列は複製する・関数ポインタはそのまま保持する**。
// ---------------------------------------------------------------------------

TEST(a_host_without_a_script_hook_is_accepted)
{
	// 古い殻（スクリプトを貸さない）でも本体は動く——同梱スクリプトに頼るもの（実機テストの
	// 一時ファイルの片付け＝vw-update の q-pr-state）だけが使えなくなる。
	HostHolder holder;
	VwPayloadHost host = MakeHost(&gCallbackTarget);
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadOk));
	CHECK(!holder.canRunScripts());
	std::string out = "not touched";
	CHECK(!holder.runScript("vw-update", {"q-dev"}, out));
	CHECK(out.empty()); // 失敗しても出力は空にして返す（呼び出し側が古い値を読まない）
}

TEST(run_script_passes_the_arguments_and_copies_the_reply)
{
	HostHolder holder;
	VwPayloadHost host = MakeHost(&gCallbackTarget);
	host.runBundledScript = &FakeRunScript;
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadOk));
	CHECK(holder.canRunScripts());

	gScriptReply = "source=keychain\nok=yes";
	gScriptStatus = kVwPayloadOk;
	std::string out;
	CHECK(holder.runScript("vw-update", {"do-install", "u", "n"}, out));
	CHECK_EQ(gScriptCall, std::string("vw-update do-install u n"));
	CHECK_EQ(out, std::string("source=keychain\nok=yes"));

	// 引数が無くても呼べる（nullptr を渡す形になる）。
	CHECK(holder.runScript("vw-update", {}, out));
	CHECK_EQ(gScriptCall, std::string("vw-update"));

	// 殻が失敗を返したら false（出力は使わせない）。
	gScriptStatus = kVwPayloadErrHost;
	CHECK(!holder.runScript("vw-update", {"q-dev"}, out));
	CHECK(out.empty());
	gScriptStatus = kVwPayloadOk;
}

TEST(forget_drops_the_script_hook)
{
	HostHolder holder;
	VwPayloadHost host = MakeHost(&gCallbackTarget);
	host.runBundledScript = &FakeRunScript;
	CHECK_EQ(holder.adopt(&host), static_cast<int>(kVwPayloadOk));
	holder.forget();
	CHECK(!holder.canRunScripts());
}

// ---------------------------------------------------------------------------

TEST_MAIN();
