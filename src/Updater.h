//
//	Updater.h
//
//	Drives the plug-in's self-update using NATIVE Vectorworks dialogs
//	(gSDK->AlertInform / gSDK->AlertQuestion). The network + install mechanics
//	are delegated to a bundled updater script (macOS: vw-update.sh via bash;
//	Windows: vw-update.ps1 via PowerShell) shipped alongside the installed
//	plug-in (it runs non-interactively and prints machine-readable output; see
//	its q-stable / q-dev / do-install modes), while every user-facing dialog is
//	shown by the plug-in itself. So nobody has to open a terminal.
//
//	【いつ確認するか】**起動時には確認しない。** 入口は次の 2 つだけである。
//
//	  * メニューコマンド「アップデータを確認 (みんなの構造設計支援)」
//	    （src/Extensions/ExtUpdateMenu.h）……… UpdateCheckKind::Manual。
//	    押した以上、結末は必ず伝える——最新だったのか、オフラインで確認できなかった
//	    のかが分からないのでは、コマンドとして成立しない。
//	  * 取り込みコマンドの頭（src/Extensions/ExtMenu.cpp）… UpdateCheckKind::Silent。
//	    更新があるときだけ尋ね、取得に失敗したら黙って取り込みへ進む。
//
//	起動時の確認をやめられたのは、プラグインが**殻と本体**に割れていて、
//	本体だけの更新なら再起動が要らなくなったため（src/PayloadAbi.h）。機能追加以外の
//	更新はその場で反映されるので、「起動のたびに問う」必要が無くなった。**PIO は
//	実行のたびに確認できない**ので、こちらは手動の確認か、取り込みのついでの更新に
//	乗って入れ替わるのを待つ。
//
//	【再起動】殻まで変わった更新は、Vectorworks を起動し直さないと反映されない。その
//	再起動は **SDK の CloseAllFilesAndQuitVectorworks(bAskForSave, bRestart)** に頼む
//	（Updater.cpp の CVectorworksUpdaterHost::Restart）。開いている文書の保存確認も
//	Vectorworks 自身が通常どおり行う。
//
//	以前これが使えず、終了と起動し直しを切り離したヘルパープロセスへ任せていたのは、
//	**確認がプラグインの読み込み中（スプラッシュ表示中）に走っていた**ためで、その頃は
//	SDK に終了を頼むと「サポートファイルの読み込みに失敗しました」で落ちた
//	（docs/DEVELOPMENT.md）。確認がメニューコマンドへ移り、Vectorworks が完全に
//	動いている最中に呼ばれるようになったので、その制約は無くなった。
//

#pragma once

#include "UpdaterHost.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport
{
	// Run ONE of the bundled scripts (baseName without its extension —
	// "vw-update" / "vw-feedback"; macOS adds ".sh", Windows ".ps1") and capture
	// its stdout. Returns false if the script could not be located or started.
	//
	// **本体（ペイロード）へ貸し出すためにここに口がある。** 本体は自分の在り処から
	// 同梱物へたどり着けない（読み込まれるのは一時ディレクトリへ写した複製で、
	// dladdr / GetModuleFileName はバンドルの外を指す）ので、殻の道具を借りる
	// ——境界の関数ポインタ VwPayloadHost::runBundledScript の実体がこれである。
	bool RunBundledScriptNamed(const std::string& baseName, const std::vector<std::string>& args,
							   std::string& out);

	// このビルドのチャンネル（stable / dev）に応じた更新の確認を 1 回行う。呼ぶたびに
	// 走る（起動時に 1 度きりだった頃の「済んだか」の見張りは持たない——手で押した
	// コマンドが 2 度目に黙るのでは困る）。
	//
	// 例外は投げない。呼び出し側（SDK のコールバック）へ漏らさないための最後の壁は
	// それぞれの入口が持つ。
	void CheckForUpdates(UpdateCheckKind kind);
} // namespace HomeskzIfcImport
