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

namespace HomeskzIfcImport::draw
{
	// メニューコマンドが選ばれたときに走る**1 周ぶん**。キャンセルは静かに何もせず返る。
	//
	// 戻り値は「**もう 1 周してほしいか**」。実機フィードバックの往復（draw/Feedback.h）で
	// 修正版のビルドを入れたときだけ true になり、殻はいったん本体を手放してから、この
	// 関数を呼び直す——手放すことで**新しく入った本体がその場で読み直される**
	// （src/PayloadSession.h）。2 周目以降はファイル選択も設定ダイアログも出ない
	// （1 周目の選択を core::FeedbackSession が覚えている）。
	bool runImportCommand();
} // namespace HomeskzIfcImport::draw
