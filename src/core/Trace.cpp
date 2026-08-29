//
//	core/Trace.cpp
//
//	診断ログ（core/Trace.h）の実装。状態は関数ローカル static に 1 つだけ持つ。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "core/Trace.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

namespace HomeskzIfcImport::core::trace
{
	namespace
	{
		struct State
		{
			std::ofstream out;
			std::chrono::steady_clock::time_point start;
			std::string path;
			// 書いた本文の控え。完了ダイアログのログ欄がこれを見せる（Trace.h
			// 「なぜ本文を持つか」）。ファイルを開けなかったときでも溜める。
			std::string text;
			bool open = false; // ファイルの有無に関わらず「セッションが開いている」か
		};

		// 名前空間スコープの変数にすると静的初期化順序に依存するので、関数ローカル
		// static で持つ（Extensions/ExtMenu の menuDef() と同じ理由）。
		State& state()
		{
			static State instance;
			return instance;
		}

		// 環境変数を 1 つ読む（未設定・空文字は「無い」扱い）。
		//
		// **getenv を使うのはここだけ。** MSVC は C4996（"_dupenv_s を使え"）を出し、
		// 無 SDK ライブラリは /W4 /WX で警告をエラーにするので、抑止をこの 1 か所へ
		// 閉じ込める。危険とされるのは「返った文字列を持ち回る／環境を書き換える」
		// 使い方で、ここは**その場で読んで即コピーする**だけなので問題にならない
		// （インポートはメインスレッド 1 本で走り、環境変数を書き換えもしない）。
		const char* envOrNull(const char* name)
		{
#if defined(_MSC_VER)
#	pragma warning(push)
#	pragma warning(disable : 4996)
#endif
			// NOLINTNEXTLINE(concurrency-mt-unsafe): インポートはメインスレッド 1 本で走る
			const char* value = std::getenv(name);
#if defined(_MSC_VER)
#	pragma warning(pop)
#endif
			if (value == nullptr || *value == '\0')
				return nullptr;
			return value;
		}
	} // namespace

	bool open(const std::string& path)
	{
		State& s = state();
		if (s.out.is_open())
			s.out.close();
		s.path.clear();
		// **本文は必ず溜め始める。** ファイルを開けなくてもログ自体は成り立ち（完了
		// ダイアログのログ欄はメモリの本文を見せる）、そこで諦めると「書けない環境では
		// 何も分からない」に逆戻りする。
		s.text.clear();
		s.start = std::chrono::steady_clock::now();
		s.open = true;
		s.out.open(path, std::ios::out | std::ios::trunc);
		if (!s.out.is_open())
			return false; // 書けない場所でも**インポートは続ける**（診断は付随機能）
		s.path = path;
		return true;
	}

	bool isOpen()
	{
		return state().open;
	}

	const std::string& path()
	{
		return state().path;
	}

	namespace
	{
		// 本文へ 1 ブロック足し、開いていればファイルへも書いて**即フラッシュする**。
		// **1 行ごとにフラッシュする。** 落ちたときにバッファの中身は残らないので、
		// これをしないと肝心の最終行（＝原因箇所の直前）が消える。
		void emit(const std::string& block)
		{
			State& s = state();
			if (!s.open)
				return;
			s.text += block;
			s.text += "\n";
			if (!s.out.is_open())
				return;
			s.out << block << "\n";
			s.out.flush();
		}
	} // namespace

	void log(const std::string& message)
	{
		emit("[" + std::to_string(elapsedMs()) + " ms] " + message);
	}

	void note(const std::string& text)
	{
		emit(text);
	}

	const std::string& text()
	{
		return state().text;
	}

	long long elapsedMs()
	{
		const State& s = state();
		if (!s.open)
			return 0;
		return std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::steady_clock::now() - s.start)
			.count();
	}

	void close()
	{
		State& s = state();
		if (s.out.is_open())
			s.out.close();
		s.open = false;
		// **パスと本文は残す**（閉じた後に「診断ログはここ」と案内し、完了ダイアログの
		// ログ欄に中身を出すため。Trace.h の path() / text()）。
	}

	std::string envValue(const char* name)
	{
		const char* value = envOrNull(name);
		return (value != nullptr) ? std::string(value) : std::string();
	}

	bool envFlag(const char* name)
	{
		return envOrNull(name) != nullptr;
	}

	std::string localTimestamp()
	{
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		// **スレッド安全版の綴りが処理系で違う**（MSVC は localtime_s、POSIX は
		// localtime_r）。素の localtime は MSVC が C4996 を出し、無 SDK ライブラリは
		// 警告をエラー扱いにするので、場合分けをここへ閉じ込める（Trace.h）。
#if defined(_MSC_VER)
		if (localtime_s(&local, &now) != 0)
			return {};
#else
		if (localtime_r(&now, &local) == nullptr)
			return {};
#endif
		std::array<char, 32> buffer{};
		// strftime は書けなければ 0 を返す（そのときは空文字＝見出しに時刻を出さない）。
		const std::size_t written =
			std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S", &local);
		if (written == 0)
			return {};
		return {buffer.data(), written};
	}

	std::string defaultLogPath(const std::string& fileName)
	{
		const char* dir = envOrNull("TMPDIR");
		if (dir == nullptr)
			dir = envOrNull("TEMP");
		if (dir == nullptr)
			dir = envOrNull("TMP");

		std::string directory = (dir != nullptr) ? std::string(dir) : std::string("/tmp");
		// 末尾の区切りは 1 つに正規化する（TMPDIR は末尾に "/" が付くことがある）。
		while (!directory.empty() && (directory.back() == '/' || directory.back() == '\\'))
			directory.pop_back();
		return directory + "/" + fileName;
	}
} // namespace HomeskzIfcImport::core::trace
