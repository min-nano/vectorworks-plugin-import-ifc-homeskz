#!/usr/bin/env python3
#
# vw-mcp-server.test.py — MCP サーバ（scripts/mcp/vw-mcp-server.py）の回帰テスト
#
# 【何を確かめるか】このスクリプトは**受け渡しの Claude 側**である。Vectorworks 側
# （src/draw/McpBridge.cpp）は SDK が要るので CI では動かせないから、ここでは
# **プラグインと同じ作法でスプールに応える代役**を立てて、
#
#   * MCP の握手（initialize / tools/list / tools/call）が成り立つこと、
#   * 道具の一覧が**代役から取れる**こと（＝一覧の真実がプラグイン側にあること）、
#   * 応答が返らない・ブリッジが動いていないときに**待ち切らずに理由を返す**こと、
#   * 送った順に処理されること（要求ファイル名の連番）、
#   * vw_launch が Vectorworks を起こし、橋が架かるまで待って一覧の取り直しを促すこと
#     （起こすものは VW_MCP_APP で代役に差し替える）、
#   * vw_restart のあと、橋が一度居なくなってまた架かるのを確認すること（M38）、
#   * 長く走る道具の最中（busy_until）は、印が古びていても「生きている」とみなすこと、
#   * プラグインの表の待ち時間（timeoutSeconds）を Claude へ見せないこと、
#   * vw_run_test は Vectorworks が居なければ起こしてから頼み、図面を読むだけの道具では
#     起こさないこと（M40）、
#   * スプールの探し方（一時ディレクトリの候補・持ち主と権限・綴りが対であること）、
#   * 複数のセッションから同時に呼ばれても、占有しているセッションの要求だけがブリッジへ
#     届き、他は断られて状況を購読できること（M42）、
#
# を確かめる。**代役が真似ているのは src/core/Bridge.h の綴りと手順だけ**なので、
# どちらかを変えたらこのテストが失敗する——それがこのテストの主眼である。
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
            # プラグインの表が持つ待ち時間（kTools の timeoutSeconds）。Claude には見せない。
            "timeoutSeconds": 5,
        },
    ]

    def __init__(self, spool):
        threading.Thread.__init__(self)
        self.spool = spool
        self.daemon = True
        self.stop_flag = threading.Event()
        self.seen = []  # 処理した順（＝送った順のはず）
        self.cleanups = 0  # vw_test_cleanup を受けた回数
        # 再起動の代役: この時刻までは印を書かない（＝Vectorworks が居ない）。
        self.down_until = 0.0

    def run(self):
        os.makedirs(self.spool, exist_ok=True)
        while not self.stop_flag.is_set():
            if time.time() < self.down_until:
                time.sleep(0.02)
                continue
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
            # 削除する）ので、ここで止めない。
            pass

    def _beat(self):
        payload = {"plugin": "min-nano_structureDev", "protocol": 1, "beat": int(time.time())}
        temp = os.path.join(self.spool, "bridge.json.tmp")
        with open(temp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle)
        os.replace(temp, os.path.join(self.spool, "bridge.json"))

    def _reply(self, request):
        tool = request["tool"]
        if tool == "vw_tools":
            body = {"ok": True, "result": {"tools": self.TOOLS, "protocol": 1}}
        elif tool == "vw_ping":
            body = {"ok": True, "result": {"plugin": "min-nano_structureDev", "document_open": True}}
        elif tool == "vw_restart":
            # プラグインは**応答を書いてから**再起動を頼む。代役は印を消して、しばらく応答しない。
            body = {"ok": True, "result": {"requested": True}}
            self.down_until = time.time() + 2.5
            try:
                os.remove(os.path.join(self.spool, "bridge.json"))
            except OSError:
                pass  # 消せなくても、印が古びれば同じこと
        elif tool == "vw_layers":
            body = {"ok": True, "result": {"layers": [{"name": "1-FL"}], "count": 1}}
        elif tool == "vw_test_cleanup":
            # 占有を解くときの片付け（M42）。
            self.cleanups += 1
            body = {"ok": True, "result": {"done": True, "message": "片付けました"}}
        elif tool == "vw_slow":
            return  # わざと応えない（待ち切らずに諦めるかを確かめる）
        else:
            body = {"ok": False, "error": "知らない道具です: " + tool}
        body["id"] = request["id"]
        temp = os.path.join(self.spool, request["id"] + ".res.json.tmp")
        with open(temp, "w", encoding="utf-8") as handle:
            json.dump(body, handle, ensure_ascii=False)
        os.replace(temp, os.path.join(self.spool, request["id"] + ".res.json"))


def drive(spool, messages, timeout="30", by_tmpdir=False, extra_env=None, with_notes=False):
    """サーバへ一括で流し込み、返ってきた応答（id のある行）を返す。

    with_notes=True のときは (応答, 通知のメソッド名の列) を返す。通知は応答の間に挟まるので、
    既定では除いて、応答だけを添字で参照できるようにする。

    by_tmpdir=True のときは VW_MCP_SPOOL を渡さず、**TMPDIR から自力で探させる**
    （プラグイン側と Claude 側が別々に場所を決める、本番と同じ経路）。
    """
    env = dict(os.environ)
    if by_tmpdir:
        env.pop("VW_MCP_SPOOL", None)
        env["TMPDIR"] = os.path.dirname(spool.rstrip("/"))
        env["TMP"] = env["TMPDIR"]
        env["TEMP"] = env["TMPDIR"]
        # **VW_MCP_PLUGIN を渡さない**——既定のプラグイン名（開発版）で探し当てることも確かめる。
        env.pop("VW_MCP_PLUGIN", None)
    else:
        env["VW_MCP_SPOOL"] = spool
    env["VW_MCP_TIMEOUT"] = timeout
    # **本物の占有に触れない。** 既定の置き場所は利用者ごとの一時ディレクトリで、この PC で
    # 動いている本物のセッションと共有している（M42）。
    env["VW_MCP_LEASE"] = lease_base_for(spool)
    env.update(extra_env or {})
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
    replies, notes = split_notifications(out)
    return (replies, notes) if with_notes else replies


def lease_base_for(spool):
    """テストで使う占有の置き場所（スプールの隣）。"""
    return os.path.join(os.path.dirname(spool.rstrip("/")), "test-lease")


def split_notifications(lines):
    """応答（id あり）と通知（id なし）に分ける。"""
    replies = [m for m in lines if "id" in m]
    notes = [m.get("method") for m in lines if "id" not in m]
    return replies, notes


# vw_launch の代役（Vectorworks の代わりに起こされる実行ファイル）。**起こされたら印を
# 書き続ける**——本物の Vectorworks がパレットの時計で印を書き直すのと同じに見せる。
# 止める印（stop ファイル）が置かれるか、上限の時間が来たら終わる。
FAKE_APP = '''#!%(python)s
import json, os, sys, time
spool = %(spool)r
stop = os.path.join(os.path.dirname(spool), "stop-fake-app")
os.makedirs(spool, exist_ok=True)
os.chmod(spool, 0o700)
deadline = time.time() + 30
while time.time() < deadline and not os.path.exists(stop):
    temp = os.path.join(spool, "bridge.json.tmp")
    with open(temp, "w") as handle:
        json.dump({"plugin": "min-nano_structureDev", "protocol": 1, "beat": int(time.time())}, handle)
    os.replace(temp, os.path.join(spool, "bridge.json"))
    time.sleep(0.2)
'''


def write_fake_app(path, spool, body=None):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(body if body is not None else FAKE_APP % {"python": sys.executable, "spool": spool})
    os.chmod(path, 0o755)


def check_launch(root):
    """**vw_launch**（Vectorworks を起こして、橋が架かるまで待つ）。"""
    if os.name == "nt":
        # 代役をスクリプトのまま直に起こせない（.exe でなければならない）。CI は Linux。
        return
    spool = os.path.join(root, "launch-mcp")
    app = os.path.join(root, "fake-vectorworks")

    # 起こすものが無ければ、起こしに行かずに理由を返す。
    replies = drive(
        spool,
        [call("vw_launch", {"timeout": 1}, request_id=1)],
        extra_env={"VW_MCP_APP": os.path.join(root, "no-such-app")},
    )
    check(replies[0]["result"]["isError"] is True, "起こすものが無ければエラー")
    check("VW_MCP_APP" in content_text(replies[0]), "何が無いのかを言う")

    # 起こしても橋が架からなければ、待つのを諦めて理由を返す。
    silent = os.path.join(root, "silent-vectorworks")
    write_fake_app(silent, spool, body="#!/bin/sh\nexit 0\n")
    started = time.time()
    replies = drive(
        spool, [call("vw_launch", {"timeout": 1}, request_id=1)], extra_env={"VW_MCP_APP": silent}
    )
    result = json.loads(content_text(replies[0]))
    check(replies[0]["result"]["isError"] is True, "橋が架からなければエラー")
    check(result["launched"] is True, "起こしたことは言う")
    check("MCP ブリッジを表示" in result["hint"], "パレットを出すよう案内する")
    check(time.time() - started < 30, "待つ上限で諦める")

    # 起こすと橋が架かる。一覧の取り直しを促す通知が出る。
    write_fake_app(app, spool)
    replies, notes = drive(
        spool,
        [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            call("vw_launch", {"timeout": 20}, request_id=2),
        ],
        # 代役は印を書くだけで応えないので、終了のときの片付けを長く待たせない。
        timeout="2",
        extra_env={"VW_MCP_APP": app},
        with_notes=True,
    )
    check(
        replies[0]["result"]["capabilities"]["tools"]["listChanged"] is True,
        "一覧が変わりうると宣言する",
    )
    result = json.loads(content_text(replies[1]))
    check(replies[1]["result"]["isError"] is False, "起こして橋が架かれば成功")
    check(result["launched"] is True and result["running"] is True, "起こして、受け付けている")
    check_eq(notes, ["notifications/tools/list_changed"], "一覧の取り直しを促す")

    # 既に受け付けていれば、2 つ目を起こさない。
    replies = drive(
        spool,
        [call("vw_launch", request_id=1)],
        timeout="2",
        extra_env={"VW_MCP_APP": os.path.join(root, "no-such-app")},
    )
    result = json.loads(content_text(replies[0]))
    check(replies[0]["result"]["isError"] is False, "受け付けていれば成功")
    check(result["launched"] is False, "受け付けていれば起こさない")

    with open(os.path.join(root, "stop-fake-app"), "w", encoding="utf-8"):
        pass
    time.sleep(0.5)


# vw_run_test の起こしてから頼む（M40）の代役。印を書くだけでなく、**vw_tools と
# vw_run_test に応える**（本物のパレットの時計と同じ手順。綴りは src/core/Bridge.h）。
SERVING_APP = '''#!%(python)s
import json, os, time
spool = %(spool)r
stop = os.path.join(os.path.dirname(spool), "stop-serving-app")
os.makedirs(spool, exist_ok=True)
os.chmod(spool, 0o700)
tools = [{"name": "vw_run_test", "description": "run", "inputSchema": {"type": "object"},
          "timeoutSeconds": 20}]
deadline = time.time() + 30
while time.time() < deadline and not os.path.exists(stop):
    temp = os.path.join(spool, "bridge.json.tmp")
    with open(temp, "w") as handle:
        json.dump({"plugin": "min-nano_structureDev", "protocol": 1, "beat": int(time.time())}, handle)
    os.replace(temp, os.path.join(spool, "bridge.json"))
    for name in sorted(os.listdir(spool)):
        if not name.endswith(".req.json"):
            continue
        path = os.path.join(spool, name)
        with open(path) as handle:
            request = json.load(handle)
        os.remove(path)
        if request["tool"] == "vw_tools":
            body = {"ok": True, "result": {"tools": tools, "protocol": 1}}
        elif request["tool"] == "vw_run_test":
            body = {"ok": True, "result": {"round": 1, "args": request["args"]}}
        else:
            body = {"ok": False, "error": "unknown"}
        body["id"] = request["id"]
        temp = os.path.join(spool, request["id"] + ".res.json.tmp")
        with open(temp, "w") as handle:
            json.dump(body, handle, ensure_ascii=False)
        os.replace(temp, os.path.join(spool, request["id"] + ".res.json"))
    time.sleep(0.1)
'''


def check_launch_on_demand(root):
    """**vw_run_test は Vectorworks が居なければ起こしてから頼む**（M40）。"""
    if os.name == "nt":
        return  # check_launch と同じ（代役を直に起こせない）
    spool = os.path.join(root, "on-demand-mcp")
    app = os.path.join(root, "serving-vectorworks")
    write_fake_app(app, spool, body=SERVING_APP % {"python": sys.executable, "spool": spool})

    # 図面を読むだけの道具では起こさない（橋が落ちた理由を覆い隠さない）。
    replies = drive(spool, [call("vw_ping", request_id=1)], timeout="1",
                    extra_env={"VW_MCP_APP": app})
    check(replies[0]["result"]["isError"] is True, "読む道具では起こさない")
    check(not os.path.exists(os.path.join(spool, "bridge.json")), "起こしていない")

    # 実機テストは起こしてから頼む。引数はそのまま届き、起こしたことも返る。
    args = {"ifc": "/repo/tests/fixtures/a.ifc", "template": "/repo/tests/fixtures/Default.sta"}
    replies = drive(spool, [call("vw_run_test", args, request_id=1)], timeout="1",
                    extra_env={"VW_MCP_APP": app})
    check(replies[0]["result"]["isError"] is False, "起こしてから実機テストを頼める")
    result = json.loads(content_text(replies[0]))
    check_eq(result["args"], args, "引数がそのまま届く")
    check(result["launched"]["launched"] is True, "起こしたことを返す")

    with open(os.path.join(root, "stop-serving-app"), "w", encoding="utf-8"):
        pass
    time.sleep(0.5)
    # 起こしても橋が架からないときの諦め方は vw_launch と同じ（check_launch）。待つ上限が
    # 既定の 120 秒なので、ここでは繰り返さない。


def load_server_module():
    """サーバを module として読み込む（中の関数を直に試すため）。"""
    import importlib.util

    spec = importlib.util.spec_from_file_location("vw_mcp_server", SERVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_spool_search(module, root):
    """**場所の探し方**を直に試す（本番で失敗したのはここ）。

    実機では、Claude のデスクトップアプリが $TMPDIR の無い環境でサーバを起動するため
    候補が /tmp だけになり、Vectorworks が /var/folders/…/T/ に置いた橋を見つけられなかった
    （DEV-NOTES M24）。以降そうならないよう、**利用者ごとの一時ディレクトリが候補に
    入ること**を確かめる。
    """
    if sys.platform == "darwin":
        user_temp = module.darwin_user_temp_dir()
        check(
            user_temp.startswith("/var/folders/"),
            "利用者ごとの一時ディレクトリを引ける (%r)" % user_temp,
        )
        env = dict(os.environ)
        for name in ("VW_MCP_SPOOL", "TMPDIR", "TMP", "TEMP"):
            env.pop(name, None)
        saved = os.environ.copy()
        os.environ.clear()
        os.environ.update(env)
        try:
            candidates = module.spool_candidates()
        finally:
            os.environ.clear()
            os.environ.update(saved)
        check(
            any(c.startswith(user_temp.rstrip("/")) for c in candidates),
            "$TMPDIR が無くても利用者ごとの場所を候補に入れる (%r)" % candidates,
        )
        # **推測の候補を持たない。** /tmp は Vectorworks の一時ディレクトリになり得ず、
        # 同じ計算機の誰でも書ける場所なので、利用者ごとの場所が取得できたなら候補に残さない。
        check(
            not any(c.startswith("/tmp/") for c in candidates),
            "/tmp は候補にしない (%r)" % candidates,
        )
    else:
        check_eq(module.darwin_user_temp_dir(), "", "macOS 以外では引かない")

    # **持ち主と権限を確認する。** /tmp は誰でも書けるので、偽の印を置かれても使わない。
    if os.name != "nt":
        mine = os.path.join(root, "mine-mcp")
        os.makedirs(mine, exist_ok=True)
        os.chmod(mine, 0o700)
        check(module.spool_is_safe(mine), "自分の 0700 のスプールは使う")
        os.chmod(mine, 0o777)
        check(not module.spool_is_safe(mine), "他から書けるスプールは使わない")
        os.chmod(mine, 0o700)
        check(not module.spool_is_safe(os.path.join(root, "no-such-mcp")), "無い場所は使わない")

        # **長く走る道具の最中は、印が古びていても生きている**（busy_until。M38）。
        now = int(time.time())
        def write_status(payload):
            with open(os.path.join(mine, "bridge.json"), "w", encoding="utf-8") as handle:
                json.dump(payload, handle)
        write_status({"protocol": 1, "beat": now - 100})
        check(module.Bridge._read_status(mine) is None, "古びた印は動いていない")
        write_status({"protocol": 1, "beat": now - 100, "busy": "vw_run_test",
                      "busy_until": now + 600})
        check(module.Bridge._read_status(mine) is not None, "busy_until が未来なら生きている")
        write_status({"protocol": 1, "beat": now - 100, "busy": "vw_run_test",
                      "busy_until": now - 1})
        check(module.Bridge._read_status(mine) is None, "busy_until を過ぎたら動いていない")
        os.remove(os.path.join(mine, "bridge.json"))


LOCAL_NAMES = ["vw_bridge_status", "vw_launch", "vw_call", "vw_lock_status", "vw_lock_release"]


class Session:
    """対話的に動かすサーバ 1 つ（＝Claude のセッション 1 つ）。占有の確認に使う。"""

    def __init__(self, spool, label, extra_env=None):
        env = dict(os.environ)
        env["VW_MCP_SPOOL"] = spool
        env["VW_MCP_TIMEOUT"] = "5"
        env["VW_MCP_LEASE"] = lease_base_for(spool)
        env["VW_MCP_SESSION_LABEL"] = label
        env.update(extra_env or {})
        self.proc = subprocess.Popen(
            [sys.executable, SERVER],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        self.next_id = 0
        self.notes = []

    def request(self, message):
        self.next_id += 1
        message = dict(message, id=self.next_id)
        self.proc.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("サーバが終了しました")
            reply = json.loads(line.decode("utf-8"))
            if reply.get("id") == self.next_id:
                return reply
            self.notes.append(reply)

    def call(self, name, arguments=None, meta=None):
        params = {"name": name, "arguments": arguments or {}}
        if meta is not None:
            params["_meta"] = meta
        reply = self.request({"jsonrpc": "2.0", "method": "tools/call", "params": params})
        return reply["result"]["isError"], content_text(reply)

    def close(self):
        """stdin を閉じる（Claude Code がセッションを閉じたのと同じ）。"""
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def read_lease(spool):
    with open(lease_base_for(spool) + ".lease.json", "r", encoding="utf-8") as handle:
        return json.load(handle)


def check_lease(root):
    """**複数のセッションから同時に呼ばれても競合しない**（M42）。

    Vectorworks は 1 つしか起動できず、サーバはセッションごとに 1 つ起動する。占有して
    いるセッションだけがブリッジへ要求を送れ、他は要求を書かずに断られ、状況を購読できる。
    """
    spool = os.path.join(root, "lease-mcp")
    os.makedirs(spool)
    os.chmod(spool, 0o700)
    fake = FakeVectorworks(spool)
    fake.start()
    time.sleep(0.3)
    a = Session(spool, "session-A")
    b = Session(spool, "session-B")
    try:
        is_error, _ = a.call("vw_ping")
        check(is_error is False, "最初に呼んだセッションが占有して届く")
        fake.seen = []

        # 他が占有していれば、スプールへ書かずに断る。誰が使っているかを返す。
        is_error, text = b.call("vw_layers")
        check(is_error is True, "他のセッションが占有している間は断る")
        blocked = json.loads(text)
        check(blocked.get("blocked") is True, "断った理由が占有だと分かる")
        check_eq(blocked["lock"]["holder"]["session"], "session-A", "誰が使っているかを返す")
        check("vw_lock_status" in blocked["hint"], "購読の道具を案内する")
        time.sleep(0.2)
        check_eq(fake.seen, [], "断った要求はブリッジへ届かない")

        # 占有していなくても状況は読める。
        is_error, text = b.call("vw_lock_status")
        view = json.loads(text)
        check(is_error is False and view["state"] == "held", "状況は誰でも読める")
        is_error, text = a.call("vw_lock_status")
        check_eq(json.loads(text)["state"], "yours", "持ち主には yours と見える")
        is_error, text = b.call("vw_lock_release")
        check(json.loads(text)["released"] is False, "他の占有は解けない")

        # 購読: 解放されるまで待ち、その間の出来事を受け取る（progress 通知も）。
        released = {}

        def release_later():
            time.sleep(1.5)
            a.call("vw_layers")
            released["reply"] = a.call("vw_lock_release")

        releaser = threading.Thread(target=release_later)
        releaser.start()
        started = time.time()
        is_error, text = b.call("vw_lock_status", {"wait": 20}, meta={"progressToken": "t1"})
        releaser.join(timeout=10)
        waited = json.loads(text)
        check_eq(waited["state"], "free", "解放されるまで待って戻る")
        check(time.time() - started < 15, "解放されたらすぐ戻る")
        events = [o["event"] for o in waited["observed"]]
        check(
            "vw_layers を始めた" in events and any("占有を解いた" in e for e in events),
            "待っている間の出来事を返す (%r)" % events,
        )
        progress = [n for n in b.notes if n.get("method") == "notifications/progress"]
        check(len(progress) >= 2, "出来事を progress 通知でも送る (%d)" % len(progress))
        check(all(n["params"]["progressToken"] == "t1" for n in progress), "progressToken を返す")

        # 解放するときは片付ける（実機テストの図面・一時ファイル・記憶・報告）。
        is_error, text = released["reply"]
        result = json.loads(text)
        check(result["released"] is True, "vw_lock_release で解放する")
        check_eq(result["cleanup"]["done"], True, "解放する前に片付ける")
        check_eq(fake.cleanups, 1, "片付けはプラグインの vw_test_cleanup に頼む")
        check(
            any("vw_test_cleanup" in e for e in events),
            "片付けたことも購読している側へ届く (%r)" % events,
        )
        check("cleanup_pending" not in read_lease(spool), "片付けたら引き継ぎを残さない")

        # 解放されれば他のセッションが使える。
        is_error, _ = b.call("vw_ping")
        check(is_error is False, "解放されたら他のセッションが占有できる")

        # 終了したら片付けて解く（stdin が閉じた＝Claude Code がセッションを閉じた）。
        b.close()
        check(read_lease(spool)["holder"] is None, "セッションを終了したら占有を解く")
        check_eq(fake.cleanups, 2, "セッションを終了するときも片付ける")

        # 最後の操作から一定時間で解く（相手が手を止めたまま）。
        is_error, _ = a.call("vw_ping")
        c = Session(spool, "session-C", extra_env={"VW_MCP_LEASE_IDLE": "1"})
        try:
            time.sleep(1.5)
            is_error, text = c.call("vw_ping")
            check(is_error is False, "最後の操作から一定時間で解く")
            events = [e["event"] for e in read_lease(spool)["events"]]
            check(any("最後の操作から" in e for e in events), "解いた理由を記録する")
            # 時間切れで解かれた持ち主は片付けていない。次に占有したほうが自分の操作の前に片付ける。
            check_eq(fake.cleanups, 3, "時間切れの持ち主の片付けを引き継ぐ")
            # 一覧の取得（vw_tools）は占有と無関係に挟まるので除いて比べる。
            sent = [tool for tool in fake.seen if tool != "vw_tools"]
            check_eq(sent[-2:], ["vw_test_cleanup", "vw_ping"], "片付けてから自分の操作を送る")
            reply = c.request({"jsonrpc": "2.0", "method": "tools/call",
                               "params": {"name": "vw_lock_status", "arguments": {}}})
            check("cleanup_pending" not in json.loads(content_text(reply)), "引き継ぎは 1 度だけ")
        finally:
            c.close()

        # 持ち主が異常終了したら（後始末できなかったら）、他から解ける。
        if os.name != "nt":
            is_error, _ = a.call("vw_ping")
            check(is_error is False, "持ち主として占有し直す")
            a.proc.kill()
            a.proc.wait(timeout=10)
            d = Session(spool, "session-D")
            try:
                before = fake.cleanups
                is_error, _ = d.call("vw_ping")
                check(is_error is False, "持ち主のプロセスが消えていれば解く")
                check_eq(fake.cleanups, before + 1, "異常終了した持ち主の片付けを引き継ぐ")
            finally:
                d.close()

        # Vectorworks が居なければ片付けられない。解放はして、片付けは次へ引き継ぐ。
        fake.stop_flag.set()
        fake.join(timeout=5)
        e = Session(spool, "session-E", extra_env={"VW_MCP_TIMEOUT": "1"})
        try:
            e.call("vw_ping")  # 届かないが、占有は取る
            is_error, text = e.call("vw_lock_release")
            result = json.loads(text)
            check(result["released"] is True, "Vectorworks が居なくても解放する")
            check_eq(result["cleanup"]["pending"], True, "片付けられなかったと言う")
            check_eq(read_lease(spool).get("cleanup_pending"), "session-E", "片付けを引き継ぐ印を残す")
        finally:
            e.close()
    finally:
        for session in (a, b):
            if session.proc.poll() is None:
                session.proc.kill()
        fake.stop_flag.set()
        fake.join(timeout=5)


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
        # --- 場所の探し方（本番で失敗したところ）------------------------
        check_spool_search(load_server_module(), root)

        # --- Vectorworks を起こす ----------------------------------------
        check_launch(root)
        check_launch_on_demand(root)

        # --- 複数のセッション（M42）--------------------------------------
        check_lease(root)

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
            names == LOCAL_NAMES,
            "ブリッジが無いときの一覧は自前の道具だけ (%r)" % names,
        )
        status = json.loads(content_text(replies[2]))
        check(status["running"] is False, "vw_bridge_status が「動いていない」と答える")
        check("vw_launch" in status["hint"], "起動の道具を案内する")
        check("MCP ブリッジを表示" in status["hint"], "どうすれば動くかを案内する")
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
            LOCAL_NAMES + ["vw_ping", "vw_layers"],
            "**一覧の真実はプラグイン側**（代役が返した 2 つが並ぶ）",
        )
        ping = json.loads(content_text(replies[2]))
        check_eq(ping["plugin"], "min-nano_structureDev", "vw_ping の結果が素通しで返る")
        check(
            all("timeoutSeconds" not in t for t in replies[1]["result"]["tools"]),
            "プラグインの表の待ち時間は Claude へ見せない",
        )
        layers = json.loads(content_text(replies[3]))
        check_eq(layers["count"], 1, "vw_layers の結果が素通しで返る")
        check(replies[4]["result"]["isError"] is True, "知らない道具はエラーとして返る")
        check(
            "知らない道具です" in content_text(replies[4]),
            "プラグイン側のエラー文がそのまま見える",
        )
        check_eq(replies[5]["result"], {}, "ping に空で答える")

        # 送った順に処理されている（要求ファイル名の連番が機能している）。片付け
        # （vw_test_cleanup）は占有の出入りで挟まる（M42。check_lease）ので、ここでは除く。
        check_eq(
            [tool for tool in fake.seen if tool != "vw_test_cleanup"],
            ["vw_tools", "vw_ping", "vw_layers", "vw_nope"],
            "送った順に処理される",
        )
        check_eq(fake.seen[-1], "vw_test_cleanup", "終了するときは片付けてから占有を解く")

        # --- 一覧が古いまま（アプリを Vectorworks より先に起動した）------------
        # **実機で起きたのはここ。** アプリは tools/list を起動時に 1 回しか呼ばず、その
        # ときブリッジが居なければ、図面を読む道具が最後まで見えない（M30）。そのサーバが
        # 橋の架かったあとに初めてアクセスする場面を再現する: tools/list を挟まずに
        # vw_bridge_status を呼ぶ（＝まだプラグインの道具を 1 つも見せていない）。
        # 取り直しは 1 度だけ促し、一覧に頼らない vw_call でも道具に届くこと。
        replies, notes = drive(
            spool,
            [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                call("vw_bridge_status", request_id=2),
                call("vw_bridge_status", request_id=3),
                call("vw_call", {"tool": "vw_layers"}, request_id=4),
                call("vw_call", {"tool": "vw_bridge_status"}, request_id=5),
                call("vw_call", {}, request_id=6),
            ],
            with_notes=True,
        )
        status = json.loads(content_text(replies[1]))
        check(status["running"] is True, "橋が架かれば vw_bridge_status が動いていると答える")
        check_eq(
            [t["name"] for t in status.get("tools", [])],
            ["vw_ping", "vw_layers"],
            "vw_bridge_status がプラグインの道具を並べる",
        )
        check_eq(
            notes,
            ["notifications/tools/list_changed"],
            "一覧が古ければ取り直しを 1 度だけ促す",
        )
        layers = json.loads(content_text(replies[3]))
        check_eq(layers.get("count"), 1, "vw_call でプラグインの道具に届く")
        check(
            json.loads(content_text(replies[4]))["running"] is True,
            "vw_call で自前の道具も呼べる",
        )
        check(replies[5]["result"]["isError"] is True, "tool の無い vw_call はエラー")

        # --- 再起動の完了を確認する（M38）-------------------------------
        started = time.time()
        replies = drive(spool, [call("vw_restart", request_id=1)])
        result = json.loads(content_text(replies[0]))
        check(replies[0]["result"]["isError"] is False, "再起動して橋が架かり直せば成功")
        check(result.get("requested") is True, "プラグインの応答（頼んだ）を素通しで返す")
        check(
            result.get("after_restart", {}).get("restarted") is True,
            "一度居なくなって、また架かるのを見届ける",
        )
        check(time.time() - started >= 2.0, "居なくなっている間は待つ")

        # --- 応答が返らないとき -----------------------------------------
        fake.seen = []
        started = time.time()
        replies = drive(spool, [call("vw_slow", request_id=1)], timeout="1")
        elapsed = time.time() - started
        check(replies[0]["result"]["isError"] is True, "応答が無ければエラーで返る")
        check("応答がありません" in content_text(replies[0]), "待ち切れなかったと言う")
        check(elapsed < 30, "待ち時間の上限で諦める（%.1f 秒）" % elapsed)
        # 置きっぱなしの要求は引き上げてある（次のセッションが処理しないように）。
        leftovers = [n for n in os.listdir(spool) if n.endswith(".req.json")]
        check_eq(leftovers, [], "諦めた要求はスプールに残さない")

        # ここから先は代役を建て直すので、先に止める（走ったままディレクトリの名前を
        # 変えると、代役が印を書けずに異常終了する）。
        fake.stop_flag.set()
        fake.join(timeout=5)

        # --- 場所を自力で探し当てる -------------------------------------
        # **本番はこの経路。** プラグイン側は自分の一時ディレクトリへ置き、こちらは
        # 候補を順に確認して生きた印のある場所を使う（両側で $TMPDIR が食い違いうるため）。
        # スプールは <一時ディレクトリ>/min-nano_structureDev-mcp でなければならない
        # （VW_MCP_PLUGIN を渡さないので、既定＝開発版の名前で探す）。
        found = os.path.join(root, "min-nano_structureDev-mcp")
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
            any(c.endswith("min-nano_structureDev-mcp") for c in status["searched"]),
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
