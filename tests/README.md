# テスト方針

このプラグインのテストは **Vectorworks SDK を必要としない** ことを最優先に設計して
います。SDK（約 800 MB）を落とさずに、素の Linux ランナーで速く回せるので、フォークから
の PR でもカバレッジまで含めて CI が通ります（`.github/workflows/test.yml`）。

## 何をテストしているか

テストは 2 系統に分かれます。**インポート機能（2 フェーズ）のテスト**と、テンプレート
由来の**アップデータのテスト**です。どちらも SDK を要求しないので、同じ Linux ランナーで
まとめて走ります。

| | 領域 | 中身 |
| --- | --- | --- |
| 1 | [インポート機能（`core/` + `parse/`）](../docs/development/testing/import.md) | 要素ごとの単体テストの対応表・描画側と MCP ブリッジの切り分け |
| 2 | [アップデータ（テンプレート由来）](../docs/development/testing/updater.md) | `UpdaterParse` / `UpdaterFlow` / `UpdaterRobustness` / スクリプト・インストーラ・アンインストーラのテスト、フロー全体のテスト（`IUpdaterHost`）、それでも残る部分 |
| 2 | [スクリプトのテスト（ソース＋スタブ方式）](../docs/development/testing/updater-scripts.md) | `vw-update` / `vw-install` / `vw-uninstall` の `.sh` / `.ps1` を SDK・ネットワーク抜きで |
| 3 | [殻と本体の境界（ホットリロード）](../docs/development/testing/payload-boundary.md) | 本体を探すパス・殻の記憶域を持ち続けないこと |
| 4 | [開発ツール](../docs/development/testing/dev-tools.md) | CI 完了待ち（`CiWaitScriptTests`）・clang-tidy の結果キャッシュ（`TidyCacheScriptTests`） |
| 5 | [MCP サーバ（Claude 側）](../docs/development/testing/mcp-server.md) | `vw-mcp-server.py` を代役のスプールに対して |

## テストの実行

**ビルド／実行のコマンド例（素の実行・カバレッジ・サニタイザ）とビルドオプションの
一覧は開発ガイドの[「テストの実行」](../docs/development/testing/running.md)が単一の真実です。**
最短は次のとおり:

```sh
cmake -S . -B build-tests -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

`-DVW_ENABLE_SANITIZERS=ON` を付けると、各テストバイナリが
`-fsanitize=address,undefined -fno-sanitize-recover=all` でビルドされ、
**境界外アクセス・use-after-free・メモリリーク・未定義動作**を検出した時点で
（アサーションの成否とは無関係に）テストが失敗します。CI の `test` ジョブは常に
この設定で回るので（`.github/workflows/test.yml`）、

- **リファクタ**でメモリ不正を持ち込めばそのコミットで赤くなり、
- **予期しない外部入力**（例: GitHub 側の仕様変更で崩れた updater パーサ入力）は
  `UpdaterRobustnessTests` の擬似ファズが大量に流し込み、サニタイザが番人になります。

カバレッジ計測とは別ジョブ・別ビルドに分けてあるので、赤の原因が「テスト失敗＋
サニタイザ検出」か「カバレッジ閾値割れ」かで一目で切り分けられます。

テスト自体は依存ゼロの小さなハーネス（`TestFramework.h`）で書きます。`TEST(name){ … }`
の中で `CHECK` / `CHECK_EQ` を使い、`TEST_MAIN()` を 1 か所だけ置きます。

## まとめ

- **インポートの解析側**（`core/` + `parse/`）は SDK に依存しないので、実フィクスチャに
  対して要素ごとに単体テストする。**描画側**（`draw/`）は SDK と実図面が要るため、
  切り出せるロジックを `core/` へ寄せたうえで、残りは実機での目視確認に委ねる。
- **判断**は `UpdaterParse.h` の純粋関数に寄せ、関数単位で網羅的にテストする。
- **フロー**は `IUpdaterHost` というシームを挟み、SDK 全体をモックするのではなく
  プラグインが触る 6 つの副作用（スクリプト実行・通知・質問・ビルド選択・本体の
  取り下ろし・再起動）
  だけをフェイク化して、分岐と文言まで丸ごとテストする
  （ユニット／コンポーネントテスト。e2e ではない）。
- **スクリプト**は末尾のディスパッチをガードして `source`（dot-source）可能にし、
  ネットワーク境界（`.sh`: `curl` / `plutil`、`.ps1`: `Invoke-GH` / `Invoke-WebRequest`）
  だけを差し替えて `q-stable` / `q-dev` / `do-install` を **Linux 上で**単体テストする
  （C++ の `IUpdaterHost` に対応するスクリプト版のシーム）。
- 残るのは SDK 関数を呼ぶだけの薄い配線と、Mac／Windows 実機でしか動かない OS 固有
  ツールの実行だけで、ここは費用対効果から e2e ／手動に委ねる。
