//
//	core/Trace.cpp
//
//	クラッシュ診断ログ（core/Trace.h）の実装。状態は関数ローカル static に 1 つだけ持つ。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "core/Trace.h"

#include <chrono>
#include <cstdlib>
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
		};

		// 名前空間スコープの変数にすると静的初期化順序に依存するので、関数ローカル
		// static で持つ（Extensions/ExtMenu の menuDef() と同じ理由）。
		State& state()
		{
			static State instance;
			return instance;
		}

		// 環境変数を 1 つ読む（未設定・空文字は「無い」扱い）。
		const char* envOrNull(const char* name)
		{
			// NOLINTNEXTLINE(concurrency-mt-unsafe): インポートはメインスレッド 1 本で走る
			const char* value = std::getenv(name);
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
		s.out.open(path, std::ios::out | std::ios::trunc);
		if (!s.out.is_open())
			return false; // 書けない場所でも**インポートは続ける**（診断は付随機能）
		s.path = path;
		s.start = std::chrono::steady_clock::now();
		return true;
	}

	bool isOpen()
	{
		return state().out.is_open();
	}

	const std::string& path()
	{
		return state().path;
	}

	void log(const std::string& message)
	{
		State& s = state();
		if (!s.out.is_open())
			return;
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - s.start);
		s.out << "[" << elapsed.count() << " ms] " << message << "\n";
		// **1 行ごとにフラッシュする。** 落ちたときにバッファの中身は残らないので、
		// これをしないと肝心の最終行（＝原因箇所の直前）が消える。
		s.out.flush();
	}

	void close()
	{
		State& s = state();
		if (s.out.is_open())
			s.out.close();
		s.path.clear();
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
