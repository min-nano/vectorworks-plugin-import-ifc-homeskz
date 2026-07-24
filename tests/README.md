# テスト方針

このプラグインのテストは **Vectorworks SDK を必要としない** ことを最優先に設計して
います。SDK（約 800 MB）を落とさずに、素の Linux ランナーで速く回せるので、フォークから
の PR でもカバレッジまで含めて CI が通ります（`.github/workflows/test.yml`）。

## 何をテストしているか

テストは 5 本立てです。

1. **`UpdaterParseTests`** … `src/UpdaterParse.h` の純粋ロジック（`std::string` /
   `std::vector` だけに依存し、`gSDK`・`dladdr`・Win32・VWFC ダイアログに一切触れない
   関数）を関数単位でテストします。
2. **`UpdaterFlowTests`** … 起動時の更新フロー本体（`RunStableStartupCheckWith` /
   `RunDevStartupCheckWith`、`src/UpdaterFlow.cpp`）を、**フェイクの `IUpdaterHost`**
   越しに丸ごと動かして、分岐とダイアログ文言まで検証します（後述）。
3. **`UpdaterRobustnessTests`** … `src/UpdaterParse.h` のパーサに **予期しない外部入力**
   （壊れたスクリプト出力・埋め込み NUL・巨大／退化した行・ランダムなバイト列）を
   食わせ、境界外アクセスや未定義動作を起こさないこと、そして「戻り値の `url` は必ず
   非空」「`EvaluateStable` が更新を提示するのは整形式のときだけ」「`ResolveDevSelection`
   は範囲外を返さない」といった**契約**が保たれることを検証します。とりわけ
   **ASan / UBSan 有効時**（後述）に真価を発揮し、リファクタが招くメモリ不正や、GitHub 側
   仕様変更で崩れた入力への耐性を守ります（`tests/UpdaterRobustnessTests.cpp`）。
4. **`UpdaterScriptTests`** … 同梱スクリプト `scripts/vw-update.sh`（macOS）の
   **機械可読バックエンド**（`q-stable` / `q-dev` / `do-install` と、その土台の
   `asset_url` / `installed_commit`）を、`curl` / `plutil` を差し替えて検証します
   （`tests/vw-update.test.sh`、後述）。
5. **`UpdaterScriptTestsPs`** … その Windows 版 `scripts/vw-update.ps1` を、同じ発想で
   `Invoke-GH` / `Invoke-WebRequest` を差し替えて検証します（`tests/vw-update.Tests.ps1`、
   後述）。PowerShell 7（`pwsh`）は Linux でも動くので、**同じ Linux ランナー**で回せます。

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

サニタイザ（ASan + UBSan。GCC か Clang が必要）で回す:

```sh
cmake -S . -B buildsan -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON -DVW_ENABLE_SANITIZERS=ON
cmake --build buildsan
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir buildsan --output-on-failure
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

## スクリプトのテスト（ソース＋スタブ方式）

「更新の実体」を担う `scripts/vw-update.sh`（macOS）と `scripts/vw-update.ps1`
（Windows。GitHub API 取得・zip 展開・インストール）も、C++ 側と同じ発想で
SDK ／ネットワーク抜きに単体テストします（`tests/vw-update.test.sh` /
`tests/vw-update.Tests.ps1`）。Pester や bats などの外部フレームワークは使わず、
`TestFramework.h` と同じ**依存ゼロの極小ハーネス**を各ファイルに同梱しています。

いずれも「実行（プラグイン・手動）ではディスパッチが走り、テストでは `source`
（dot-source）して個々の関数を直接呼ぶ」という**シーム**をスクリプト末尾に用意して
あります（`UpdaterFlow.cpp` の `IUpdaterHost` に対応するスクリプト版）。

```sh
# vw-update.sh
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	main "$@"
fi
```

```powershell
# vw-update.ps1
if ($MyInvocation.InvocationName -ne '.') {
    switch ($mode) { ... }   # 実行時のみディスパッチ
}
```

プラグインは `.sh` を `/bin/bash <script>`、`.ps1` を `powershell -File <script>` で
**実行**するので本番は不変。テストだけが dot-source して、外側の I/O を差し替えます。

**`vw-update.sh`（bash）**

| 差し替える関数 | 本番 | テスト |
|----------------|------|--------|
| `jval` | `plutil`（macOS 専用）で JSON 抽出 | `python3` で同等のキーパス抽出。実際の `asset_url` / `q_stable` / `q_dev` がフィクスチャ JSON に対して動く |
| `api_get` | `curl` で GitHub REST API | フィクスチャファイルを返す（オフラインも再現） |
| `download` | `curl` でアセット取得 | ローカルの zip を配置（失敗も再現） |
| `installed_commit` | `PlistBuddy`（macOS 専用） | 既定コミットを返す（「バンドル無し → none」の枝は本物を直接検証） |

**`vw-update.ps1`（PowerShell 7）** — こちらは差し替えが 2 つで済み、より本物に近い形で
動きます（`Get-InstalledCommit` は macOS ツールではなく `<name>.commit` テキストを読む
だけなので実物、`Expand-Archive` / `Copy-Item` も pwsh の標準機能でそのまま動く）。

| 差し替える関数 | 本番 | テスト |
|----------------|------|--------|
| `Invoke-GH` | `Invoke-RestMethod` で GitHub REST API | フィクスチャ JSON を `ConvertFrom-Json` して返す（オフラインは throw） |
| `Invoke-WebRequest` | アセットをダウンロード | ローカルの zip を `-OutFile` にコピー（失敗は throw） |

これで両スクリプトとも、`q-stable` の `installed` / `latest`（7 桁化）/ `url`、`q-dev` の
`dev-*` フィルタとアセットのあるビルドだけの列挙、`do-install` の成功・各失敗経路
（ダウンロード失敗／想定外の zip／引数不足）まで、**素の Linux ランナーで**検証できます。

各スクリプトに残る OS 固有の面（`.sh` の osascript ダイアログ・`codesign` / `xattr` の
再署名・`PlistBuddy`、`.ps1` の `%APPDATA%` 既定パス）は、その OS でしか動かないため、
C++ 側が `dladdr` / `gSDK` のグルーを対象外にしているのと同様、手動／e2e に委ねます。
必要なツール（`.sh`: python3 / unzip / zip、`.ps1`: `pwsh`）が無い**ローカル**環境では、
ハーネス自体が自動で SKIP、あるいは CMake がそのテストを登録しません（`scripts/lint.sh` の
`skip` と同じ方針）。

ただし **CI では黙ってスキップさせません**。ツールが欠けたまま「テストが 1 件も走らずに緑」
になるのを防ぐため、`-DVW_REQUIRE_SCRIPT_TESTS=ON`（Tests ワークフローが指定）を付けると
挙動が逆転します。インタプリタ（`bash` / `pwsh`）が無ければ **configure が FATAL_ERROR** で
失敗し、ハーネスに渡す補助ツール（python3 / unzip / zip）が無ければ **ハーネスが SKIP では
なく exit 1** で失敗します。この値は各ハーネスの環境変数 `VW_REQUIRE_SCRIPT_TESTS` として
渡され、ローカル既定（OFF）では従来どおり穏やかに SKIP します。

## それでも残る部分

判断・フロー・スクリプトのバックエンドまで SDK 抜きでカバーできたので、テストが届いて
いないのは **プラットフォーム固有のグルーと外部 API 呼び出しそのもの** だけになりました。

### 1. `Updater.cpp` の薄いグルー

`BundledScriptPath` / `BundlePluginsDir` / `RunBundledScript`（`popen`）と、
`CVectorworksUpdaterHost` の各メソッド（`gSDK->AlertInform` / `AlertQuestion` の呼び出し、
VWFC ダイアログの生成）。ロジックは全て他へ委譲済みで、ここは「どの SDK 関数を呼ぶか」
という配線だけです。パスの導出は `*FromBinary`（テスト済み）に、選択→ビルドの写像は
`ResolveDevSelection`（テスト済み）に寄せてあるため、この層をさらにテストするための
SDK ヘッダ全体のスタブ化は、保守コストが高い割にリターンが小さいので推奨しません。
`CBuildPickerDialog` も同様に、責務を `PickBuild` の外（`ResolveDevSelection`）へ
出してあるので薄いままにしています。

### 2. スクリプトの OS 固有部分

`.sh` / `.ps1` のバックエンドは上記 `UpdaterScriptTests` / `UpdaterScriptTestsPs` で
ctest／CI に組み込み済みです。残るのは各 OS でしか動かない部分だけ、

- **macOS**（`.sh`）… `codesign` / `xattr` の再署名、`PlistBuddy`、osascript ダイアログ。
- **Windows**（`.ps1`）… `%APPDATA%` 既定パス、ロード中 `.vlb` の rename 退避など。

いずれもその OS の実機（＝手動／e2e）でしか意味を持たないため、C++ 側が
`dladdr` / `gSDK` のグルーを対象外にしているのと同じ理由で対象外とします。

## まとめ

- **判断**は `UpdaterParse.h` の純粋関数に寄せ、関数単位で網羅的にテストする。
- **フロー**は `IUpdaterHost` というシームを挟み、SDK 全体をモックするのではなく
  プラグインが触る 4 つの副作用だけをフェイク化して、分岐と文言まで丸ごとテストする
  （ユニット／コンポーネントテスト。e2e ではない）。
- **スクリプト**は末尾のディスパッチをガードして `source`（dot-source）可能にし、
  ネットワーク境界（`.sh`: `curl` / `plutil`、`.ps1`: `Invoke-GH` / `Invoke-WebRequest`）
  だけを差し替えて `q-stable` / `q-dev` / `do-install` を **Linux 上で**単体テストする
  （C++ の `IUpdaterHost` に対応するスクリプト版のシーム）。
- 残るのは SDK 関数を呼ぶだけの薄い配線と、Mac／Windows 実機でしか動かない OS 固有
  ツールの実行だけで、ここは費用対効果から e2e ／手動に委ねる。
