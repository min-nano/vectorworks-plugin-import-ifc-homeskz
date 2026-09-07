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
	// 戻り値は「**次にこのコマンドが走るときは、更新を尋ねずに入れてよいか**」。実機
	// フィードバックの往復（draw/Feedback.h）で投稿できたときだけ true になる——往復の
	// 最中にいる人へ、周ごとに「新しいビルドがあります。インストールしますか？」を出さない
	// ためのもので、判断できるのは記憶を持っているこちら側だけである。
	//
	// 2 周目以降はファイル選択も設定ダイアログも出ない（1 周目の選択を
	// core::FeedbackSession が覚えている）。
	bool runImportCommand();
} // namespace HomeskzIfcImport::draw
