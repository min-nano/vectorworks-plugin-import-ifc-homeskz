//
//	CoreTraceTests.cpp
//
//	診断ログ（src/core/Trace）の単体テスト。無 SDK のテストハーネスで走る。
//	出力先は CMake が渡すビルドディレクトリ（HOMESKZ_TRACE_TEST_DIR）で、各ケースは
//	書いたファイルを必ず消してから終わる。
//

#include "TestFramework.h"

#include "core/Progress.h"
#include "core/Trace.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace trace = HomeskzIfcImport::core::trace;

namespace
{
	// ログ全文を読み出す（開いていない・書けていなければ空）。
	std::string readAll(const std::string& path)
	{
		std::ifstream in(path);
		std::ostringstream out;
		out << in.rdbuf();
		return out.str();
	}

	// テスト用のログパス（ケースごとに名前を分け、並列実行でぶつからないようにする）。
	//
	// **環境変数から組み立てない。** `defaultLogPath` は TMPDIR / TEMP を読むので、その値を
	// そのままファイルを開く先に使うと CodeQL が「制御されていないデータをパスに使った」
	// （cpp/path-injection）と報告する。テストが書く先は CMake から受け取るビルド
	// ディレクトリで十分——共有の一時ディレクトリを汚さずに済むという実利もある。
	// `defaultLogPath` 自体は下の default_log_path_… が**文字列として**検証する
	// （ファイルは開かないので流れが繋がらない）。
	std::string logPath(const std::string& name)
	{
		return std::string(HOMESKZ_TRACE_TEST_DIR) + "/homeskz-trace-test-" + name + ".log";
	}
} // namespace

TEST(trace_is_disabled_until_opened)
{
	trace::close(); // 直前のケースの状態を持ち越さない
	CHECK(!trace::isOpen());
	// 開いていないときの log は「何もしない」——例外も出さず、どこにも書かない。
	trace::log("この行はどこへも出ない");
	CHECK(!trace::isOpen());
}

TEST(trace_writes_each_line_immediately)
{
	std::string const path = logPath("lines");
	CHECK(trace::open(path));
	CHECK(trace::isOpen());
	CHECK_EQ(trace::path(), path);

	trace::log("解析: 開始");
	// **閉じる前に読める**ことが肝心（落ちてもバッファに残さない＝1 行ごとにフラッシュ）。
	std::string const midway = readAll(path);
	CHECK(midway.find("解析: 開始") != std::string::npos);
	// 行頭に経過ミリ秒が付く。
	CHECK(midway.find("[") == 0);
	CHECK(midway.find(" ms] ") != std::string::npos);

	trace::log("描画: 開始");
	trace::close();
	CHECK(!trace::isOpen());
	// **閉じてもパスは残る**——エラーダイアログが閉じた後に「診断ログはここ」と案内できる
	// ようにするため（残さないと、案内のためだけに閉じる前のコピーが要る）。
	CHECK_EQ(trace::path(), path);

	std::string const text = readAll(path);
	CHECK(text.find("解析: 開始") != std::string::npos);
	CHECK(text.find("描画: 開始") != std::string::npos);
	std::remove(path.c_str());
}

TEST(trace_reopen_truncates)
{
	// 欲しいのは「最後に落ちたときの記録」なので、開き直したら前回の行は残さない。
	std::string const path = logPath("truncate");
	CHECK(trace::open(path));
	trace::log("前回の行");
	CHECK(trace::open(path));
	trace::log("今回の行");
	trace::close();

	std::string const text = readAll(path);
	CHECK(text.find("前回の行") == std::string::npos);
	CHECK(text.find("今回の行") != std::string::npos);
	std::remove(path.c_str());
}

TEST(trace_open_failure_still_records_in_memory)
{
	// 書けない場所を指されても false を返すだけ（診断は付随機能で、インポートは続く）。
	// **それでも本文は溜める**——完了ダイアログのログ欄はメモリの本文を見せるので、
	// 一時ディレクトリへ書けない環境でも「何が起きたか」は読める（core/Trace.h）。
	CHECK(!trace::open("/this/directory/does/not/exist/homeskz.log"));
	CHECK(trace::isOpen()); // セッションは開いている（ファイルに書けているとは限らない）
	CHECK(trace::path().empty()); // 書けなかったので案内できる場所は無い
	trace::log("落ちない");
	CHECK(trace::text().find("落ちない") != std::string::npos);
	trace::close();
}

TEST(trace_note_writes_without_the_elapsed_prefix)
{
	// 見出しブロックや結果の一覧は「その行を書いた時刻」に意味が無いので、経過ミリ秒を
	// 付けずに複数行のまま書く（core/Trace.h の note）。
	std::string const path = logPath("note");
	CHECK(trace::open(path));
	trace::note("=== ホームズ君 IFC インポート ===\n日時: 2026-08-29 14:03:21");
	trace::log("解析: 開始");
	trace::close();

	std::string const text = readAll(path);
	CHECK(text.find("=== ホームズ君 IFC インポート ===\n日時: 2026-08-29 14:03:21\n") !=
		  std::string::npos);
	// note の行には ms が付かない（log の行には付く）。
	CHECK(text.find(" ms] 日時") == std::string::npos);
	CHECK(text.find(" ms] 解析: 開始") != std::string::npos);
	// 書いたものはメモリの本文とも一致する（ダイアログのログ欄が見せるのはこちら）。
	CHECK_EQ(trace::text(), text);
	std::remove(path.c_str());
}

TEST(trace_text_survives_close_and_resets_on_open)
{
	// **閉じても本文は残る**（完了ダイアログは閉じた後にログを見せる）。次の取り込みで
	// 開き直したら前回の本文は消える（ファイルと同じく「今回の記録」だけを持つ）。
	std::string const path = logPath("text");
	CHECK(trace::open(path));
	trace::log("前回の行");
	trace::close();
	CHECK(trace::text().find("前回の行") != std::string::npos);

	CHECK(trace::open(path));
	CHECK(trace::text().find("前回の行") == std::string::npos);
	trace::close();
	std::remove(path.c_str());
}

TEST(env_value_reads_the_environment)
{
	// 立っていない変数は空・false。**getenv を使うのは core/Trace だけ**なので、その
	// 読み取り（未設定・空文字の扱い）はここで固定しておく。
	CHECK(trace::envValue("HOMESKZ_IFC_TRACE_DEFINITELY_NOT_SET").empty());
	CHECK(!trace::envFlag("HOMESKZ_IFC_TRACE_DEFINITELY_NOT_SET"));
	// PATH は mac / Windows / Linux のいずれでも必ず入っている。
	CHECK(!trace::envValue("PATH").empty());
	CHECK(trace::envFlag("PATH"));
}

TEST(local_timestamp_has_the_expected_shape)
{
	// ログの見出しに出す壁時計。値そのものは検証できないので**形**を固定する
	// （"YYYY-MM-DD HH:MM:SS"）。報告の日時とユーザーの記憶を突き合わせる唯一の手掛かり。
	std::string const stamp = trace::localTimestamp();
	CHECK_EQ(stamp.size(), static_cast<std::size_t>(19));
	CHECK_EQ(stamp[4], '-');
	CHECK_EQ(stamp[7], '-');
	CHECK_EQ(stamp[10], ' ');
	CHECK_EQ(stamp[13], ':');
	CHECK_EQ(stamp[16], ':');
}

TEST(default_log_path_uses_temp_dir_and_single_separator)
{
	std::string const path = trace::defaultLogPath("HomeskzIfcImport.log");
	CHECK(path.find("HomeskzIfcImport.log") != std::string::npos);
	// 区切りは 1 つだけ（TMPDIR が末尾に "/" を持っていても "//" にしない）。
	CHECK(path.find("//") == std::string::npos);
	CHECK(path.size() > std::string("HomeskzIfcImport.log").size());
}

TEST(progress_phases_land_in_the_trace)
{
	// **トレースの呼び出しを各要素へ撒かない**設計の要: 進捗のフェーズ見出しが
	// そのままログの行になる（core/Progress の beginPhase が 1 か所で流す）。
	std::string const path = logPath("phases");
	CHECK(trace::open(path));

	HomeskzIfcImport::core::NullProgressReporter progress;
	progress.beginPhase("横架材を描画しています…", 10.0, 196);
	progress.step();
	progress.beginPhase("柱を描画しています…", 10.0, 42);
	trace::close();

	std::string const text = readAll(path);
	CHECK(text.find("横架材を描画しています… (0/196)") != std::string::npos);
	CHECK(text.find("柱を描画しています… (0/42)") != std::string::npos);
	// step() は行にしない（1 件ごとに書くとログが数千行になり、最終行が読めなくなる）。
	CHECK(text.find("(1/196)") == std::string::npos);
	std::remove(path.c_str());
}

TEST_MAIN();
