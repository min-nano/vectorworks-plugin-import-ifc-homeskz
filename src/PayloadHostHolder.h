//
//	PayloadHostHolder.h
//
//	**本体（ペイロード）が、殻から渡された VwPayloadHost を複製して保持する入れ物。**
//
//	**境界を越えて来たものは、その場で複製する**。殻の側もアンロードするまで保持しては
//	いるが、それは二重の歯止めであって、片方だけでは足りない——殻と本体は別々に配布される
//	ので、**古い相手と組んでも壊れない**ことが要る（それがホットリロードの目的そのもの）。
//
//	【なぜ複製するのか——これを怠ると Vectorworks ごと異常終了する】殻が渡してくる
//	`const VwPayloadHost*` が殻のどこを指しているか（ローカルか、メンバか）は、本体からは
//	分からない。SDK リファレンス側の実装は当初これをポインタのまま持っていて、殻が
//	`load()` の**ローカル**に置いていたため、`load()` から戻った時点でその番地が他の用途へ
//	使い回され、次に関数ポインタを呼んだ瞬間にスタック上の不定値へ分岐して異常終了した
//	（[Findings「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)
//	の「殻の記憶域を本体に持たせると落ちる」）。**コンパイルもリンクも CI も通る**——
//	実機でしか出ない不具合なので、規約として両側に置く。
//
//	【SDK にもプラットフォームにも依存しない】だから tests/ から単体で確かめられる
//	（tests/PayloadHostHolderTests.cpp）。複製しているかどうかは、渡した記憶域を後から
//	上書きしても中身が保たれていることで確かめる。
//

#pragma once

#include "PayloadAbi.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::payload
{
	class HostHolder
	{
	public:
		// 受け取って**複製する**。版と大きさが合わなければ複製せずに理由を返す
		// （戻り値は VwPayloadStatus。0 が成功）。
		int adopt(const VwPayloadHost* host);

		// 手放す（アンロードする直前・初期化に失敗したとき）。
		void forget()
		{
			fHost = VwPayloadHost{};
			fValid = false;
		}

		bool valid() const
		{
			return fValid;
		}

		// SDK の CallBackPtr（GS_InitializeVCOM へ渡すもの）。
		void* callbacks() const
		{
			return fValid ? fHost.callbacks : nullptr;
		}

		// 同梱スクリプトを実行できるか（古い殻はこの機能を提供しない）。
		bool canRunScripts() const
		{
			return fValid && fHost.runBundledScript != nullptr;
		}

		// 同梱スクリプトを 1 本実行して標準出力を受け取る。**返ってきた文字列は
		// その場で複製する**（殻が所有し、次の呼び出しまでしか有効でない。PayloadAbi.h）。
		bool runScript(const std::string& baseName, const std::vector<std::string>& args,
					   std::string& out) const
		{
			out.clear();
			if (!this->canRunScripts())
				return false;

			// C の配列へ並べ替える（境界を越えるのは const char* の列だけ）。
			std::vector<const char*> raw;
			raw.reserve(args.size());
			for (const std::string& arg : args)
				raw.push_back(arg.c_str());

			const char* result = nullptr;
			const int status =
				fHost.runBundledScript(baseName.c_str(), raw.empty() ? nullptr : raw.data(),
									   static_cast<unsigned int>(raw.size()), &result);
			if (status != kVwPayloadOk)
				return false;
			if (result != nullptr)
				out = result; // ← ここで複製する
			return true;
		}

	private:
		VwPayloadHost fHost{};
		bool fValid = false;
	};

	inline int HostHolder::adopt(const VwPayloadHost* host)
	{
		this->forget();
		if (host == nullptr)
			return kVwPayloadErrHost;
		// 版と大きさの二重の歯止め（殻と本体は別々にビルドされ、別々に配布される）。
		if (host->abiVersion != VW_PAYLOAD_ABI_VERSION)
			return kVwPayloadErrAbi;
		if (host->size < sizeof(VwPayloadHost))
			return kVwPayloadErrAbi;
		if (host->callbacks == nullptr)
			return kVwPayloadErrHost;

		// 大きさは確かめてあるので、**こちらが知っている分だけ**複製すればよい（殻の
		// ほうが新しく、後ろに知らない項目が付いていても構わない）。
		fHost = *host;
		fHost.size = static_cast<unsigned int>(sizeof(VwPayloadHost));
		fValid = true;
		return kVwPayloadOk;
	}
} // namespace HomeskzIfcImport::payload
