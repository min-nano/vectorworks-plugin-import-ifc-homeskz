//
//	draw/ImportCommand.h
//
//	**本番の取り込みコマンド**（ファイル選択 → 設定 → 解析 → 描画 → 結果ダイアログ）。
//	殻の DoInterface はこの関数を C の ABI 越しに呼ぶだけの 1 行になる。
//
//	【注意: 実機テストのことを知らない】実機テスト（記憶した条件で取り込み直して結果を
//	記録する）は dev だけの実機テストのコマンドが受け持ち（draw/Feedback.h の
//	runTestRound）、描画結果を作るところだけを両者が共有する（draw/ImportRun.h）。
//	このコマンドには
//
//	  * 記憶を読んでファイル選択を省く分岐
//	  * 「次は更新を尋ねずに入れてよいか」を意味する戻り値
//	  * 投稿できたら結果ダイアログを出さない分岐
//
//	が 1 つも無い——**安定版と開発版でここは同じ経路を通る**。M25 で**実機フィードバックの
//	往復をこのコマンドから分離した**（docs/DEV-NOTES.md M25）。
//
//	【注意: 例外】例外は runImportRound（draw/ImportRun.h）が受け止める（境界を越えさせない。
//	src/payload/PayloadMain.cpp も最後の砦として受けるが、ユーザーへ「何が起きたか」を
//	出せるのはこちら側だけ）。
//
//	【なぜメニュー拡張から分離してあるか】メニュー拡張（Extensions/ExtMenu）は**殻**——
//	Vectorworks が起動時に読み込むモジュール——に残り、そこに書けるのは「登録」だけである。
//	実処理をこちら（本体＝ペイロード）へ出しておくと、**Vectorworks を再起動せずに
//	入れ替えられる**（src/PayloadAbi.h）。
//
#pragma once

namespace HomeskzIfcImport::draw
{
	// メニューコマンドが選ばれたときに実行する**1 周ぶん**。キャンセルは何もせずに返る。
	void runImportCommand();
} // namespace HomeskzIfcImport::draw
