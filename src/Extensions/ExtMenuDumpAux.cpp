//
//	ExtMenuDumpAux.cpp
//
//	dev ビルド専用の調査コマンドの実装（ねらいと使い方は ExtMenuDumpAux.h 冒頭）。
//
//	【何を見ているか】VectorScript から見える範囲では UI の噛み合わせと本プラグインの
//	削り取りが同一だったので、SDK でしか読めない**補助オブジェクトのデータタグ**
//	（`GetDataTag`。OSType＝4 文字）まで降りて差を探す。データオブジェクトは
//	`NewDataObject(attachTo, tag, size)` で作れるので、**差がデータタグであれば再現できる**
//	見込みがある（型そのものが違うなら SDK からは作れない、という切り分けにもなる）。
//
//	【出力先】デスクトップの vw_aux_dump.txt。SDK のファイル保存ダイアログを使わないのは、
//	これが一時的な調査用で、環境変数から素直に組み立てるほうが短く済むため。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenuDumpAux.h"

#include "VWFC/VWObjects/VWGroupObj.h"
#include "VWFC/VWObjects/VWObject.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// メニュー定義（文字列は dev の .vwr にだけ置く）。文書がアクティブなときだけ有効。
		const SMenuDef& menuDef()
		{
			static const SMenuDef def = {/*Needs*/ EMenuEnableFlags::DocIsActive,
										 /*NeedsNot*/ EMenuEnableFlags::None,
										 /*Title*/ {PLUGIN_VWR_ID, "dumpTitle"},
										 /*Category*/ {PLUGIN_VWR_ID, "category"},
										 /*HelpText*/ {PLUGIN_VWR_ID, "dumpHelp"},
										 /*VersionCreated*/ 31,
										 /*VersionModified*/ 0,
										 /*VersionRetired*/ 0,
										 /*OverrideHelpID*/ ""};
			return def;
		}

		// OSType（4 バイトのタグ）を "abcd (0x61626364)" の形へ。印字できないバイトは '.'。
		std::string TagText(OSType tag)
		{
			std::string text;
			for (int shift = 24; shift >= 0; shift -= 8)
			{
				const char byte = static_cast<char>((tag >> shift) & 0xFF);
				text += (byte >= 0x20 && byte < 0x7F) ? byte : '.';
			}
			char hex[16] = {};
			std::snprintf(hex, sizeof(hex), " (0x%08X)", static_cast<unsigned>(tag));
			return text + hex;
		}

		// 1 オブジェクトの見出し行（型番号とデータタグ）。
		std::string ObjectLine(MCObjectHandle object, const std::string& label)
		{
			if (object == nil)
				return label + ": (nil)\n";
			return label + ": type=" + std::to_string(gSDK->GetObjectTypeN(object)) +
				   " dataTag=" + TagText(gSDK->GetDataTag(object)) + "\n";
		}

		// 補助オブジェクト（aux list）を順に書き出す。**ここが本命**——UI で噛み合わせた
		// 押し出しにだけ付く補助オブジェクトの型とデータタグが分かる。
		std::string DumpAux(MCObjectHandle object, const std::string& indent)
		{
			std::string text;
			MCObjectHandle aux = gSDK->FirstAuxObject(object);
			int index = 0;
			while (aux != nil)
			{
				text += indent + ObjectLine(aux, "aux[" + std::to_string(index) + "]");
				++index;
				aux = VWObject(aux).GetNextObject();
			}
			if (index == 0)
				text += indent + "(補助オブジェクトなし)\n";
			return text;
		}

		// 1 つの選択図形をダンプする（本体 → 補助 → プロファイル群 → 群の各要素と補助）。
		std::string DumpObject(MCObjectHandle object, int index)
		{
			std::string text =
				"================ 選択 " + std::to_string(index) + " ================\n";
			text += ObjectLine(object, "object");
			text += DumpAux(object, "  ");

			const MCObjectHandle group = gSDK->GetCustomObjectProfileGroup(object);
			text += "--- プロファイル群 ---\n";
			text += ObjectLine(group, "profileGroup");
			if (group != nil)
			{
				MCObjectHandle member = VWGroupObj(group).GetFirstMemberObject();
				int memberIndex = 0;
				while (member != nil)
				{
					text += ObjectLine(member, "  member[" + std::to_string(memberIndex) + "]");
					text += DumpAux(member, "    ");
					++memberIndex;
					member = VWObject(member).GetNextObject();
				}
				if (memberIndex == 0)
					text += "  (中身なし)\n";
			}
			return text + "\n";
		}

		// デスクトップの vw_aux_dump.txt のパス（環境変数から組み立てる）。取れなければ空。
		std::string DumpFilePath()
		{
			const char* home = std::getenv("HOME");
			if (home == nullptr)
				home = std::getenv("USERPROFILE"); // Windows
			if (home == nullptr)
				return {};
			return std::string(home) + "/Desktop/vw_aux_dump.txt";
		}
	} // namespace
} // namespace HomeskzIfcImport

// ---------------------------------------------------------------------------
// 拡張の実体。**dev ビルドでしか登録されない**ので UUID は 1 つだけ持つ。
// UUID: 9f3c7d41-5b28-4a16-9c0e-7d5a2f8b6431  (dev only)
// NOLINTBEGIN(misc-const-correctness)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuDumpAux,
	/*Event sink*/ CDumpAuxMenu_EventSink,
	/*Universal name*/ "CExtMenuDumpAux_HomeskzIfcImportDev",
	/*Version*/ 1,
	/*UUID*/ 0x9f3c7d41, 0x5b28, 0x4a16, 0x9c, 0x0e, 0x7d, 0x5a, 0x2f, 0x8b, 0x64, 0x31);
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuDumpAux::CExtMenuDumpAux(CallBackPtr cbp) : VWExtensionMenu(cbp, menuDef()) {}

CExtMenuDumpAux::~CExtMenuDumpAux() = default;

// ---------------------------------------------------------------------------
CDumpAuxMenu_EventSink::CDumpAuxMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent) {}

CDumpAuxMenu_EventSink::~CDumpAuxMenu_EventSink() = default;

void CDumpAuxMenu_EventSink::DoInterface()
{
	// 選択図形を先頭から辿る（FirstSelectedObject 以降をレイヤの並び順に見て、選択中の
	// ものだけを拾う）。1 つずつ実行してもよいが、比べたい図形をまとめて選べるようにする。
	MCObjectHandle object = gSDK->FirstSelectedObject();
	if (object == nil)
	{
		gSDK->AlertInform("図形を選択してから実行してください。", "", true);
		return;
	}

	std::string text;
	int index = 0;
	while (object != nil)
	{
		if (gSDK->IsSelected(object))
		{
			text += HomeskzIfcImport::DumpObject(object, index);
			++index;
		}
		object = VWObject(object).GetNextObject();
	}

	const std::string path = HomeskzIfcImport::DumpFilePath();
	if (path.empty())
	{
		gSDK->AlertInform("出力先（ホームディレクトリ）が分かりませんでした。", "", true);
		return;
	}

	std::ofstream file(path);
	if (!file)
	{
		gSDK->AlertInform("ダンプを書き出せませんでした。", path.c_str(), false);
		return;
	}
	file << text;
	file.close();

	gSDK->AlertInform((std::to_string(index) + " 個の図形をダンプしました。").c_str(), path.c_str(),
					  false);
}
