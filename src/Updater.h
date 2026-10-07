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
//	    実行された以上、結末は必ず伝える——最新だったのか、オフラインで確認できなかった
//	    のかが分からないのでは、コマンドとして成立しない。
//	  * 取り込みコマンドの先頭（src/Extensions/ExtMenu.cpp）… UpdateCheckKind::Silent。
//	    更新があるときだけ尋ね、取得に失敗したら何も表示せずに取り込みへ進む。
//
//	**PIO は実行のたびに確認できない**ので、PIO の側は手動の確認か、取り込みに付随する
//	更新で入れ替わるのを待つ。
//
//	起動時の確認をやめられたのは、プラグインが**殻と本体**に分かれていて、
//	本体だけの更新なら再起動が要らなくなったため（src/PayloadAbi.h）。機能追加以外の
//	更新はその場で反映されるので、「起動のたびに確認する」必要が無くなった。
//
//	【再起動】殻まで変わった更新は、Vectorworks を再起動しないと反映されない。その
//	再起動は **SDK の CloseAllFilesAndQuitVectorworks(bAskForSave, bRestart)** に要求する
//	（Updater.cpp の CVectorworksUpdaterHost::Restart）。開いている文書の保存確認も
//	Vectorworks 自身が通常どおり行う。
//
//	以前これが使えず、終了と再起動を分離したヘルパープロセスへ任せていたのは、
//	**確認がプラグインの読み込み中（スプラッシュ表示中）に実行されていた**ためで、その頃は
//	SDK に終了を要求すると「サポートファイルの読み込みに失敗しました」で異常終了した
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
	// e.g. "vw-update"; macOS adds ".sh", Windows ".ps1") and capture
	// its stdout. Returns false if the script could not be located or started.
	//
	// **本体（ペイロード）へ貸し出すためにこの関数がある。** 本体は自分の場所から
	// 同梱物の場所を特定できない（読み込まれるのは一時ディレクトリへ複製したもので、
	// dladdr / GetModuleFileName はバンドルの外を指す）ので、殻の機能を借りる
	// ——境界の関数ポインタ VwPayloadHost::runBundledScript の実体がこれである。
	bool RunBundledScriptNamed(const std::string& baseName, const std::vector<std::string>& args,
							   std::string& out);

	// このビルドのチャンネル（stable / dev）に応じた更新の確認を 1 回行う。呼ぶたびに
	// 実行する（起動時に 1 度きりだった頃の「済んだか」のフラグは持たない——手で実行した
	// コマンドが 2 度目に何も表示しないのでは困る）。
	//
	// 例外は投げない。呼び出し側（SDK のコールバック）へ漏らさないための最後の防御は
	// それぞれの入口が持つ。
	// 戻り値は「この実行のまま取り込みへ進んでよいか」（UpdaterHost.h）。
	bool CheckForUpdates(UpdateCheckKind kind);

	// **MCP の `vw_update` が要求する、確認も通知もしない開発版の入れ替え**（M38。
	// UpdaterHost.h の RemoteDevUpdateWith を実際の host と殻の素性で呼び出す）。安定版では
	// 常に CheckFailed——MCP ブリッジは開発版にしか無く、安定版を尋ねずに入れ替える対象に
	// しない。
	RemoteUpdateResult RemoteDevUpdate(const std::string& wantedBranch);

	// **MCP の `vw_restart` が要求する再起動**（M38）。手で実行した確認の「再起動」と同じ
	// 経路（SDK の CloseAllFilesAndQuitVectorworks。開いている文書の保存確認は通常どおり出る）。
	// 要求すらできなかったときだけ false。
	bool RequestRestart();
} // namespace HomeskzIfcImport
