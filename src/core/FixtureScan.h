//
//	core/FixtureScan.h
//
//	**回帰テストの対象フォルダを走査する**（M28）。dev だけのメニュー「回帰テストを実行…」
//	（src/draw/Regression.h）が、指定されたフォルダから取り込む IFC を集めるのに使う。
//
//	【なぜ要るか】このリポジトリは公開されているので、**過去の物件の IFC を中に置けない**。
//	一方で退行を捕まえるのに一番効くのは「実際にやった物件を通し直すこと」である。そこで
//	**フォルダの場所だけを利用者に指してもらい、中身はリポジトリの外に置いたまま**回す。
//
//	【リンクを辿る】物件の IFC は普通あちこちに散らばっているので、**1 つのフォルダへ実体を
//	集め直さなくて済む**ようにする——フォルダの中にシンボリックリンクやショートカットを
//	並べれば、それだけで対象になる。辿るものは 4 つ:
//
//	  * シンボリックリンク（mac / Windows）… `std::filesystem` が既定で透過する
//	  * ジャンクション（Windows）………………… 同上
//	  * `.lnk`（Windows のショートカット）…… OS に訊く（下の `ShortcutResolver`）
//	  * Finder のエイリアス（macOS）………… 同上。**拡張子は元のまま**なので、`.ifc` に
//	    見えるファイルがエイリアスであることがある
//
//	後ろ 2 つは OS の API（COM / CoreFoundation）でしか解けない。**ここは SDK にも OS にも
//	依存しない**ので、解き方は呼び出し側から関数で注入してもらう（実装は
//	`src/draw/Shortcut.h`、テストは偽の解決関数を渡す）。
//
//	【決定性】`directory_iterator` の順序は規定されていないので、**フォルダごとに名前で
//	並べてから降り**、最後にもう一度並べ替えて返す（CLAUDE.md「決定性を守る」）。同じ実体を
//	2 つの経路が指していたら 1 つに畳む——リンクを並べる使い方では、実体とリンクの両方が
//	同じフォルダに入ることが普通に起きる。
//
//	【フォルダはどう決まるか】対象フォルダは `IFolderChooserDialog` で選ばせる
//	（`draw/ImportRun` の `chooseFolder`）。**ここはその結果のパスを受け取るだけ**で、
//	選ばせ方は知らない。
//
//	【SDK 非依存】core/ は VectorWorks SDK を include しない（CLAUDE.md「Phase 1」）。
//	ここはファイルシステムを読むだけなので、無 SDK で単体テストできる
//	（tests/CoreFixtureScanTests.cpp）。
//

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// **ショートカット／エイリアスを実体へ解決する口。** 解決できたら true を返し、target に
	// 実体の絶対パス（UTF-8）を入れる。**ショートカットではなかった場合も false**（呼び出し
	// 側は「素のファイルだった」と読む）。OS 依存の実装は draw/Shortcut.h にあり、テストは
	// 偽の解決関数を渡す。空（未設定）でもよく、そのときは何も解決しない＝シンボリック
	// リンクとジャンクションだけが辿られる。
	using ShortcutResolver = std::function<bool(const std::string& path, std::string& target)>;

	// 見つかった IFC 1 件。
	struct FixtureEntry
	{
		// 実体の絶対パス（リンク・ショートカットは解決済み。取り込みにはこちらを渡す）。
		std::string path;
		// 見出し（**辿り始めたファイルの名前**）。基準ファイルの鍵になるので、実体ではなく
		// フォルダに並べた名前を使う——実体を別の場所へ移しても、同じ名前で並べ直せば
		// 基準と繋がったままになる。
		std::string name;
		// ショートカット／エイリアスを解決して辿り着いたか（記録に出すだけ）。
		bool viaShortcut = false;
	};

	// 走査の結果。
	struct FixtureScan
	{
		// 見つかった IFC（**name の昇順・重複なし**）。
		std::vector<FixtureEntry> entries;
		// 辿れなかったもの・畳んだもの・打ち切った理由（1 件 1 行。空なら何も無かった）。
		// **黙って落とさない**——「あるはずの物件が回っていない」に気付けるのはここだけ。
		std::vector<std::string> notes;
	};

	// 1 回の走査で拾う上限。物件フォルダを丸ごと指されても、際限なく取り込み続けない
	// （1 件 1 分以上かかる）。超えた分は notes に断って捨てる。
	inline constexpr std::size_t kMaxFixtureCount = 200;

	// 降りる深さの上限（指定したフォルダ自身が 0）。リンクの輪は実体パスで弾くが、
	// 深さでも歯止めを持つ。
	inline constexpr int kMaxFixtureDepth = 8;

	// フォルダの中のファイルのパスを組む（**区切りを手で書かない**——Windows と POSIX で
	// 綴りが違ううえ、UTF-8 との往復もここ 1 か所に閉じる）。**フォルダ側の末尾に区切りが
	// 付いていても `//` にならない**（`std::filesystem` が畳む）——フォルダ選択ダイアログが
	// 返すパスは末尾に区切りが付く（下記 `scanFixtureFolder`）ので、これが要る。
	std::string folderFilePath(const std::string& directory, const std::string& fileName);

	// フォルダを走査して IFC を集める。**例外を投げない**（読めないところは notes に
	// 断って飛ばす。CLAUDE.md「1 要素の欠損で全体を止めない」）。
	//
	// **末尾に区切りが付いていてもよい。** フォルダ選択ダイアログが返すパスは
	// `…/物件/` の形で末尾に区切りが付く（[SDK リファレンス「ファイル・フォルダを選ばせる
	// ダイアログ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/File%20and%20Folder%20Dialogs.md)）。
	// そのまま渡すと `path.filename()` が空になり、断りの文言から名前が消えるので、
	// **ここで落としてから使う**（呼び出し側に整形を求めない）。
	FixtureScan scanFixtureFolder(const std::string& directory, const ShortcutResolver& resolve);
} // namespace HomeskzIfcImport::core
