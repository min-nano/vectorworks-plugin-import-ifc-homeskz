//
//	PayloadHostHolder.h
//
//	**本体（ペイロード）が、殻から渡された VwPayloadHost を写して持つ入れ物。**
//
//	【なぜ写すのか——ここを落とすと Vectorworks ごと落ちる】殻が渡してくる
//	`const VwPayloadHost*` が殻のどこを指しているか（ローカルか、メンバか）は、本体からは
//	分からない。SDK リファレンス側の実装は当初これをポインタのまま持っていて、殻が
//	`load()` の**ローカル**に置いていたため、`load()` から戻った時点でその番地が他所へ
//	使い回され、次に受け口を呼んだ瞬間にスタックのゴミへ分岐して落ちた
//	（[Findings「プラグインモジュールの読み込みと入れ替え」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Plug-in%20Modules.md)
//	の「殻の記憶域を本体に持たせると落ちる」）。**コンパイルもリンクも CI も通る**——
//	実機でしか出ない壊れ方なので、規約として両側に置く。
//
//	だから**境界を越えて来たものは、その場で写す**。殻の側も降ろすまで生かしてはいるが、
//	それは二重の歯止めであって、片方だけでは足りない——殻と本体は別々に配られるので、
//	**古い相手と組んでも壊れない**ことが要る（それがホットリロードの目的そのもの）。
//
//	【SDK にもプラットフォームにも依存しない】だから tests/ から単体で確かめられる
//	（tests/PayloadHostHolderTests.cpp）。写しているかどうかは、渡した記憶域を後から
//	塗り潰しても中身が生きていることで確かめる。
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
		// 受け取って**写す**。版と大きさが合わなければ写さずに理由を返す
		// （戻り値は VwPayloadStatus。0 が成功）。
		int adopt(const VwPayloadHost* host);

		// 手放す（降ろす直前・初期化に失敗したとき）。
		void forget()
		{
			fHost = VwPayloadHost{};
			fShellId.clear();
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

		// **いま動いている殻の ID**（貸されていなければ空）。文字列は adopt のときに
		// 写してある——ポインタのまま持たない、というこのファイルの決めごとどおり。
		const std::string& shellId() const
		{
			return fShellId;
		}

		// 同梱スクリプトを走らせられるか（古い殻は貸してくれない）。
		bool canRunScripts() const
		{
			return fValid && fHost.runBundledScript != nullptr;
		}

		// 同梱スクリプトを 1 本走らせて標準出力を受け取る。**返ってきた文字列は
		// その場で写す**（殻が所有し、次の呼び出しまでしか生きていない。PayloadAbi.h）。
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
				out = result; // ← ここで写す
			return true;
		}

	private:
		VwPayloadHost fHost{};
		std::string fShellId; // 殻の ID の**写し**（相手の記憶域を持たない）
		bool fValid = false;
	};

	inline int HostHolder::adopt(const VwPayloadHost* host)
	{
		this->forget();
		if (host == nullptr)
			return kVwPayloadErrHost;
		// 版と大きさの二重の歯止め（殻と本体は別々にビルドされ、別々に配られる）。
		if (host->abiVersion != VW_PAYLOAD_ABI_VERSION)
			return kVwPayloadErrAbi;
		if (host->size < sizeof(VwPayloadHost))
			return kVwPayloadErrAbi;
		if (host->callbacks == nullptr)
			return kVwPayloadErrHost;

		// 大きさは確かめてあるので、**こちらが知っている分だけ**写せばよい（殻の
		// ほうが新しく、後ろに知らない項目が付いていても構わない）。
		fHost = *host;
		fHost.size = static_cast<unsigned int>(sizeof(VwPayloadHost));
		// **文字列はここで写す。** 構造体を写しただけでは中の const char* は相手の
		// 記憶域を指したままで、このファイルが避けようとしている当のものになる。
		fShellId = (host->shellId != nullptr) ? host->shellId : "";
		fValid = true;
		return kVwPayloadOk;
	}
} // namespace HomeskzIfcImport::payload
