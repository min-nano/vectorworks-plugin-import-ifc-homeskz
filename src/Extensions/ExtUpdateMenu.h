//
//	Extensions/ExtUpdateMenu.h
//
//	メニューコマンド「アップデータを確認 (みんなの構造設計支援)」（開発版は
//	「…(みんなの構造設計支援Dev)」）。実行すると、新しいビルドが公開されていないかを
//	確認し、あれば入れ替える。
//
//	【何が起きるか】src/Updater.h の CheckForUpdates(UpdateCheckKind::Manual)。
//	実行された以上は結末を必ず表示する——更新をインストールした／既に最新だった／確認
//	できなかった（オフライン等）のいずれか。**何も表示せずに終わらない**ことがこのコマンドの
//	要件である。
//
//	【登録は殻に、処理は…も殻に】本プラグインの決めごとでは実処理は本体側へ置くが、
//	**自動アップデートだけは殻に残る**（CLAUDE.md「殻と本体」の表）。本体を置き換える
//	処理が本体の中にあると、実行中の自分自身をアンロードすることになるため。
//
//	【なぜコマンドにしたか】以前この確認は **Vectorworks の起動時**に自動で実行していた。
//	コンパイル済みプラグインは起動時にしか読み込まれないので、「入れ替えるなら起動時
//	しかない」と考えていたためである。いまはプラグインが**殻と本体**に分かれていて、
//	本体だけの更新なら再起動なしでその場から反映される（src/PayloadAbi.h）。機能追加以外の
//	更新はほぼ本体だけで済むので、**起動のたびに確認する理由が無くなった**——確認したい
//	ときに実行できるコマンドのほうが、起動を待たせないぶん適切である。
//

#pragma once

#include "VectorworksSDK.h"

namespace HomeskzIfcImport
{
	using namespace VWFC::PluginSupport;

	// ------------------------------------------------------------------------
	// メニュー項目を実行したときの処理。
	class CCheckUpdateMenu_EventSink : public VWMenu_EventSink
	{
	public:
		CCheckUpdateMenu_EventSink(IVWUnknown* parent);
		~CCheckUpdateMenu_EventSink() override;

		// 更新の確認 1 回。**文書が開いていなくても実行できる**（menuDef() の Needs は
		// None）——更新に図面は要らないし、「取り込む前に更新しておきたい」ときに
		// 文書を開く必要があるのでは目的に反する。
		void DoInterface() override;
	};

	// ------------------------------------------------------------------------
	// 拡張そのもの（ModuleMain が REGISTER_Extension で登録する）。
	class CExtMenuCheckUpdate : public VWExtensionMenu
	{
		DEFINE_VWMenuExtension;

	public:
		CExtMenuCheckUpdate(CallBackPtr cbp);
		~CExtMenuCheckUpdate() override;
	};
} // namespace HomeskzIfcImport
