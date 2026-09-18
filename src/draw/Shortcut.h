//
//	draw/Shortcut.h
//
//	**OS のショートカット／エイリアスを実体へ解決する**（M28）。回帰テストの対象フォルダの
//	走査（core/FixtureScan.h）が、辿れないものに出会ったときここへ訊きに来る。
//
//	【なぜ core/ に置かないか】シンボリックリンクとジャンクションは `std::filesystem` が
//	透過するので何も要らないが、**Windows の `.lnk` と macOS の Finder エイリアスは OS の
//	API でしか解けない**——前者は COM（`IShellLink`）、後者は CoreFoundation のブックマーク
//	である。どちらも Linux の CI ではコンパイルもできないので、無 SDK でテストする `core/`
//	には置けない。そこで**解決の口だけを関数として core へ渡す**形にし、実体をこちらへ
//	置いた（`core::ShortcutResolver`）。**辿ったあとの扱い**——フォルダなら降りる・IFC なら
//	拾う・それ以外は飛ばす——は core 側にあり、偽の解決関数で無 SDK テストできる。
//
//	【SDK には依存しない】ここが使うのは OS の API だけで、VectorWorks SDK は要らない。
//	それでも `draw/` に置くのは、**mac と Windows でしかコンパイルされない**もの＝本体
//	（ペイロード）側のプラットフォーム依存コードの置き場所がここだから（CLAUDE.md
//	「殻と本体」の表で本体に入るもの）。
//
//	【出るダイアログは無い】`.lnk` の解決は**UI を出さない**指定で行う（`SLR_NO_UI`）。
//	無人で何十件も回すものの途中で「リンク先が見つかりません」が出ると、そこで止まる。
//

#pragma once

#include <string>

namespace HomeskzIfcImport::draw
{
	// path がショートカット（Windows の `.lnk`）またはエイリアス（macOS）なら、実体の
	// 絶対パス（UTF-8）を target に入れて true。**そうでなければ false**（呼び出し側は
	// 「素のファイルだった」と読む）。解決に失敗したときも false——理由は伝えない
	// （呼び出し側は「辿れなかった」としか言えないし、それで足りる）。
	bool resolveShortcut(const std::string& path, std::string& target);
} // namespace HomeskzIfcImport::draw
