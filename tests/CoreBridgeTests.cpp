//
//	CoreBridgeTests.cpp
//
//	MCP ブリッジのスプール（src/core/Bridge.h）の単体テスト。**Vectorworks も MCP も
//	要らない**——このファイルが押さえるのは「置かれた要求を送った順に拾い、消し、応える」
//	という受け渡しの作法と、外から来たものに対する安全側の振る舞いだけである
//	（何を返す道具があるかは draw/McpBridge.cpp。そちらは実機で確かめる）。
//

#include "TestFramework.h"
#include "core/Bridge.h"
#include "core/Json.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using HomeskzIfcImport::core::BridgeRequest;
using HomeskzIfcImport::core::BridgeResponse;
using HomeskzIfcImport::core::BridgeSpool;
using HomeskzIfcImport::core::bridgeSpoolDir;
using HomeskzIfcImport::core::isValidBridgeId;
using HomeskzIfcImport::core::Json;
using HomeskzIfcImport::core::kBridgeRequestSuffix;
using HomeskzIfcImport::core::kBridgeResponseSuffix;
using HomeskzIfcImport::core::kBridgeStatusFile;
using HomeskzIfcImport::core::parseBridgeRequest;

namespace
{
	// テスト 1 件ぶんの作業ディレクトリ（作って、抜けるときに消す）。
	//
	// **一時ディレクトリを使わない。** TMPDIR / TEMP から組み立てたパスがファイル操作へ
	// 届くと CodeQL が環境変数由来のパスとして報告する（cpp/path-injection）ので、
	// CoreTraceTests と同じくビルドツリーの中へ書く（置き場所は CMake が定義で渡す）。
	class TempDir
	{
	public:
		explicit TempDir(const std::string& tag)
		{
			fPath = std::filesystem::path(HOMESKZ_BRIDGE_TEST_DIR) / ("vw-bridge-test-" + tag);
			std::error_code ec;
			std::filesystem::remove_all(fPath, ec);
			std::filesystem::create_directories(fPath, ec);
		}
		~TempDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(fPath, ec);
		}
		TempDir(const TempDir&) = delete;
		TempDir& operator=(const TempDir&) = delete;

		std::string path() const
		{
			return fPath.string();
		}

	private:
		std::filesystem::path fPath;
	};

	void WriteFile(const std::string& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
	}

	std::string ReadFile(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	// 要求ファイルを 1 つ置く。
	void PutRequest(const std::string& dir, const std::string& id, const std::string& tool)
	{
		Json value = Json::object();
		value.set("id", Json::string(id));
		value.set("tool", Json::string(tool));
		value.set("args", Json::object());
		WriteFile(dir + "/" + id + kBridgeRequestSuffix, value.dump());
	}
} // namespace

TEST(bridge_spool_dir_is_the_same_rule_on_both_sides)
{
	// **Python 側（scripts/mcp/vw-mcp-server.py）と同じ組み立て。** 変えるときは両方。
	CHECK_EQ(bridgeSpoolDir("/Users/me", "min-nano_structure"),
			 std::string("/Users/me/.min-nano_structure/mcp"));
	// 末尾の区切りは足さない（home の綴りが環境によって揺れる）。
	CHECK_EQ(bridgeSpoolDir("/Users/me/", "min-nano_structure"),
			 std::string("/Users/me/.min-nano_structure/mcp"));
	CHECK_EQ(bridgeSpoolDir("C:\\Users\\me\\", "min-nano_structureDev"),
			 std::string("C:\\Users\\me/.min-nano_structureDev/mcp"));
}

TEST(bridge_id_charset_is_the_only_gate)
{
	CHECK(isValidBridgeId("000000000001-a1b2c3d4"));
	CHECK(isValidBridgeId("a"));
	CHECK(!isValidBridgeId(""));
	CHECK(!isValidBridgeId(std::string(65, 'a')));
	// **スプールの外へ書かせない**（これがその関門）。
	CHECK(!isValidBridgeId("../../etc/passwd"));
	CHECK(!isValidBridgeId("a/b"));
	CHECK(!isValidBridgeId("a\\b"));
	CHECK(!isValidBridgeId("a.b"));
	CHECK(!isValidBridgeId("a b"));
}

TEST(bridge_request_parsing)
{
	BridgeRequest request;
	std::string error;
	CHECK(parseBridgeRequest("{\"id\":\"abc\",\"tool\":\"vw_layers\",\"args\":{\"limit\":5}}",
							 request, error));
	CHECK_EQ(request.id, std::string("abc"));
	CHECK_EQ(request.tool, std::string("vw_layers"));
	CHECK_EQ(request.args.at("limit").asNumber(), 5.0);

	// args を省いてもオブジェクトとして受け取れる（道具側に分岐を撒かない）。
	CHECK(parseBridgeRequest("{\"id\":\"abc\",\"tool\":\"vw_ping\"}", request, error));
	CHECK(request.args.isObject());

	// 綴りの悪い id・tool 無し・JSON でないものは受けない。
	CHECK(!parseBridgeRequest("{\"id\":\"../x\",\"tool\":\"vw_ping\"}", request, error));
	CHECK(!parseBridgeRequest("{\"id\":\"abc\"}", request, error));
	CHECK(!parseBridgeRequest("[1,2]", request, error));
	CHECK(!parseBridgeRequest("not json", request, error));
}

TEST(bridge_spool_round_trip)
{
	const TempDir temp("round-trip");
	BridgeSpool spool(temp.path() + "/mcp");
	std::string error;
	CHECK(spool.prepare(error));
	CHECK(std::filesystem::is_directory(spool.dir()));

	PutRequest(spool.dir(), "000000000001-aaaa", "vw_ping");

	std::vector<std::string> broken;
	const std::vector<BridgeRequest> requests = spool.poll(broken);
	CHECK_EQ(requests.size(), std::size_t(1));
	CHECK(broken.empty());
	CHECK_EQ(requests[0].tool, std::string("vw_ping"));
	// **拾ったら消える**（同じものを毎周拾い直さない）。
	CHECK(!std::filesystem::exists(spool.dir() + "/000000000001-aaaa" + kBridgeRequestSuffix));

	BridgeResponse response;
	response.id = requests[0].id;
	response.ok = true;
	response.result = Json::object();
	response.result.set("pong", Json::boolean(true));
	CHECK(spool.reply(response, error));

	const std::string written =
		ReadFile(spool.dir() + "/000000000001-aaaa" + std::string(kBridgeResponseSuffix));
	CHECK_EQ(written, std::string("{\"id\":\"000000000001-aaaa\",\"ok\":true,"
								  "\"result\":{\"pong\":true}}"));
	// 書きかけ（.tmp）は残さない。
	CHECK(!std::filesystem::exists(spool.dir() + "/000000000001-aaaa" +
								   std::string(kBridgeResponseSuffix) + ".tmp"));
}

TEST(bridge_spool_polls_in_name_order)
{
	const TempDir temp("order");
	BridgeSpool spool(temp.path() + "/mcp");
	std::string error;
	CHECK(spool.prepare(error));

	// 置く順を入れ替えても、拾う順は名前（＝Python が付ける連番）の昇順。
	PutRequest(spool.dir(), "000000000003-cccc", "c");
	PutRequest(spool.dir(), "000000000001-aaaa", "a");
	PutRequest(spool.dir(), "000000000002-bbbb", "b");

	std::vector<std::string> broken;
	const std::vector<BridgeRequest> requests = spool.poll(broken);
	CHECK_EQ(requests.size(), std::size_t(3));
	CHECK_EQ(requests[0].tool, std::string("a"));
	CHECK_EQ(requests[1].tool, std::string("b"));
	CHECK_EQ(requests[2].tool, std::string("c"));
}

TEST(bridge_spool_reports_broken_requests_once)
{
	const TempDir temp("broken");
	BridgeSpool spool(temp.path() + "/mcp");
	std::string error;
	CHECK(spool.prepare(error));

	WriteFile(spool.dir() + "/000000000001-aaaa" + kBridgeRequestSuffix, "{ not json");
	// 中身の id がファイル名と食い違うもの（応答の宛先が定まらない）。
	WriteFile(spool.dir() + "/000000000002-bbbb" + kBridgeRequestSuffix,
			  "{\"id\":\"zzzz\",\"tool\":\"vw_ping\"}");
	PutRequest(spool.dir(), "000000000003-cccc", "vw_ping");

	std::vector<std::string> broken;
	const std::vector<BridgeRequest> requests = spool.poll(broken);
	CHECK_EQ(requests.size(), std::size_t(1));
	CHECK_EQ(broken.size(), std::size_t(2));
	CHECK_EQ(broken[0], std::string("000000000001-aaaa"));
	CHECK_EQ(broken[1], std::string("000000000002-bbbb"));

	// **壊れた要求も消えている**（残すと永久に拾い直す）。
	const std::vector<BridgeRequest> again = spool.poll(broken);
	CHECK(again.empty());
	CHECK(broken.empty());
}

TEST(bridge_spool_sweeps_leftovers_but_keeps_nothing_else)
{
	const TempDir temp("sweep");
	BridgeSpool spool(temp.path() + "/mcp");
	std::string error;
	CHECK(spool.prepare(error));

	PutRequest(spool.dir(), "000000000001-aaaa", "vw_ping");
	WriteFile(spool.dir() + "/000000000009-zzzz" + kBridgeResponseSuffix, "{}");
	WriteFile(spool.dir() + "/000000000009-zzzz" + std::string(kBridgeResponseSuffix) + ".tmp",
			  "{");
	WriteFile(spool.dir() + "/notes.txt", "触らない");

	CHECK_EQ(spool.sweep(), std::size_t(3));
	CHECK(std::filesystem::exists(spool.dir() + "/notes.txt"));
}

TEST(bridge_spool_status_appears_and_disappears)
{
	const TempDir temp("status");
	BridgeSpool spool(temp.path() + "/mcp");
	std::string error;
	CHECK(spool.prepare(error));

	Json status = Json::object();
	status.set("protocol", Json::integer(1));
	CHECK(spool.writeStatus(status, error));
	CHECK(std::filesystem::exists(spool.dir() + "/" + kBridgeStatusFile));
	CHECK_EQ(ReadFile(spool.dir() + "/" + kBridgeStatusFile), std::string("{\"protocol\":1}"));

	spool.removeStatus();
	CHECK(!std::filesystem::exists(spool.dir() + "/" + kBridgeStatusFile));
	// 2 回目は何もしない（止め損ねても落ちない）。
	spool.removeStatus();
}

TEST(bridge_spool_refuses_a_reply_with_a_bad_id)
{
	const TempDir temp("bad-id");
	BridgeSpool spool(temp.path() + "/mcp");
	std::string error;
	CHECK(spool.prepare(error));

	BridgeResponse response;
	response.id = "../escape";
	response.ok = true;
	CHECK(!spool.reply(response, error));
	CHECK(!error.empty());
}

TEST_MAIN();
