//
//	draw/HostServices.h
//
//	**殻が本体（ペイロード）へ貸してくれる道具。** 境界（src/PayloadAbi.h）で受け取った
//	ものを、本体の中で使いやすい形に包んで 1 か所に置く。
//
//	【なぜ要るか】本体は 2 つのものに手が届かない:
//	  * **同梱スクリプト**（vw-update.sh / vw-feedback.sh）。本体が読み込まれるのは
//	    一時ディレクトリへ写した複製なので、自分の在り処からバンドルへはたどり着けない
//	    （src/PayloadHost.h「必ず複製してから読む」）。
//	  * **殻の ID**（VW_SHELL_ID）。殻にしかコンパイルされていない。
//	どちらも殻から借りる。借りたものは `payload/PayloadMain.cpp` が init のときにここへ
//	預け、`draw/Feedback` が使う。
//
//	【なぜ写して持つか】境界を越えて来たものは受け取った側がその場で写す——これは
//	この仕組み全体の決めごとで、破ると実機で Vectorworks ごと落ちる
//	（src/PayloadHostHolder.h）。std::function / std::string で持つのは**本体の中だけ**
//	なので、C++ の型が境界を越えることにはならない。
//
//	【SDK 依存ではない】このヘッダ自身は標準ライブラリしか見ないが、置き場所は draw/ に
//	してある——使うのが描画側（draw/Feedback）だけで、core/ / parse/ から見えてはいけない
//	もの（殻の存在は解析フェーズの知るところではない）だから。
//

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 借りた道具ひとそろい。**空でも動く**——貸してもらえなかった（古い殻・単体テスト）
	// ときは runScript が空で、フィードバックの機能だけが静かに使えなくなる。
	struct HostServices
	{
		// いま動いている殻の ID（アップデート後に再起動が要るかの判定に使う）。
		std::string shellId;

		// 同梱スクリプトを 1 本走らせて標準出力を受け取る。baseName は拡張子を除いた
		// 名前（"vw-update" / "vw-feedback"）。起動できなければ false。
		std::function<bool(const std::string& baseName, const std::vector<std::string>& args,
						   std::string& out)>
			runScript;

		// スクリプトを走らせられるか（＝フィードバックの往復が使えるか）。
		bool canRunScripts() const
		{
			return static_cast<bool>(runScript);
		}
	};

	// 預ける（payload/PayloadMain.cpp が init のときに 1 度だけ）。
	void setHostServices(const HostServices& services);

	// 手放す（降ろす直前。殻から借りたものを持ったままにしない）。
	void clearHostServices();

	// 借りたもの（預けられていなければ空のまま）。
	const HostServices& hostServices();
} // namespace HomeskzIfcImport::draw
