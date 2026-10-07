# みんなの構造設計支援（Vectorworks 2026 プラグイン）

**構造設計に使う機能をまとめて収める** Vectorworks 2026 用のネイティブプラグイン
（C++ SDK 製）です。プラグイン 1 つに複数のコマンドと PIO を同梱できるので、機能ごとに
プラグインを分けず、この 1 つへ追加していきます。

いま入っている機能は次のとおりです。

| 何 | どこから |
| --- | --- |
| **IFC (ホームズ君) 取り込み…** — ホームズ君構造EX の IFC を Vectorworks のネイティブオブジェクトへ変換して配置するコマンド | メニュー |
| **アップデータを確認 (みんなの構造設計支援)** — 新しいビルドが出ていないか確かめ、あれば入れ替えるコマンド | メニュー |
| **MCP ブリッジを表示… (Dev)** — ローカルの Claude Code と開いている図面をつなぐパレットを出すコマンド。以後は Vectorworks が動いている間、Claude が図面・ログ・実機テストの報告を読み出し、要求に応じて更新・再起動・実機テストを実行できる（**開発版だけ**。[「MCP ブリッジ」](docs/user-guide/mcp-bridge.md)） | メニュー |
| **実機テストを実行… (みんなの構造設計支援Dev)** — 記憶した条件で、テンプレートから開いた新しい図面へ再度取り込み、結果をローカルに記録するコマンド（**開発版だけ**。開発ガイドの[「実機テスト」](docs/development/live-test/README.md)） | メニュー |
| **柱記号** / **耐力壁** — 取り込みで配置される 2 つのプラグインオブジェクト（PIO） | 図面上のオブジェクト |

## IFC (ホームズ君) 取り込み

**ホームズ君構造EX** が出力する木造軸組工法建築物の IFC ファイル（IFC2x3 Coordination
View 2.0）を読み込み、Vectorworks 2026 の**ネイティブオブジェクト**（ストーリ・構造材・壁・
スラブ・屋根面・シンボル・ビューポート）へ変換して図面に配置します。

メニューコマンドを 1 回実行するだけで、

- 階（ストーリ）とデザインレイヤ、通り芯、
- 基礎（立上り・底盤・人通口・地中梁）、床、横架材、柱、屋根組（垂木・野地板）、
- アンカーボルト・床束・火打・仕口・継手のシンボル、柱・小屋束の記号、耐力壁（筋かい・面材）、
- **伏図**（基礎伏図・横架材の高さごとの柱梁伏図・母屋伏図）と**軸組図**のシート

までが一度に作成されます。取り込んだものは編集可能なネイティブオブジェクトなので、
そのまま Vectorworks 上で修正・出図できます。

**macOS と Windows の両方**に対応します（Vectorworks 2026 が対応する 2 プラットフォーム）。

## 使い方とドキュメント

利用者向けの説明は、トピックごとに [`docs/user-guide/`](docs/user-guide/) に分けてあります。

| ページ | 中身 |
| --- | --- |
| [取り込むもの](docs/user-guide/import-contents.md) | IFC 側の要素と図面にできるものの対応・配置先レイヤ・できあがる図面の構造（レイヤ・クラス・構造材の端点・シートレイヤ・縮尺・図面枠・寸法） |
| [使い方](docs/user-guide/usage.md) | 1. 図面側で用意しておくもの／2. 実行する（設定ダイアログ・軸組図にする通り）／3. 取り消す／4. うまくいかないとき／5. 困ったときは（ログをコピーして貼る） |
| [インストール](docs/user-guide/install.md) | stable と dev・置き場所・2 つのファイルで 1 つのプラグイン・インストーラ・macOS / Windows の手作業の手順 |
| [アップデート](docs/user-guide/update.md) | 「アップデータを確認」・取り込み時の確認・ふつうは再起動が要らないこと |
| [MCP ブリッジ（開発版）](docs/user-guide/mcp-bridge.md) | ローカルの Claude Code から実機確認を回す——用意する・使う・制限 |
| [既知の制限](docs/user-guide/known-limitations.md) | 対応しない IFC・描画の制約・手作業で修正するところ |

---

## 開発者向け

- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — **開発ガイド**の目次。ソースの構成、ビルド
  （`VW_SDK_DIR`）、テストとカバレッジ、lint、CI とリリース、SDK ドキュメントの在り処、
  MCP ブリッジと実機テスト、自動アップデートの仕組み（本文は `docs/development/`）。
- [`docs/DEV-NOTES.md`](docs/DEV-NOTES.md) — **開発メモ**の目次。設計の考え方、ホームズ君
  IFC の癖、打ち切った調査（本プラグインの方針）、実装の経緯（本文は `docs/dev-notes/`）。
- [SDK リファレンス（別リポジトリ）](https://github.com/min-nano/vectorworks-developer-sdk-reference)
  — **Vectorworks SDK の実測知見**（実機でしか判明しなかった落とし穴・SDK に無い／
  機能しない API）。公式リファレンスのフォークに
  [`Findings/`](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/README.md)
  と調査用 CI を追加したもの。
- [`tests/README.md`](tests/README.md) — テストの一覧・方針・何をテストしていないか（領域ごとの
  ページは `docs/development/testing/`）。
- [`CLAUDE.md`](CLAUDE.md) — このリポジトリで作業するときの規約。

処理は **IFC 解析フェーズ（`src/parse/`）** と **VW 描画フェーズ（`src/draw/`）** に完全
分離し、両者は命令セット（`src/core/Document.h`）だけで接続します。`parse/` と `core/` は
**SDK を一切 include しない**ので、SDK 無しでコンパイル・単体テストできます。

```
IFC ファイル
   │  Phase 1: parse （SDK 非依存）
   ▼
Document（命令セット。プレーンな構造体の集まり）
   │  Phase 2: draw （VectorWorks SDK のみに依存）
   ▼
VectorWorks ネイティブオブジェクト
```
