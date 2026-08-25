//
//	core/Trace.h
//
//	クラッシュ診断ログ。フェーズの区切りを 1 行ずつ、
//	**書くたびに必ずフラッシュして**ファイルへ残す。ネイティブプラグインは落ちるときに
//	VectorWorks ごと落ちる——後から何も残らないので、「ログの最終行の直後」が原因箇所になる形に
//	しておくことが唯一の手掛かりになる（docs/DEV-NOTES.md M15「core/Trace」）。
//
//	【誰が書くか】呼び出しを各要素へ撒かない。フェーズの見出しはすでに進捗報告
//	（core/Progress の beginPhase）が受け取っているので、**そこ 1 か所から**ログへ流す
//	（解析 17 フェーズ・描画の各要素がそのまま行になる）。インポートの開始・終了・
//	例外だけは入口（Extensions/ExtMenu）が書く。
//
//	【いつ有効か】既定は無効（open していなければ log は何もしない）。dev ビルドでは
//	インポートのたびに開き、stable では環境変数 HOMESKZ_IFC_TRACE があるときだけ開く
//	（判断は SDK 側の入口が持つ。BuildConfig.h を見られるのはあちらだけ）。
//
//	【スレッド】インポートは VectorWorks のメインスレッドから 1 本で走る（メニュー
//	コマンドの実行経路）。同時に 2 か所から書かれることは無いので、排他は持たない
//	（mutex を足すとテスト側に pthread のリンクが要るだけで、守るものが無い）。
//
//	【SDK 非依存】core/ は VectorWorks SDK を一切 include しない（CLAUDE.md「依存の向き」）。
//	出力先の決定も含めて標準ライブラリだけで完結するので、無 SDK で単体テストできる。
//

#pragma once

#include <string>

namespace HomeskzIfcImport::core::trace
{
	// ログを開く（既に開いていれば閉じてから開き直す）。開けたら true。
	// 追記ではなく毎回切り詰める——欲しいのは「最後に落ちたときの記録」だけで、
	// 過去のインポートの行が混ざると最終行の意味が薄れる。
	bool open(const std::string& path);

	// 開いているか（＝log が実際に書くか）。
	bool isOpen();

	// **最後に開いたログのパス**（一度も開けていなければ空）。エラーダイアログで
	// 「どこを見ればよいか」を案内するために要る。
	//
	// **close() しても消えない。** 消してしまうと、閉じた直後に案内しようとしても空になり、
	// 案内のためだけに閉じる前へコピーを取る不自然なコードを呼び出し側に強いる。
	// 「開いているか」は isOpen() が答えるので、この 2 つで役割が分かれる。
	const std::string& path();

	// 1 行書いて**即フラッシュする**。開いていなければ何もしない。行頭には open からの
	// 経過ミリ秒が付く（どのフェーズで時間を使ったかが後から読めるので、進捗バーの
	// 配分の見直しにも使える）。
	void log(const std::string& message);

	// 閉じる（2 回呼んでも安全）。
	void close();

	// 環境変数が立っているか（未設定・空文字は false）。
	//
	// トレースの有効化条件を SDK 側（Extensions/ExtMenu）が判断するのに使う。**ここに
	// 置くのは、`std::getenv` の作法をこのファイル 1 つへ閉じ込めるため**——MSVC は
	// getenv に C4996（"_dupenv_s を使え"）を出し、無 SDK ライブラリは /W4 /WX で
	// 警告をエラー扱いにしているので、抑止をあちこちに書きたくない。
	bool envFlag(const char* name);

	// 既定の出力先を組み立てる。ディレクトリは環境変数 TMPDIR / TEMP / TMP の順に見て、
	// どれも無ければ "/tmp"（macOS は TMPDIR、Windows は TEMP が必ず入っている）。
	// 図面や IFC の隣には置かない——ユーザーのデータのある場所へ勝手に書かないため。
	std::string defaultLogPath(const std::string& fileName);
} // namespace HomeskzIfcImport::core::trace
