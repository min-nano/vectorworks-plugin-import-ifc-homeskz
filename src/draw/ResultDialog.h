//
//	draw/ResultDialog.h
//
//	取り込みの結果を見せるダイアログ（完了・エラー共通）。本文は短く保ち、**診断ログを
//	折り畳んだテキスト欄**として同じダイアログに載せる（M19）。
//
//	【なぜ要るか】素の `gSDK->AlertInform` では本文を出すことしかできない。ふだんは
//	「終わったか・問題があったか」だけ読めればよく、不具合を報告するときにはログ全文が要る
//	——この 2 つを 1 枚で満たすには、本文の下に**開閉できる・スクロールする・コピーできる**
//	テキスト欄が要る。ログをファイルから探させると、macOS の一時ディレクトリ
//	（`/var/folders/…/T/`）は当てられないので、実際には誰も貼れない。
//
//	【SDK 依存】実装（draw/ResultDialog.cpp）は PluginPrefix.h（VectorWorks SDK）と
//	VWFC のダイアログ（VWFC::VWUI::VWDialog）を include する。このヘッダは標準ライブラリ
//	しか参照しないので、SDK を持たない翻訳単位からも安全に include できる
//	（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include <string>

namespace HomeskzIfcImport::draw
{
	// 結果ダイアログを出す（モーダル。閉じるまで戻らない）。body は完了・エラーの本文
	// （parse/Summary が組み立てたもの。改行で 1 行ずつ並べる）、log は診断ログの全文
	// （core::trace::text()。空ならログ欄も開閉ボタンも作らない）。
	//
	// **ログは「ログを表示」ボタン（OK と同じ行）で開き、一度開いたら畳まない**
	// （レイアウトの大きさは作るときにしか決まらないため。draw/ResultDialog.cpp 冒頭）。
	// 閉じるまで戻らないのは変わらない——押されたら内部でログ付きの 1 枚を開き直すが、
	// 位置を引き継ぐので、利用者にはその場でログが増えたように見える。
	//
	// 戻り値は**ダイアログを出せたか**。false なら呼び出し側は素のアラート
	// （gSDK->AlertInform）へ落とす——結果を伝えられないまま黙って終わるのが最悪なので、
	// ダイアログを組めなかったときの逃げ道を必ず用意する。
	bool showImportResult(const std::string& title, const std::string& body,
						  const std::string& log);
} // namespace HomeskzIfcImport::draw
