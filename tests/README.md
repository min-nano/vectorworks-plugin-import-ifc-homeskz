# テスト方針

このプラグインのテストは **Vectorworks SDK を必要としない** ことを最優先に設計して
います。SDK（約 800 MB）をダウンロードせずに、標準の Linux ランナーで速く回せるので、フォークから
の PR でもカバレッジまで含めて CI が通ります（`.github/workflows/test.yml`）。

## 何をテストしているか

テストは次の領域に分かれます。軸は**インポート機能（2 フェーズ）のテスト**と、テンプレート
由来の**アップデータのテスト**の 2 系統で、そこへ殻と本体の境界・開発ツール・MCP サーバの
テストが加わります。どれも SDK を要求しないので、同じ Linux ランナーでまとめて走ります。

| 領域 | 中身 |
| --- | --- |
| [インポート機能（`core/` + `parse/`）](../docs/development/testing/import.md) | 要素ごとの単体テストの対応表・描画側と MCP ブリッジの切り分け |
| [アップデータ（テンプレート由来）](../docs/development/testing/updater.md) | `UpdaterParse` / `UpdaterFlow` / `UpdaterRobustness` / スクリプト・インストーラ・アンインストーラのテスト、判断とグルーの切り分け、フロー全体のテスト（`IUpdaterHost`）、それでも残る部分 |
| [スクリプトのテスト（ソース＋スタブ方式）](../docs/development/testing/updater-scripts.md) | アップデータの続き。`vw-update` / `vw-install` / `vw-uninstall` の `.sh` / `.ps1` を SDK・ネットワーク抜きで |
| [殻と本体の境界（ホットリロード）](../docs/development/testing/payload-boundary.md) | 本体を探すパス・殻の記憶域を持ち続けないこと |
| [開発ツール](../docs/development/testing/dev-tools.md) | CI 完了待ち（`CiWaitScriptTests`）・clang-tidy の結果キャッシュ（`TidyCacheScriptTests`） |
| [MCP サーバ（Claude 側）](../docs/development/testing/mcp-server.md) | `vw-mcp-server.py` を代役のスプールに対して |

## テストの実行

ビルド／実行のコマンド・ビルドオプション・CI の `test` ジョブの設定は開発ガイドの
[「テストの実行」](../docs/development/testing/running.md)にあります。テスト自体は依存ゼロの
小さなハーネス（`TestFramework.h`）で書き、`TEST(name){ … }` の中で `CHECK` / `CHECK_EQ` を
使って、`TEST_MAIN()` を 1 か所だけ置きます。

## まとめ

- **インポートの解析側**（`core/` + `parse/`）は実フィクスチャに対して要素ごとに単体
  テストする。**描画側**（`draw/`）は切り出せるロジックを `core/` へ寄せたうえで、残りは
  実機での目視確認に委ねる（[インポート機能](../docs/development/testing/import.md)）。
- **アップデータ**は、判断を `UpdaterParse.h` の純粋関数に寄せて関数単位で、フローを
  `IUpdaterHost` のフェイク越しに分岐と文言まで、同梱スクリプトをソース＋スタブ方式で
  Linux 上で、それぞれテストする（[アップデータ](../docs/development/testing/updater.md)・
  [スクリプトのテスト](../docs/development/testing/updater-scripts.md)。どれも e2e ではない）。
- 残るのは SDK 関数を呼ぶだけの薄い配線と、Mac／Windows 実機でしか動かない OS 固有
  ツールの実行だけで、ここは費用対効果から e2e ／手動に委ねる
  （[「それでも残る部分」](../docs/development/testing/updater.md#それでも残る部分アップデータ)）。
