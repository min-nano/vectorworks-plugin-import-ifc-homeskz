//
//	PayloadPathTests.cpp
//
//	殻が本体（ペイロード）を探すときのパスの組み立て（src/PayloadHost.h の
//	`payloadpath` 名前空間）。**純粋な文字列操作なので SDK も実機も要らない**——
//	自動アップデートの UpdaterParse と同じ作法で、ここだけ切り出して単体で確かめる。
//
//	ここが狂うと症状は「本体が見つからない」だけになり、どこで曲がったのかが分から
//	なくなる（実機でしか出ない）。特に **mac でバンドルの中を指してしまう**のは
//	静かな事故で、署名が壊れて次の起動から読み込めなくなりうる。
//

#include "TestFramework.h"
#include "PayloadHost.h"

#include <string>

using namespace HomeskzIfcImport::payloadpath;

// ---------------------------------------------------------------------------

TEST(file_name_is_the_plugin_name_with_the_payload_extension)
{
	// 拡張子が .vwpayload なのは**Vectorworks にプラグインとして拾わせないため**
	// （.vlb / .vwlibrary だと Plug-Ins の走査に引っかかり、殻と二重に読み込まれる）。
	CHECK_EQ(FileNameFor("HomeskzIfcImport"), "HomeskzIfcImport.vwpayload");
	// stable と dev は名前が違う＝同じ Plug-Ins に同居しても取り違えない。
	CHECK_EQ(FileNameFor("HomeskzIfcImportDev"), "HomeskzIfcImportDev.vwpayload");
}

// ---------------------------------------------------------------------------
// macOS

TEST(mac_payload_sits_next_to_the_bundle_not_inside_it)
{
	// **バンドルの中には置かない。** mac の署名はリソースまで封をするので、
	// Contents/Resources のファイルを差し替えると署名が壊れる（src/PayloadHost.h）。
	const std::string binary = "/Users/me/Library/Application Support/Vectorworks/2026/Plug-ins/"
							   "HomeskzIfcImport.vwlibrary/Contents/MacOS/HomeskzIfcImport";
	CHECK_EQ(MacPayloadPathFromBinary(binary, "HomeskzIfcImport.vwpayload"),
			 "/Users/me/Library/Application Support/Vectorworks/2026/Plug-ins/"
			 "HomeskzIfcImport.vwpayload");
}

TEST(mac_payload_path_is_empty_for_an_unexpected_shape)
{
	// マーカーが無い＝バンドルの中から読み込まれていない。**当てずっぽうのパスを
	// 返さない**（呼び出し側は「置き場所を割り出せませんでした」と言える）。
	CHECK_EQ(MacPayloadPathFromBinary("/tmp/HomeskzIfcImport", "x.vwpayload"), "");
	CHECK_EQ(MacPayloadPathFromBinary("", "x.vwpayload"), "");
	// "/Contents/MacOS/" はあるが、その上に親フォルダが無い。
	CHECK_EQ(MacPayloadPathFromBinary("HomeskzIfcImport.vwlibrary/Contents/MacOS/X", "x.vwpayload"),
			 "");
}

// ---------------------------------------------------------------------------
// Windows

TEST(win_payload_sits_next_to_the_module)
{
	const std::string module = "C:\\Users\\me\\AppData\\Roaming\\Nemetschek\\Vectorworks\\2026\\"
							   "Plug-ins\\HomeskzIfcImport.vlb";
	CHECK_EQ(WinPayloadPathFromModule(module, "HomeskzIfcImport.vwpayload"),
			 "C:\\Users\\me\\AppData\\Roaming\\Nemetschek\\Vectorworks\\2026\\"
			 "Plug-ins\\HomeskzIfcImport.vwpayload");
	// 区切りは "/" でも通す（ツール経由のパスは混ざることがある）。
	CHECK_EQ(WinPayloadPathFromModule("C:/plugins/HomeskzIfcImport.vlb", "P.vwpayload"),
			 "C:/plugins/P.vwpayload");
}

TEST(win_payload_path_is_empty_without_a_separator)
{
	CHECK_EQ(WinPayloadPathFromModule("HomeskzIfcImport.vlb", "P.vwpayload"), "");
}

// ---------------------------------------------------------------------------
// 一時ディレクトリへの複製先

TEST(temp_copy_path_differs_per_generation)
{
	// **世代ごとに名前を変える**のが肝。Windows は読み込み中の DLL を置き換えられない
	// ので、同じパスを使い回すと 2 回目が古いまま読まれる（src/PayloadHost.h）。
	const std::string a = TempCopyPath("/tmp", "1", "P.vwpayload", '/');
	const std::string b = TempCopyPath("/tmp", "2", "P.vwpayload", '/');
	CHECK_EQ(a, "/tmp/vwpayload-1-P.vwpayload");
	CHECK_EQ(b, "/tmp/vwpayload-2-P.vwpayload");
	CHECK(a != b);
}

TEST(temp_copy_path_does_not_double_the_separator)
{
	// temp_directory_path() は末尾に区切りを付けて返す処理系がある。
	CHECK_EQ(TempCopyPath("/tmp/", "1", "P.vwpayload", '/'), "/tmp/vwpayload-1-P.vwpayload");
	CHECK_EQ(TempCopyPath("C:\\Temp\\", "1", "P.vwpayload", '\\'),
			 "C:\\Temp\\vwpayload-1-P.vwpayload");
}

// ---------------------------------------------------------------------------

TEST_MAIN();
