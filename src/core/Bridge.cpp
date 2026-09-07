//
//	core/Bridge.cpp
//
//	スプールの実装（意図と制約は core/Bridge.h）。ファイル操作は std::filesystem で足りる。
//
//	**`file_size` / `last_write_time` は使わない。** MSVC の <filesystem> はその中で
//	フラグの enum を範囲外へキャストしており、clang-tidy の静的解析がそれを我々の呼び出しの
//	経路として報告する（src/PayloadHost.cpp に同じ但し書きがある。tidy-windows で実測）。
//	大きさは「読みながら上限で打ち切る」で足り、順序は**ファイル名**（Python が付ける連番）
//	で決めるので、どちらも要らない。
//

#include "core/Bridge.h"
#include "core/Json.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#	include <cerrno>
#else
#	include <cerrno>
#	include <sys/stat.h>
#	include <unistd.h>
#endif

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 上限つきで丸ごと読む。上限を越えたら false（内容は捨てる）。
		bool ReadCapped(const std::filesystem::path& path, std::size_t limit, std::string& out)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;
			out.clear();
			std::array<char, 4096> buffer{};
			while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
				   in.gcount() > 0)
			{
				const auto got = static_cast<std::size_t>(in.gcount());
				if (out.size() + got > limit)
					return false;
				out.append(buffer.data(), got);
				if (!in)
					break;
			}
			return true;
		}

		// 同じディレクトリへ書いてから rename する。**読み手に半端な内容を拾わせない**
		// ための決まりで、応答も生存の印も同じ手順を通る。
		bool WriteAtomically(const std::filesystem::path& target, const std::string& text,
							 std::string& error)
		{
			std::filesystem::path temp = target;
			temp += kBridgeTempSuffix;
			{
				std::ofstream out(temp, std::ios::binary | std::ios::trunc);
				if (!out)
				{
					error = "書き込めません: " + temp.string();
					return false;
				}
				out.write(text.data(), static_cast<std::streamsize>(text.size()));
				if (!out)
				{
					error = "書き込みに失敗しました: " + temp.string();
					return false;
				}
			}
			std::error_code ec;
			// 置き換えで rename する（同じ名前が残っていても通す）。
			std::filesystem::rename(temp, target, ec);
			if (ec)
			{
				std::filesystem::remove(temp, ec);
				error = "置き換えに失敗しました: " + target.string();
				return false;
			}
			return true;
		}

		// 末尾が suffix か。
		bool EndsWith(const std::string& text, const std::string& suffix)
		{
			return text.size() >= suffix.size() &&
				   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}
	} // namespace

	// -----------------------------------------------------------------------
	std::string bridgeSpoolDir(const std::string& tempDir, const std::string& pluginName)
	{
		std::string base = tempDir;
		while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
			base.pop_back();
		return base + "/" + pluginName + "-mcp";
	}

	bool isValidBridgeId(const std::string& id)
	{
		if (id.empty() || id.size() > 64)
			return false;
		return std::all_of(id.begin(), id.end(),
						   [](const char ch)
						   {
							   return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
									  (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_';
						   });
	}

	bool parseBridgeRequest(const std::string& text, BridgeRequest& out, std::string& error)
	{
		Json value;
		if (!Json::parse(text, value, error))
			return false;
		if (!value.isObject())
		{
			error = "要求はオブジェクトでなければなりません";
			return false;
		}
		BridgeRequest made;
		made.id = value.at("id").asString();
		made.tool = value.at("tool").asString();
		made.args = value.at("args");
		if (!isValidBridgeId(made.id))
		{
			error = "id の綴りが不正です";
			return false;
		}
		if (made.tool.empty())
		{
			error = "tool がありません";
			return false;
		}
		if (!made.args.isObject())
			made.args = Json::object();
		out = std::move(made);
		return true;
	}

	std::string dumpBridgeResponse(const BridgeResponse& response)
	{
		Json value = Json::object();
		value.set("id", Json::string(response.id));
		value.set("ok", Json::boolean(response.ok));
		if (!response.ok)
			value.set("error", Json::string(response.error));
		else
			value.set("result", response.result.isNull() ? Json::object() : response.result);
		return value.dump();
	}

	BridgeResponse bridgeFailure(const std::string& id, const std::string& error)
	{
		BridgeResponse response;
		response.id = id;
		response.ok = false;
		response.error = error;
		return response;
	}

	// -----------------------------------------------------------------------
	BridgeSpool::BridgeSpool(std::string dir) : fDir(std::move(dir)) {}

	std::string BridgeSpool::pathFor(const std::string& name) const
	{
		return fDir + "/" + name;
	}

	bool BridgeSpool::prepare(std::string& error)
	{
		error.clear();
		if (fDir.empty())
		{
			error = "スプールの場所が決まりません（一時ディレクトリが読めない？）";
			return false;
		}
#if defined(_WIN32)
		// Windows の %TEMP% は利用者ごと（AppData\Local\Temp）なので、持ち主の確認は
		// 要らない。
		std::error_code ec;
		std::filesystem::create_directories(fDir, ec);
		if (ec && !std::filesystem::is_directory(fDir))
		{
			error = "スプールを作れません: " + fDir + "（" + ec.message() + "）";
			return false;
		}
		return true;
#else
		// **自分にしか書けない形で作る**（0700）。umask はビットを落とすだけなので、
		// これより緩くはならない。親（一時ディレクトリ）は既にあるので 1 段でよい。
		if (::mkdir(fDir.c_str(), S_IRWXU) != 0 && errno != EEXIST)
		{
			error = "スプールを作れません: " + fDir;
			return false;
		}

		// 既にあったものは**素性を確かめてから使う**（Bridge.h「持ち主と権限を確かめる」）。
		// `stat` は関数名でもあるので型は別名で綴る（src/PayloadHost.cpp と同じ理由）。
		using FileStat = struct stat;
		FileStat info{};
		if (::stat(fDir.c_str(), &info) != 0)
		{
			error = "スプールを確かめられません: " + fDir;
			return false;
		}
		if (!S_ISDIR(info.st_mode))
		{
			error = "スプールの場所がディレクトリではありません: " + fDir;
			return false;
		}
		if (info.st_uid != ::geteuid())
		{
			error = "スプールが他の利用者のものです: " + fDir;
			return false;
		}
		if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
		{
			error = "スプールが他からも書き込める状態です: " + fDir;
			return false;
		}
		return true;
#endif
	}

	std::size_t BridgeSpool::sweep()
	{
		std::size_t removed = 0;
		std::error_code ec;
		const std::filesystem::directory_iterator it(fDir, ec);
		if (ec)
			return 0;
		for (const auto& entry : it)
		{
			const std::string name = entry.path().filename().string();
			if (!EndsWith(name, kBridgeRequestSuffix) && !EndsWith(name, kBridgeResponseSuffix) &&
				!EndsWith(name, kBridgeTempSuffix))
				continue;
			std::error_code removeEc;
			if (std::filesystem::remove(entry.path(), removeEc))
				++removed;
		}
		return removed;
	}

	std::vector<BridgeRequest> BridgeSpool::poll(std::vector<std::string>& broken)
	{
		std::vector<BridgeRequest> requests;
		broken.clear();

		// **まず名前だけ集めて並べ替える。** ディレクトリの列挙順は OS 任せなので、
		// そのまま処理すると送った順に応えられない（CLAUDE.md「決定性を守る」）。
		std::vector<std::string> names;
		{
			std::error_code ec;
			const std::filesystem::directory_iterator it(fDir, ec);
			if (ec)
				return requests;
			for (const auto& entry : it)
			{
				const std::string name = entry.path().filename().string();
				if (EndsWith(name, kBridgeRequestSuffix))
					names.push_back(name);
			}
		}
		std::sort(names.begin(), names.end());
		if (names.size() > kBridgeMaxRequestsPerPoll)
			names.resize(kBridgeMaxRequestsPerPoll);

		for (const std::string& name : names)
		{
			const std::filesystem::path path(pathFor(name));
			std::string text;
			const bool read = ReadCapped(path, kBridgeMaxRequestBytes, text);

			// **読めても読めなくても、まず消す。** 残すと同じものを毎周拾い直す。
			std::error_code ec;
			std::filesystem::remove(path, ec);

			// ファイル名から id を拾う（"<id>.req.json"）。壊れた要求にも応えるために要る。
			const std::string id =
				name.substr(0, name.size() - std::string(kBridgeRequestSuffix).size());

			if (!read)
			{
				if (isValidBridgeId(id))
					broken.push_back(id);
				continue;
			}
			BridgeRequest request;
			std::string error;
			if (!parseBridgeRequest(text, request, error))
			{
				if (isValidBridgeId(id))
					broken.push_back(id);
				continue;
			}
			// 中身の id とファイル名が食い違うものは受けない（応答の宛先が定まらない）。
			if (request.id != id)
			{
				if (isValidBridgeId(id))
					broken.push_back(id);
				continue;
			}
			requests.push_back(std::move(request));
		}
		return requests;
	}

	bool BridgeSpool::reply(const BridgeResponse& response, std::string& error)
	{
		error.clear();
		if (!isValidBridgeId(response.id))
		{
			error = "id の綴りが不正です";
			return false;
		}
		const std::filesystem::path path(pathFor(response.id + kBridgeResponseSuffix));
		return WriteAtomically(path, dumpBridgeResponse(response), error);
	}

	bool BridgeSpool::writeStatus(const Json& status, std::string& error)
	{
		error.clear();
		const std::filesystem::path path(pathFor(kBridgeStatusFile));
		return WriteAtomically(path, status.dump(), error);
	}

	void BridgeSpool::removeStatus()
	{
		std::error_code ec;
		std::filesystem::remove(std::filesystem::path(pathFor(kBridgeStatusFile)), ec);
	}
} // namespace HomeskzIfcImport::core
