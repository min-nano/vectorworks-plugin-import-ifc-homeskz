# MCP ブリッジ（開発版: ローカルの Claude Code から実機確認を回す）

（利用者向けのページの一覧は [`README.md`](../../README.md#使い方とドキュメント)）

> **開発版（みんなの構造設計支援Dev）だけの機能です。** 安定版にはメニューもパレットも
> ありません。取り込みだけを使う方は読み飛ばして構いません。

**Vectorworks が動いている間、同じ PC で動く Claude Code が、開いている図面の中身・取り込みの
診断ログ・実機テストの報告を自分で読み、頼まれれば新しい開発版ビルドを入れ、Vectorworks を
再起動し、実機テストを走らせられる**ようになります。プラグインを直す → ビルドを入れる →
取り込み直す → ログを貼る、という往復を、PR のコメントを介さずに Claude が回すための道具です。
**図面を触ったまま**使えます。Vectorworks が起動していなければ、Claude から起動することも
できます（`vw_launch`）。

```
Claude Code ──MCP──▶ vw-mcp-server.py ──ファイル──▶ Vectorworks（MCP ブリッジのパレット）
```

## 用意する

1. **開発版のプラグインを入れる**（[「インストール」](install.md)。チャンネルは dev）。
2. **このリポジトリで Claude Code を起動する。** リポジトリ直下の `.mcp.json` が
   `scripts/mcp/vw-mcp-server.py` を登録しているので、ほかに設定は要りません（初回だけ
   Claude Code がこのサーバを使ってよいか尋ねます）。Python 3.8 以降が要ります
   （macOS には最初から入っています。Windows で `python3` が無いときは `.mcp.json` の
   `command` を `py` や `python` に読み替えてください）。

**リポジトリを持たない PC から使うとき**は、インストール先のフォルダ
（`Plug-Ins/min-nano_structureDev/`）に置かれた `vw-mcp-server.py` を登録します。

```bash
# macOS
claude mcp add vectorworks -- python3 "<プラグインのフォルダ>/vw-mcp-server.py"
```

```powershell
# Windows
claude mcp add vectorworks -- python "<プラグインのフォルダ>\vw-mcp-server.py"
```

Claude のデスクトップアプリへ登録するときは `claude_desktop_config.json`
（macOS は `~/Library/Application Support/Claude/`、Windows は `%APPDATA%\Claude\`）の
`mcpServers` に足します。

```jsonc
{
  "mcpServers": {
    "vectorworks": {
      "type": "stdio",
      "command": "/usr/bin/python3",
      "args": ["/Users/<ユーザー名>/Library/Application Support/Vectorworks/2026/Plug-Ins/min-nano_structureDev/vw-mcp-server.py"],
      "env": {}
    }
  }
}
```

- **`command` は Python の絶対パスにしてください**（アプリの `PATH` は端末とは別物です）。
- **`args` はただの JSON で、シェルを通りません。** `~` は展開されず、空白を `\` で
  エスケープすると `\` が名前の一部になります。Finder で `vw-mcp-server.py` を右クリックし
  Option キーを押すと出る「パス名をコピー」を貼るのが確実です。
- 失敗しているときは、ログ（macOS なら `~/Library/Logs/Claude/mcp-server-vectorworks.log`）に
  Python が実際に開こうとしたパスが出ます。書き換えたらアプリを開き直してください。

## 使う

1. Vectorworks のメニューの **「MCP ブリッジを表示…」** を実行します。
   **MCP ブリッジ**のパレットが出て「受け付けています」と表示されたら、橋が架かっています。
   **以後は Vectorworks を終えるまで受け付けます**——パレットは閉じても構いません。
   **パレットは Vectorworks を起動し直しても開いたまま**なので、メニューを押すのは最初の
   1 回だけです。
2. **実機テストの 1 周目だけは人が行います。** 試したい図面を開き、メニューの
   **「実機テストを実行…」** で IFC と取り込み設定を選びます（いま開いている図面が作業
   ファイルとして保存され、以後の周の基準になります）。
3. あとは Claude Code に頼みます——「直して push して、新しいビルドで実機テストを回して」。
   Claude は CI を待ち、`vw_update` でビルドを入れ、`vw_run_test` で同じ条件のまま図面を
   戻して取り込み直し、報告を読みます。**殻まで変わったビルドは再起動が要り**、Claude は
   `vw_restart` の前に一言断ります（未保存の図面があれば Vectorworks の保存の確認が出るので、
   応えてください）。
4. **絵を見て気付いたことは Claude とのチャットへ書いてください。** 報告にあるのは数字と
   診断ログだけで、絵が正しいかは人にしか分かりません。

**Claude の道具の一覧に図面を読む道具（`vw_layers` など）が見えないとき**は、Claude を
Vectorworks より先に起動したためです（道具の一覧は起動したときに 1 回しか取りに行きません）。
そのままでも `vw_call` を通せば同じ道具を呼べます——Claude に「`vw_bridge_status` で道具を
確かめて、`vw_call` で呼んで」と頼んでください。

| 道具 | 何をするか |
| --- | --- |
| `vw_bridge_status` | ブリッジが動いているか（動いていれば呼べる道具の一覧、動いていなければどうすれば動くか） |
| `vw_launch` | Vectorworks を起動し、ブリッジが受け付けるまで待つ（既に受け付けていれば何もしない） |
| `vw_call` | 下の道具を名前で呼ぶ（道具の一覧に見えていないとき用。例: `{"tool": "vw_layers"}`） |
| `vw_ping` | プラグインのビルドと、開いている図面のカレントレイヤ |
| `vw_layers` / `vw_classes` | レイヤ一覧・クラス名の一覧 |
| `vw_layer_objects` / `vw_object_counts` | 指定したレイヤの中身・種別番号ごとの数 |
| `vw_log` | 直近の取り込み（本番か実機テスト）の診断ログ |
| `vw_test_report` | 直近の実機テストの報告（要素の内訳・前の周からの変化・図面の状態） |
| `vw_run_test` | 実機テストを 1 周走らせる（前の周と同じ条件で。ダイアログは出さない） |
| `vw_update` | 開発版の新しいビルドを入れる（尋ねない。別のブランチも名指しできる） |
| `vw_restart` | Vectorworks を再起動する（保存の確認は通常どおり出る） |

## 制限（承知のうえで使ってください）

- **取り込みなどの最中は受け付けを見送ります**（パレットに「一時的に見送っています」と
  出ます）。描きかけの図面を読ませないためで、終われば戻ります。`vw_run_test` を頼まれた
  ときは、その 1 周が終わってから応えます。
- **取り込み・更新・再起動は Claude が頼んだときにしか起きません。** パレットが勝手に
  ビルドを入れたり取り込んだりすることはありません。
- **`vw_launch` が起動するのは Vectorworks 2026 の標準のインストール先**です（macOS は
  「Vectorworks 2026」というアプリ、Windows は `C:\Program Files\Vectorworks 2026\` の
  中の実行ファイル）。別の場所に入れている場合は、環境変数
  `VW_MCP_APP`（`.mcp.json` やデスクトップアプリの設定の `env`）に
  `<Vectorworks のアプリか .exe のパス>` を書いてください。Windows では既に Vectorworks が
  動いていれば 2 つ目は起動しません。
- **やり取りはこの PC の中だけで完結します。** 一時フォルダの
  `min-nano_structureDev-mcp` に置いたファイルを介してつないでいるので、ネットワークは
  使いません（ファイアウォールの許可も要りません）。置き場所は自動的に見つけます。
- **使えるのは、この PC で動いている Claude だけです。** ブラウザの Claude や
  claude.ai/code のリモートセッション（クラウド上のコンテナで動くもの）は、
  **別の計算機なのでこのファイルに触れません**。iPhone などから使いたいときは、この PC で
  `claude remote-control` を起動し、Claude アプリからそのセッションを操作してください
  （PC はスリープさせない）。
- 種別番号（`vw_layer_objects` の `type`）は Vectorworks の内部の番号です。名前が分かって
  いるものだけ `type_name` を添えていますが、多くは番号のままです。
- **繋がらないときは、まず Claude に `vw_bridge_status` を実行させてください。**
  ブリッジが動いていなければ、探した場所の一覧と対処が返ります。
- **パレットが「受け付けています」なのに「動いていない」と返るとき**は、探した場所を見て
  ください。急ぐときは、ターミナルで `getconf DARWIN_USER_TEMP_DIR` を実行して出た場所を使い、
  環境変数 `VW_MCP_SPOOL` に `<その場所>min-nano_structureDev-mcp` を渡しても繋がります。
  場所を総当たりで探すことはしません——曖昧に繋がるより、はっきり繋がらないほうが原因を
  追えるためです。
