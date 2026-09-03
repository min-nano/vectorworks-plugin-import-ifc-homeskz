//
//	draw/ImportCommand.h
//
//	**取り込みコマンドの本体**（ファイル選択 → 設定 → 解析 → 描画 → 結果ダイアログ）。
//
//	【なぜメニュー拡張から出してあるか】メニュー拡張（Extensions/ExtMenu）は**殻**——
//	Vectorworks が起動時に読み込むモジュール——に残り、そこに書けるのは「登録」だけである。
//	実処理をこちら（本体＝ペイロード）へ出しておくと、**Vectorworks を再起動せずに
//	入れ替えられる**（src/PayloadAbi.h）。殻の DoInterface はこの関数を C の ABI 越しに
//	呼ぶだけの 1 行になる。
//
//	例外はこの関数の中で受け止める（境界を越えさせない。src/payload/PayloadMain.cpp も
//	最後の砦として受けるが、ユーザーへ「何が起きたか」を出せるのはここだけ）。
//

#pragma once

#include <string>

namespace HomeskzIfcImport
{
	namespace draw
	{
		// メニューコマンドが選ばれたときに走るもの。キャンセルは静かに何もせず返る。
		void runImportCommand();
	} // namespace draw
} // namespace HomeskzIfcImport
