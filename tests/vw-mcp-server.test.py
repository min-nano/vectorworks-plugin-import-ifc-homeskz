#!/usr/bin/env python3
#
# vw-mcp-server.test.py — MCP サーバ（scripts/mcp/vw-mcp-server.py）の回帰テスト
#
# 【何を押さえるか】このスクリプトは**受け渡しの Claude 側**である。Vectorworks 側
# （src/draw/McpBridge.cpp）は SDK が要るので CI では動かせないから、ここでは
# **プラグインと同じ作法でスプールに応える代役**を立てて、
#
#   * MCP の握手（initialize / tools/list / tools/call）が成り立つこと、
#   * 道具の一覧が**代役から取れる**こと（＝一覧の真実がプラグイン側にあること）、
#   * 応答が返らない・ブリッジが動いていないときに**待ち切らずに理由を返す**こと、
#   * 送った順に処理されること（要求ファイル名の連番）、
#
# を確かめる。**代役が真似ているのは src/core/Bridge.h の綴りと手順だけ**なので、
# どちらかを変えたらこのテストが落ちる——それがこのテストの主眼である。
#
# ネットワークも SDK も要らない。python3 だけで走る。

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(os.path.dirname(HERE), "scripts", "mcp", "vw-mcp-server.py")

FAILURES = []


def check(condition, label):
    if condition:
        print("[ PASS ] " + label)
    else:
        print("[ FAIL ] " + label)
        FAILURES.append(label)


def check_eq(actual, expected, label):
    check(actual == expected, "%s (actual=%r expected=%r)" % (label, actual, expected))


class FakeVectorworks(threading.Thread):
    """プラグイン側の代役。**綴りと手順は src/core/Bridge.h に合わせてある。**"""

    TOOLS = [
        {
            "name": "vw_ping",
            "description": "素性を返す",
            "inputSchema": {"type": "object", "properties": {}},
        },
        {
            "name": "vw_layers",
            "description": "レイヤ一覧",
            "inputSchema": {"type": "object", "properties": {}},
        },
    ]

    def __init__(self, spool):
        threading.Thread.__init__(self)
        self.spool = spool
        self.daemon = True
        self.stop_flag = threading.Event()
        self.seen = []  # 拾った順（＝送った順のはず）

    def run(self):
        os.makedirs(self.spool, exist_ok=True)
        while not self.stop_flag.is_set():
            self._beat()
            for name in sorted(os.listdir(self.spool)):
                if not name.endswith(".req.json"):
                    continue
                path = os.path.join(self.spool, name)
                try:
                    with open(path, "r", encoding="utf-8") as handle:
                        request = json.load(handle)
                except (OSError, ValueError):
                    continue
                os.remove(path)
                self.seen.append(request["tool"])
                self._reply(request)
            time.sleep(0.02)
        try:
            os.remove(os.path.join(self.spool, "bridge.json"))
        except OSError:
            # 代役の後始末。消せなくてもテストの結果は変わらない（作業ディレクトリごと
            # 捨てる）ので、ここで止めない。
            pass

    def _beat(self):
        payload = {"plugin": "min-nano_structure", "protocol": 1, "beat": int(time.time())}
        temp = os.path.join(self.spool, "bridge.json.tmp")
        with open(temp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle)
        os.replace(temp, os.path.join(self.spool, "bridge.json"))

    def _reply(self, request):
        tool = request["tool"]
        if tool == "vw_tools":
            body = {"ok": True, "result": {"tools": self.TOOLS, "protocol": 1}}
        elif tool == "vw_ping":
            body = {"ok": True, "result": {"plugin": "min-nano_structure", "document_open": True}}
        elif tool == "vw_layers":
            body = {"ok": True, "result": {"layers": [{"name": "1-FL"}], "count": 1}}
        elif tool == "vw_slow":
            return  # わざと応えない（待ち切らずに諦めるかを見る）
        else:
            body = {"ok": False, "error": "知らない道具です: " + tool}
        body["id"] = request["id"]
        temp = os.path.join(self.spool, request["id"] + ".res.json.tmp")
        with open(temp, "w", encoding="utf-8") as handle:
            json.dump(body, handle, ensure_ascii=False)
        os.replace(temp, os.path.join(self.spool, request["id"] + ".res.json"))


def drive(spool, messages, timeout="30", by_tmpdir=False):
    """サーバへ一括で流し込み、返ってきた JSON 行を返す。

    by_tmpdir=True のときは VW_MCP_SPOOL を渡さず、**TMPDIR から自力で探させる**
    （プラグイン側と Claude 側が別々に場所を決める、本番と同じ経路）。
    """
    env = dict(os.environ)
    if by_tmpdir:
        env.pop("VW_MCP_SPOOL", None)
        env["TMPDIR"] = os.path.dirname(spool.rstrip("/"))
        env["TMP"] = env["TMPDIR"]
        env["TEMP"] = env["TMPDIR"]
        env["VW_MCP_PLUGIN"] = "min-nano_structure"
    else:
        env["VW_MCP_SPOOL"] = spool
    env["VW_MCP_TIMEOUT"] = timeout
    text = "".join(json.dumps(m) + "\n" for m in messages)
    done = subprocess.run(
        [sys.executable, SERVER],
        input=text.encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
        env=env,
    )
    out = []
    for line in done.stdout.decode("utf-8").splitlines():
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


def call(name, arguments=None, request_id=1):
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments or {}},
    }


def content_text(reply):
    return reply["result"]["content"][0]["text"]


def main():
    if not os.path.exists(SERVER):
        print("[ FAIL ] サーバが見つかりません: " + SERVER)
        return 1

    root = tempfile.mkdtemp(prefix="vw-mcp-test-")
    spool = os.path.join(root, "mcp")
    try:
        # --- ブリッジが動いていないとき ---------------------------------
        os.makedirs(spool)
        replies = drive(
            spool,
            [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                call("vw_bridge_status", request_id=3),
                call("vw_ping", request_id=4),
            ],
            timeout="1",
        )
        check_eq(len(replies), 4, "動いていなくても 4 件すべてに答える")
        check_eq(
            replies[0]["result"]["serverInfo"]["name"],
            "vectorworks-bridge",
            "initialize が素性を返す",
        )
        names = [t["name"] for t in replies[1]["result"]["tools"]]
        check(
            names == ["vw_bridge_status"],
            "ブリッジが無いときの一覧は自前の道具だけ (%r)" % names,
        )
        status = json.loads(content_text(replies[2]))
        check(status["running"] is False, "vw_bridge_status が「動いていない」と答える")
        check("MCP ブリッジを開始" in status["hint"], "どうすれば動くかを案内する")
        check(replies[3]["result"]["isError"] is True, "道具の呼び出しはエラーになる")
        check(
            "ブリッジが動いていません" in content_text(replies[3]),
            "エラーの本文が理由を言う",
        )

        # --- 動いているとき ---------------------------------------------
        fake = FakeVectorworks(spool)
        fake.start()
        time.sleep(0.3)
        replies = drive(
            spool,
            [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                call("vw_ping", request_id=3),
                call("vw_layers", request_id=4),
                call("vw_nope", request_id=5),
                {"jsonrpc": "2.0", "id": 6, "method": "ping"},
            ],
        )
        check_eq(len(replies), 6, "通知には答えず、リクエストにだけ答える")
        names = [t["name"] for t in replies[1]["result"]["tools"]]
        check_eq(
            names,
            ["vw_bridge_status", "vw_ping", "vw_layers"],
            "**一覧の真実はプラグイン側**（代役が返した 2 つが並ぶ）",
        )
        ping = json.loads(content_text(replies[2]))
        check_eq(ping["plugin"], "min-nano_structure", "vw_ping の結果が素通しで返る")
        layers = json.loads(content_text(replies[3]))
        check_eq(layers["count"], 1, "vw_layers の結果が素通しで返る")
        check(replies[4]["result"]["isError"] is True, "知らない道具はエラーとして返る")
        check(
            "知らない道具です" in content_text(replies[4]),
            "プラグイン側のエラー文がそのまま見える",
        )
        check_eq(replies[5]["result"], {}, "ping に空で答える")

        # 送った順に拾われている（要求ファイル名の連番が効いている）。
        check_eq(
            fake.seen,
            ["vw_tools", "vw_ping", "vw_layers", "vw_nope"],
            "送った順に処理される",
        )

        # --- 応答が返らないとき -----------------------------------------
        fake.seen = []
        started = time.time()
        replies = drive(spool, [call("vw_slow", request_id=1)], timeout="1")
        elapsed = time.time() - started
        check(replies[0]["result"]["isError"] is True, "応答が無ければエラーで返る")
        check("応答がありません" in content_text(replies[0]), "待ち切れなかったと言う")
        check(elapsed < 30, "待ち時間の上限で諦める（%.1f 秒）" % elapsed)
        # 置きっぱなしの要求は引き上げてある（次のセッションが拾わないように）。
        leftovers = [n for n in os.listdir(spool) if n.endswith(".req.json")]
        check_eq(leftovers, [], "諦めた要求はスプールに残さない")

        # ここから先は代役を建て直すので、先に止める（走ったままディレクトリの名前を
        # 変えると、代役が印を書けずに落ちる）。
        fake.stop_flag.set()
        fake.join(timeout=5)

        # --- 場所を自力で探し当てる -------------------------------------
        # **本番はこの経路。** プラグイン側は自分の一時ディレクトリへ置き、こちらは
        # 候補を順に見て生きた印のある場所を使う（両側で $TMPDIR が食い違いうるため）。
        # スプールは <一時ディレクトリ>/min-nano_structure-mcp でなければならない。
        found = os.path.join(root, "min-nano_structure-mcp")
        os.rename(spool, found)
        found_fake = FakeVectorworks(found)
        found_fake.start()
        time.sleep(0.3)
        replies = drive(found, [call("vw_ping", request_id=1)], by_tmpdir=True)
        check(
            replies[0]["result"]["isError"] is False,
            "VW_MCP_SPOOL 無しでも一時ディレクトリから探し当てる",
        )
        found_fake.stop_flag.set()
        found_fake.join(timeout=5)

        # 名前が違えば見つからない（＝両側の綴りが対であることの確認）。
        os.rename(found, os.path.join(root, "wrong-name-mcp"))
        replies = drive(
            os.path.join(root, "wrong-name-mcp"),
            [call("vw_bridge_status", request_id=1)],
            timeout="1",
            by_tmpdir=True,
        )
        status = json.loads(content_text(replies[0]))
        check(status["running"] is False, "綴りが違うスプールは見つけない")
        check(
            any(c.endswith("min-nano_structure-mcp") for c in status["searched"]),
            "探した場所を返す（%r）" % status["searched"],
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print("")
    if FAILURES:
        print("%d 件失敗しました。" % len(FAILURES))
        return 1
    print("すべて通りました。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
