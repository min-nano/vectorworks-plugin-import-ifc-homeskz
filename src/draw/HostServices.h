//
//	draw/HostServices.h
//
//	**殻が本体（ペイロード）へ提供する機能。** 境界（src/PayloadAbi.h）で受け取った
//	ものを、本体の中で使いやすい形に包んで 1 か所に置く。殻から受け取ったものは
//	`payload/PayloadMain.cpp` が init のときにここへ預ける。
//
//	【現在の利用箇所】実機テストの一時ファイルの片付けだけ（draw/Feedback.cpp の
//	CleanUpClosedBranches が `vw-update q-pr-state` で PR の状態を問い合わせる）。
//
//	【注意: 受け取ったものは複製して持つ】境界を越えて来たものは受け取った側がその場で
//	複製する——これはこの仕組み全体の決めごとで、破ると実機で Vectorworks ごと落ちる
//	（src/PayloadHostHolder.h）。std::function / std::string で持つのは**本体の中だけ**
//	なので、C++ の型が境界を越えることにはならない。
//
//	【注意: 置き場所は draw/】このヘッダ自身は標準ライブラリしか参照しないが、置き場所は
//	draw/ にしてある——core/ / parse/ から見えてはいけないもの（殻の存在は解析フェーズの
//	知るところではない）だから。
//
//	【なぜ要るか】本体は**同梱スクリプト**（scripts/vw-*.sh / .ps1）にアクセスできない。
//	本体が読み込まれるのは一時ディレクトリへ複製したものなので、自分の在り処からバンドルへは
//	たどり着けないためである（src/PayloadHost.h「必ず複製してから読む」）。だから殻から
//	借りる。
//
//	【経緯】M38 までの利用箇所だった実機フィードバックの投稿（vw-feedback）は M38 で
//	削除した。
//
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 殻から借りた機能の一式。**空でも動作する**——提供されなかった（古い殻・単体テスト）
	// ときは runScript が空になる。
	struct HostServices
	{
		// 同梱スクリプトを 1 本実行して標準出力を受け取る。baseName は拡張子を除いた
		// 名前（"vw-update" など）。起動できなければ false。
		std::function<bool(const std::string& baseName, const std::vector<std::string>& args,
						   std::string& out)>
			runScript;

		// スクリプトを実行できるか。
		bool canRunScripts() const
		{
			return static_cast<bool>(runScript);
		}
	};

	// 預ける（payload/PayloadMain.cpp が init のときに 1 度だけ）。
	void setHostServices(const HostServices& services);

	// 手放す（アンロードする直前。殻から借りたものを保持したままにしない）。
	void clearHostServices();

	// 借りたもの（預けられていなければ空のまま）。
	const HostServices& hostServices();
} // namespace HomeskzIfcImport::draw
