# MCP ブリッジ（`core/Bridge` ＋ `draw/McpBridge` ＋ `scripts/mcp/vw-mcp-server.py`）

利用者から見た使い方は[「MCP ブリッジ」](../user-guide/mcp-bridge.md)、経緯は
[M24](../dev-notes/milestones/m24-mcp-bridge.md) / [M30](../dev-notes/milestones/m30-mcp-resident.md) / [M38](../dev-notes/milestones/m38-local-mcp-verification.md)。ここは変えるときの決めごとです。

**開発版だけの道具です（M38）。** メニュー「MCP ブリッジを表示…」とパレットは開発版
（`min-nano_structureDev`）にだけ登録し、安定版はクラスを持つだけで登録しません
（`src/ModuleMain.cpp`）。リポジトリ直下の `.mcp.json` が `scripts/mcp/vw-mcp-server.py` を
登録するので、**このリポジトリでローカルの Claude Code を起動すれば道具が使えます**
（Windows で `python3` が無いときは `.mcp.json` の `command` を `py` 等へ読み替えてください）。

## 道具

| 道具 | 種類 | 中身 |
| --- | --- | --- |
| `vw_bridge_status` / `vw_launch` / `vw_call` | Python | 橋の生死・Vectorworks の起動・一覧が古いときの汎用の呼び出し口（橋が無くても答える） |
| `vw_ping` / `vw_layers` / `vw_classes` / `vw_layer_objects` / `vw_object_counts` | 読む | 図面の素性・レイヤ・クラス・中身 |
| `vw_log` | 読む | 直近の取り込み（本番か実機テスト）の**診断ログ**。ファイルから読むので本体を入れ替えたあとも読める |
| `vw_test_report` | 読む | 直近の**実機テストの報告**（[「実機テスト」](live-test/README.md)） |
| `vw_run_test` | 長く走る | 実機テストを 1 周走らせて報告を返す（ダイアログを出さない。1 周目は人がメニューから） |
| `vw_update` | 殻に頼む | 開発版の新しいビルドを入れて本体を読み直す（`branch` で名指し・`restart_if_needed`） |
| `vw_restart` | 殻に頼む | Vectorworks を再起動する（保存の確認は通常どおり出る）。Python 側が架かり直すまで見届ける |

## 決めごと

- **受け渡しの作法**（要求／応答の形・スプールのファイル名・原子的な書き方・id の綴り検査）は
  **`core/Bridge.h` ただ 1 つ**で、Python サーバはその対になる綴りを持ちます。どちらかを
  変えたら両方を直してください（`tests/vw-mcp-server.test.py` が落ちます）。
- **道具の表**（名前・説明・引数の形・種類・待ち時間）は **`draw/McpBridge.cpp` の `kTools`
  ただ 1 つ**で、Python サーバは起動時に `vw_tools` でそれを取りに行きます。**道具を足すときに
  触るのはその 1 行と実装 1 つだけ**で、Python 側は直しません。待ち時間（`timeoutSeconds`）も
  表が持ち、Python はそれを読んでから Claude へ見せる前に落とします。JSON は `core/Json`
  （ブリッジ専用）。例外は**再起動の見届け**（`vw_restart` と `vw_update` の `restarting`）で、
  橋の向こうが一度居なくなるのを見られるのは Python 側だけなので、そこに持ちます。
- **受け付けは常駐のパレットの時計が 1 回ずつ呼びます**（M30。`draw::serveMcpBridge` は待たずに
  戻る）。**本体の中にループを書かない**——書けば図面がまた塞がります。本体のコードが
  スタックに載っている間（`PayloadInUse`）は見送ります。例外は `vw_run_test` で、その 1 周が
  終わるまで戻りません（走る前に生存の印へ `busy_until` を書き、Python はそれが未来のうちは
  印が古びても「生きている」と見ます）。
- **殻に頼む道具**（`vw_update` / `vw_restart`）は本体の中ではできません——本体は自分を
  降ろせない（`src/PayloadSession.h`）。本体は要求を引き取って見え方の `action` に載せて返し、
  **応えません**。殻（`Extensions/ExtMcpPalette.cpp`）が本体から戻ったあとで済ませ、結末を
  次の呼び出しの `shellReport` で本体へ渡し、**そのとき載っている本体**（入れ替えたなら新しい
  ほう）が応答を書きます（`src/PayloadAbi.h` の `VwPayloadMcpServeFn`。ABI 7）。応答には
  書いた本体の `payload_commit` が付くので、入れ替えが効いたかを Claude が確かめられます。
- **インストールの経路は 1 本のまま**です。`vw_update` が通るのは `src/UpdaterFlow.cpp` の
  `RemoteDevUpdateWith`（ダイアログを 1 枚も出さず結末を値で返す）で、手で押した確認と同じ
  `Install` を使います。
- **再起動は応答を書いてから頼みます**（先に頼むと応える者がいなくなる）。保存の確認を
  飛ばす道は持ちません（作業中の図面を失う）。殻まで変わって ABI が上がった直後は新しい本体を
  古い殻が読めず橋が落ちていることがあり、そのときの `vw_restart` は Python が macOS の作法で
  普通に終了させてから起こし直します（強制終了はしない）。
- **隠れていても受け付けます。** 相手は同じ計算機の同じ利用者の Claude だけで（スプールは
  0700）、取り込み・更新・再起動は**頼まれたときにしか起きません**——勝手に回る時計は持ち
  ません。M24 の往復が事故を起こしたのは、隠れたパレットが自分の判断で取り込みを回したから
  でした。**勝手に動く道具を足すなら、この判断をやり直してください。**
- **新しく図面を書く道具を足すなら**、undo の作法
  （[SDK リファレンス「Undo」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Undo.md)）を
  必ず通してください。`vw_run_test` が書くのは本番の取り込みと同じ経路（`draw/ImportRun`）だけです。
- **Vectorworks を起こすのは Python サーバの道具**（`vw_launch`）で、プラグイン側には書きません
  （起こす前にはプラグインが居ない）。

**実機で確かめたこと**（M38・PR #188）: パレットの時計の中から
`CloseAllFilesAndQuitVectorworks` を頼んだ再起動と、`vw_run_test` の 1 周を時計の中で走らせる
ことは、どちらも実機で効いた（[M38](../dev-notes/milestones/m38-local-mcp-verification.md)）。新しく分かったことは SDK の挙動
なら `Findings/`、本プラグインの話なら [M38](../dev-notes/milestones/m38-local-mcp-verification.md) へ足してください。
