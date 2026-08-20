//
//	CoreTraceTests.cpp
//
//	クラッシュ診断ログ（src/core/Trace）の単体テスト。無 SDK のテストハーネスで走る。
//	出力先は一時ディレクトリ（core::trace::defaultLogPath が返す場所）で、各ケースは
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
	std::string logPath(const std::string& name)
	{
		return trace::defaultLogPath("homeskz-trace-test-" + name + ".log");
	}
} // namespace

TEST(trace_is_disabled_until_opened)
{
	trace::close(); // 直前のケースの状態を持ち越さない
	CHECK(!trace::isOpen());
	CHECK(trace::path().empty());
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
	CHECK(trace::path().empty());

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

TEST(trace_open_failure_is_not_fatal)
{
	// 書けない場所を指されても false を返すだけ（診断は付随機能で、インポートは続く）。
	CHECK(!trace::open("/this/directory/does/not/exist/homeskz.log"));
	CHECK(!trace::isOpen());
	trace::log("落ちない");
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
