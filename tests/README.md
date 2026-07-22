# テスト方針

このプラグインのテストは **Vectorworks SDK を必要としない** ことを最優先に設計して
います。SDK（約 800 MB）を落とさずに、素の Linux ランナーで速く回せるので、フォークから
の PR でもカバレッジまで含めて CI が通ります（`.github/workflows/test.yml`）。

## 何をテストしているか

テストは 2 本立てです。

1. **`UpdaterParseTests`** … `src/UpdaterParse.h` の純粋ロジック（`std::string` /
   `std::vector` だけに依存し、`gSDK`・`dladdr`・Win32・VWFC ダイアログに一切触れない
   関数）を関数単位でテストします。
2. **`UpdaterFlowTests`** … 起動時の更新フロー本体（`RunStableStartupCheckWith` /
   `RunDevStartupCheckWith`、`src/UpdaterFlow.cpp`）を、**フェイクの `IUpdaterHost`**
   越しに丸ごと動かして、分岐とダイアログ文言まで検証します（後述）。

`Updater.cpp` は残った

- 自分自身のバイナリ位置の解決（`dladdr` / `GetModuleFileName`）
- スクリプトの起動（`popen` / `_popen`）
- ネイティブダイアログの表示（`gSDK->AlertInform` / `AlertQuestion`、`VWDialog`）

という **プラットフォーム／SDK 固有のグルーだけ** を担います。

`UpdaterParse.h` の関数は 3 層に分かれます。

| 層 | 関数 | 役割 |
|----|------|------|
| スクリプト出力の解析 | `Trim` / `ValueOf` / `ParseDevBuilds` | `key=value` 行・`build\t…` 行の解析 |
| コマンドライン生成 | `ShellQuote` / `CmdQuote` | `/bin/sh`・cmd.exe 用の安全なクオート |
| 自パスからの導出 | `Mac*FromBinary` / `Win*FromPath/Dir` | 同梱スクリプト・Plug-Ins フォルダのパス導出 |
| **更新フローの判断** | `EvaluateStable` / `DevSwitchCandidates` / `ResolveDevSelection` / `InstallReportedOk` / `InstallErrorText` | 「更新があるか」「切替候補はどれか」「選択→ビルド」「インストール成否」 |

最後の「更新フローの判断」層は、もともと `Updater.cpp` の `gSDK` 呼び出しの合間に
インラインで書かれていた分岐です。純粋関数として切り出したことで単体テストの対象になり、
`Updater.cpp` 側は判断結果を受けてダイアログを出すだけになりました。

## フロー全体のテスト（インターフェイス／フェイク方式）

判断だけでなく **フロー全体**（スクリプトに問い合わせ→判断→ダイアログ→インストール→
結果表示）も SDK 抜きでテストしています。フローが実行する副作用を 4 つに絞って
`IUpdaterHost`（`src/UpdaterHost.h`）というインターフェイスにまとめました。

| メソッド | 本番（`Updater.cpp`） | テスト（`UpdaterFlowTests.cpp`） |
|----------|----------------------|--------------------------------|
| `RunScript` | 同梱スクリプトを `popen` で実行 | 固定の stdout を返す |
| `Inform` / `Ask` | `gSDK->AlertInform` / `AlertQuestion` | 呼び出しを記録／既定の回答を返す |
| `PickBuild` | VWFC のプルダウンダイアログ | 選択インデックスを返す |

フロー本体（`RunStableStartupCheckWith` / `RunDevStartupCheckWith`、`src/UpdaterFlow.cpp`）
は `IUpdaterHost&` だけに依存し、SDK ヘッダを一切 include しません。よって

- **本番** は `Updater.cpp` が `gSDK` / `popen` / VWFC で実装した本物の host を渡し、
- **テスト** は呼び出しを記録して canned な回答を返すフェイク host を渡す

だけで、「更新あり→肯定→インストール成功→完了ダイアログ」「ユーザーが拒否→何もしない」
「インストール失敗→エラー文言」といった経路を、**実際のダイアログ文言と `do-install` の
引数まで含めて**検証できます。

> これは **ユニットテスト（コンポーネントテスト）** です。テスト対象の「ユニット」は
> フロー関数で、host はそれを差し替えるテストダブル（フェイク）です。**e2e ではありません**
> — e2e なら実際に Vectorworks 上でプラグインを起動し、本物の GitHub API を叩き、本物の
> ダイアログを出して確認することになります。ここではプロセス内で SDK ゼロで完結します。

## テストの実行

```sh
cmake -S . -B build -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

カバレッジ（gcov/gcovr。GCC か Clang が必要）:

```sh
cmake -S . -B buildcov -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON -DVW_ENABLE_COVERAGE=ON
cmake --build buildcov
ctest --test-dir buildcov
gcovr --root . --filter 'src/.*' buildcov --txt --print-summary
```

テスト自体は依存ゼロの小さなハーネス（`TestFramework.h`）で書きます。`TEST(name){ … }`
の中で `CHECK` / `CHECK_EQ` を使い、`TEST_MAIN()` を 1 か所だけ置きます。

## それでも残る部分

判断もフローも SDK 抜きでカバーできたので、テストが届いていないのは
**外部 API の呼び出しそのもの** だけになりました。

### 1. `Updater.cpp` の薄いグルー

`BundledScriptPath` / `BundlePluginsDir` / `RunBundledScript`（`popen`）と、
`CVectorworksUpdaterHost` の各メソッド（`gSDK->AlertInform` / `AlertQuestion` の呼び出し、
VWFC ダイアログの生成）。ロジックは全て他へ委譲済みで、ここは「どの SDK 関数を呼ぶか」
という配線だけです。パスの導出は `*FromBinary`（テスト済み）に、選択→ビルドの写像は
`ResolveDevSelection`（テスト済み）に寄せてあるため、この層をさらにテストするための
SDK ヘッダ全体のスタブ化は、保守コストが高い割にリターンが小さいので推奨しません。
`CBuildPickerDialog` も同様に、責務を `PickBuild` の外（`ResolveDevSelection`）へ
出してあるので薄いままにしています。

### 2. シェル／PowerShell スクリプト（`scripts/vw-update.sh` / `vw-update.ps1`）

GitHub API 取得・zip 展開・インストールを担う部分。ここは C++ ではないので上記の対象外
ですが、`bats`（bash）や Pester（PowerShell）で、`curl` / `plutil` をスタブに差し替えた
うえで `asset_url` / `installed_commit` / `q_stable` などの関数単位テストが可能です。
本テンプレートの CI には含めていません（別ジョブとして追加できます）。

## まとめ

- **判断**は `UpdaterParse.h` の純粋関数に寄せ、関数単位で網羅的にテストする。
- **フロー**は `IUpdaterHost` というシームを挟み、SDK 全体をモックするのではなく
  プラグインが触る 4 つの副作用だけをフェイク化して、分岐と文言まで丸ごとテストする
  （ユニット／コンポーネントテスト。e2e ではない）。
- 残るのは SDK 関数を呼ぶだけの薄い配線で、ここは費用対効果からテスト対象外とする。
