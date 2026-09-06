//
//	core/Bridge.h
//
//	**MCP ブリッジの、SDK を知らない半分。** 要求／応答の形と、その受け渡しに使う
//	スプール（ファイルの置き場）の作法だけを持つ。Vectorworks から何を読むか（＝道具の
//	中身）は draw/McpBridge.h にある。
//
//	    Claude ──MCP(stdio)──▶ scripts/mcp/vw-mcp-server.py
//	                              │ <id>.req.json を書く／<id>.res.json を待つ
//	                              ▼
//	                          スプール（既定: ~/.min-nano_structure/mcp）
//	                              ▲
//	                              │ 拾う／応える（メニュー「MCP ブリッジ」の実行中だけ）
//	                        Vectorworks（draw::runMcpBridge）
//
//	【なぜファイルなのか】ソケットを開くと (1) macOS のファイアウォールが利用者に問い、
//	(2) Winsock と BSD ソケットで分岐が増え、(3) 受け口をメインスレッド以外に置くと
//	SDK 呼び出しの前提（Vectorworks はメインスレッドから呼ぶ）を壊す。**ファイルなら
//	どれも起きない**——ブリッジは Vectorworks のメインスレッドで回る 1 本のループのまま、
//	置かれたファイルを拾って応えるだけでよい。速さは要らない（人が待つ対話の速度）。
//
//	【なぜ 2 プロセスなのか】MCP そのもの（JSON-RPC・stdio・initialize の握手）は
//	Claude 側の作法であって図面の話ではない。**プラグインに持ち込まず Python の小さな
//	サーバへ出す**と、プロトコルが変わってもプラグインを配り直さずに済み、C++ 側は
//	「道具の名前と引数を受けて図面から値を返す」だけになる。
//
//	【安全側の決めごと】スプールへ書くのは**別プロセス**である。したがって:
//	  * id は綴りを検査してからファイル名に使う（`../` を混ぜてスプールの外へ書かせない）。
//	  * 要求 1 件の大きさに上限を設ける（巨大なファイルを丸ごと読み込まない）。
//	  * 1 周で捌く件数に上限を設ける（溜まっていても Vectorworks を握り続けない）。
//	  * 壊れた要求は**消してから**エラーで応える（同じものを永久に拾い直さない）。
//

#pragma once

#include "core/Json.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// スプールに置くファイルの綴り。**Python 側（scripts/mcp/vw-mcp-server.py）と対**なので、
	// 変えるときは両方を直す。
	inline constexpr const char* kBridgeRequestSuffix = ".req.json";
	inline constexpr const char* kBridgeResponseSuffix = ".res.json";
	inline constexpr const char* kBridgeStatusFile = "bridge.json";
	inline constexpr const char* kBridgeTempSuffix = ".tmp";

	// 受け渡しの版。**要求／応答の形を変えたら上げる**（殻と本体の ABI と同じ考え方で、
	// プラグインと Python サーバは別々に配られうる）。status に載せて Python 側が確かめる。
	inline constexpr int kBridgeProtocolVersion = 1;

	// 要求 1 件の大きさの上限（バイト）と、1 周で捌く件数の上限。
	inline constexpr std::size_t kBridgeMaxRequestBytes = 1U << 20U; // 1 MiB
	inline constexpr std::size_t kBridgeMaxRequestsPerPoll = 16;

	// スプールの既定の置き場所。**home の下に置く**——一時ディレクトリは環境変数で
	// プロセスごとに違いうる（macOS の $TMPDIR は利用者ごとの乱数パス）ので、別々に
	// 起動した 2 つのプロセスが同じ場所を指すとは限らない。home なら両側で必ず一致する。
	// 区切りは '/' で組む（Windows も受け付ける）。
	std::string bridgeSpoolDir(const std::string& home, const std::string& pluginName);

	// id としてファイル名に使ってよい綴りか（英数字・'-'・'_' のみ、1〜64 文字）。
	// **これがスプールの外へ書かせないための唯一の関門**である。
	bool isValidBridgeId(const std::string& id);

	// -----------------------------------------------------------------------
	// 要求 1 件。
	struct BridgeRequest
	{
		std::string id;	  // 応答のファイル名になる
		std::string tool; // 道具の名前（draw/McpBridge.cpp の表を引く）
		Json args;		  // 道具ごとの引数（オブジェクト）
	};

	// 応答 1 件。ok が false のときだけ error に理由が入る。
	struct BridgeResponse
	{
		std::string id;
		bool ok = false;
		std::string error;
		Json result;
	};

	// 要求のテキスト → BridgeRequest。id / tool が無い・空なら失敗。
	bool parseBridgeRequest(const std::string& text, BridgeRequest& out, std::string& error);

	// 応答 → JSON テキスト。
	std::string dumpBridgeResponse(const BridgeResponse& response);

	// 失敗の応答を 1 つ作る短縮。
	BridgeResponse bridgeFailure(const std::string& id, const std::string& error);

	// -----------------------------------------------------------------------
	// **スプール 1 つ。** ディレクトリを用意し、要求を拾い、応答を書く。SDK を知らないので
	// 単体テストできる（tests/CoreBridgeTests.cpp）。
	class BridgeSpool
	{
	public:
		explicit BridgeSpool(std::string dir);

		const std::string& dir() const
		{
			return fDir;
		}

		// ディレクトリを作る（既にあれば何もしない）。作れなければ false と理由。
		bool prepare(std::string& error);

		// 前の回の残骸（要求・応答・書きかけ）を消す。**開始時に 1 回**呼ぶ——落ちた
		// セッションの応答を新しいセッションのものと取り違えないため。消した数を返す。
		std::size_t sweep();

		// 置かれている要求を**名前の昇順で**取り出し、そのファイルを消す。名前の昇順は
		// Python が付ける連番の順（＝送った順）である（CLAUDE.md「決定性を守る」）。
		//
		// 読めなかった要求は `out` に「壊れている」印の付いた要求として載せる（id は
		// ファイル名から拾う）。呼ぶ側はそれにエラーで応えればよく、**拾い直しは起きない**。
		std::vector<BridgeRequest> poll(std::vector<std::string>& broken);

		// 応答を書く（同じディレクトリへ書いてから rename する＝読み手が半端な内容を
		// 拾わない）。
		bool reply(const BridgeResponse& response, std::string& error);

		// 生存の印。ブリッジが動いている間だけ置かれ、止まると消える。
		bool writeStatus(const Json& status, std::string& error);
		void removeStatus();

	private:
		std::string fDir;

		std::string pathFor(const std::string& name) const;
	};
} // namespace HomeskzIfcImport::core
