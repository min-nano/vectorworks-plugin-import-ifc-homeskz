//
//	draw/ImportCommand.h
//
//	**本番の取り込みコマンド**（ファイル選択 → 設定 → 解析 → 描画 → 結果ダイアログ）。
//
//	【なぜメニュー拡張から出してあるか】メニュー拡張（Extensions/ExtMenu）は**殻**——
//	Vectorworks が起動時に読み込むモジュール——に残り、そこに書けるのは「登録」だけである。
//	実処理をこちら（本体＝ペイロード）へ出しておくと、**Vectorworks を再起動せずに
//	入れ替えられる**（src/PayloadAbi.h）。殻の DoInterface はこの関数を C の ABI 越しに
//	呼ぶだけの 1 行になる。
//
//	【往復のことを知らない】M25 で**実機フィードバックの往復をこのコマンドから追い出した**。
//	往復（記憶した条件で取り込み直して PR へ投稿する）は dev だけの実機テストのコマンドが
//	受け持ち（draw/Feedback.h の runTestRound）、絵を作るところだけを両者が共有する
//	（draw/ImportRun.h）。おかげでこのコマンドには
//
//	  * 記憶を読んでファイル選択を飛ばす分岐
//	  * 「次は更新を尋ねずに入れてよいか」を意味する戻り値
//	  * 投稿できたら結果ダイアログを出さない分岐
//
//	が 1 つも無い——**安定版と開発版でここは同じ道を通る**（docs/DEV-NOTES.md M25）。
//
//	例外は runImportRound（draw/ImportRun.h）が受け止める（境界を越えさせない。
//	src/payload/PayloadMain.cpp も最後の砦として受けるが、ユーザーへ「何が起きたか」を
//	出せるのはこちら側だけ）。
//

#pragma once

namespace HomeskzIfcImport::draw
{
	// メニューコマンドが選ばれたときに走る**1 周ぶん**。キャンセルは静かに何もせず返る。
	void runImportCommand();
} // namespace HomeskzIfcImport::draw
