# vectorworks-plugin-native-template

C++ SDK でネイティブな Vectorworks 2026 プラグインを作るためのテンプレートです。

ビルドシステム・CI・リリース／アップデートの仕組みまで揃った**最小構成の動く
プラグイン**が入っています。サンプルのプラグインはメニューコマンドを 1 つ追加する
だけで、実行すると「起動した」ことを知らせるアラートダイアログを表示します。

**macOS と Windows の両方**を、同じソースからビルドします（Vectorworks 2026 が対応
する 2 プラットフォーム）。

## 構成

```
CMakeLists.txt              macOS / Windows 両対応の CMake ビルド
src/
  ModuleMain.cpp            モジュールのエントリポイント。拡張機能を登録し、
                            起動時にアップデート確認を仕掛ける（stable は新しい
                            安定版確認、dev はブランチ選択）
  Extensions/ExtMenu.{h,cpp}  アラートを表示するメニューコマンド
  Updater.{h,cpp}           同梱した更新スクリプトを起動してアップデートを駆動する
                            （macOS: vw-update.sh / Windows: vw-update.ps1）
  BuildConfig.h             stable / dev の識別切り替えスイッチ（VW_DEV_BUILD）
  PluginPrefix.h            共有プレフィックスヘッダ（SDK を取り込む）
  Module-Info.plist.in      バンドルの Info.plist テンプレート（macOS 専用・ビルド
                            ごとに埋める）
resources/
  SamplePlugin.vwr/…             stable プラグインのメニュー文字列
  SamplePluginDev.vwr/…          dev プラグインのメニュー文字列
scripts/
  vw-update.sh              CI ビルドをダウンロード／インストールする（macOS 用。
                            バンドルに同梱され、プラグインから起動される）
  vw-update.ps1             同上の Windows 版（PowerShell。.vlb の隣に同梱される）
  lint.sh                   ローカルで全 lint（clang / cmake / yaml / shell …）
                            を実行する（CI と同じチェック。--fix で自動修正）
.clang-format               C/C++ フォーマット規則（タブ・Allman ブレース等）
.clang-tidy                 C/C++ 静的解析チェックの設定（WarningsAsErrors）
.cmake-format.yaml          CMake の整形（cmake-format）＋ lint（cmake-lint）設定
.yamllint.yaml              YAML の構造スタイル（yamllint）設定
PSScriptAnalyzerSettings.psd1  PowerShell 静的解析（PSScriptAnalyzer）のルール設定
.editorconfig              エディタ側のインデント／改行／文字コード規則
.editorconfig-checker.json  上記を CI で強制する editorconfig-checker の設定
.github/workflows/build.yml CI: macOS（Apple Silicon）と Windows でビルドする
.github/workflows/lint.yml  CI: ソース／非ソースを問わずコーディング規則を強制
```

同じソースから、1 つのスイッチ（`VW_DEV_BUILD`、`src/BuildConfig.h` を参照）で
**共存できる 2 つのプラグイン**をビルドします。

- **`SamplePlugin`** — *stable* プラグイン。`main` からビルドされます。
  メニューカテゴリは **Sample**。
- **`SamplePluginDev`** — *dev* プラグイン。フィーチャー／PR ブランチから
  ビルドされます。メニューカテゴリは **Sample (Dev)**。

プラグインの入れ物はプラットフォームで異なります。

- **macOS** — `<name>.vwlibrary` バンドル。`.vwr` リソースはバンドル内
  （`Contents/Resources`）に含まれます。
- **Windows** — `<name>.vlb` モジュール（DLL）。`.vwr` リソースは同名の別ファイルと
  して `.vlb` の隣に置かれます（SDK の Windows での作法）。

出力名・`.vwr` 識別子・VCOM ユニバーサル名・拡張機能 UUID がそれぞれ別々なので、
両方を同時にインストールしてロードできます — stable は通常利用に、dev は作業中の
ブランチを試すために使えます。各プラグインのメニューコマンドは、アラート内に自分の
チャンネルとビルドコミットを表示します。そのコミットは、判別できるよう各ビルドに
刻まれます（macOS はバンドルの `Info.plist` の `VWBuildChannel` / `VWBuildCommit`、
Windows は `.vlb` の隣の `<name>.commit` ファイル）。アップデータはこれを読んで何が
インストールされているかを判別します。

各メニューコマンドの表示テキストは、それぞれの `resources/<name>.vwr` フォルダから
来ます。ビルド時に SDK の `BuildVWR` ツールがこれをパッケージするので（macOS は
バンドル内の `Contents/Resources/<name>.vwr`、Windows は `.vlb` の隣の
`<name>.vwr`）、各プラグインは自己完結しています。

## プレースホルダー識別子

サンプル固有の識別子は次の通りです。実際のプラグインではこれらを置き換えます。

| 種別 | 値 | 場所 |
| --- | --- | --- |
| バンドル／出力名 | `SamplePlugin` / `SamplePluginDev` | `CMakeLists.txt`、`src/BuildConfig.h`、`resources/` フォルダ名、`scripts/vw-update.sh`、`scripts/vw-update.ps1`、`.github/workflows/build.yml` |
| バンドル ID（macOS） | `com.example.vectorworks.SamplePlugin(Dev)` | `CMakeLists.txt` |
| メニューカテゴリ | `Sample` / `Sample (Dev)` | `resources/*/Strings/*.vwstrings` |
| C++ 名前空間・クラス | `SamplePlugin` / `CExtMenuSample` / `CSampleMenu_EventSink` | `src/Extensions/ExtMenu.{h,cpp}`、`src/ModuleMain.cpp` |
| VCOM ユニバーサル名 | `CExtMenuSample_SamplePlugin(Dev)` | `src/BuildConfig.h` |
| 拡張機能 UUID | stable / dev 各 1 個 | `src/Extensions/ExtMenu.cpp`（一意である必要があるため `uuidgen` で再生成） |
| リポジトリ | `min-nano/vectorworks-plugin-native-template` | `scripts/vw-update.sh` / `scripts/vw-update.ps1` の `VW_REPO` 既定値 |

`.vwstrings` は UTF-16LE（BOM 付き・CRLF 改行）です。編集時はエンコーディングを保持
してください。現在の識別子は次で一覧できます。

```sh
grep -rniE "sampleplugin|com\.example|CExtMenuSample|CSampleMenu" \
  --exclude-dir=.git .
```

## ローカルでのビルド

CMake 3.20+ と、対象プラットフォームの **Vectorworks 2026 SDK** が必要です。SDK は
`VW_SDK_DIR` を **`SDKLib` を含むフォルダ**に向けて渡します（`-DVW_SDK_DIR=...` また
は環境変数）。

### macOS

Xcode（Vectorworks 2026 は公式に **Xcode 16.2** を対象）と **mac SDK** が必要です。

1. SDK をダウンロードして展開します:
   <https://release.vectorworks.net/latest/Vectorworks/2026-NNA-eng-mac-SDK.zip>
   （約 800 MB）。展開すると `SDKLib/` を含むフォルダができます。

2. コンフィグとビルド:

   ```sh
   cmake -S . -B build -DVW_SDK_DIR=/path/to/2026-NNA-eng-mac-SDK
   cmake --build build --config Release
   ```

   成果物は `build/SamplePlugin.vwlibrary` です。

既定では Apple Silicon（`arm64`）向けにビルドします。ユニバーサルバイナリにするには:

```sh
cmake -S . -B build -DVW_SDK_DIR=/path/to/sdk \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

### Windows

Visual Studio 2022（v143 ツールセット、x64）と **win SDK** が必要です。

1. SDK をダウンロードして展開します:
   <https://release.vectorworks.net/latest/Vectorworks/2026-NNA-eng-win-SDK.zip>
   展開すると `SDKLib/` を含むフォルダができます。

2. コンフィグとビルド（既定の Visual Studio ジェネレータを使います）:

   ```pwsh
   cmake -S . -B build -A x64 -DVW_SDK_DIR=C:/path/to/2026-NNA-eng-win-SDK
   cmake --build build --config Release
   ```

   成果物は `build/Release/SamplePlugin.vlb`（DLL）と、その隣の
   `build/Release/SamplePlugin.vwr`（リソース）です。ビルドスタンプの
   `SamplePlugin.commit` と更新スクリプト `vw-update.ps1` も同じ場所に出力されます。

macOS の `.vwlibrary` バンドルと違い、Windows のプラグインは `<name>.vlb` 本体と
同名の `<name>.vwr` を**同じフォルダに一緒に**置く必要があります（`.commit` と
`vw-update.ps1` も同梱すると自動アップデートが機能します）。

> **アーキテクチャは x64 のみ（ARM も x64 でカバー）**
> Vectorworks の Windows 版は x64 アプリで、SDK も **x64 ライブラリのみ**を同梱して
> います（`LibWin` に ARM64 版はありません）。プラグイン DLL はホストプロセスと同じ
> アーキテクチャでないとロードされないため、ビルド対象は **x64 一択**です（`-A x64`）。
> これは **Windows on ARM でもそのまま動きます** — その環境では x64 版 Vectorworks が
> OS の x64 エミュレーション上で動作し、この x64 プラグインをそのまま読み込みます
> （ネイティブ ARM64 プラグインはエミュレート中の x64 ホストにロードできず、そもそも
> リンクもできません）。したがって ARM 向けの別ビルドは不要です。macOS 側で
> `arm64`／ユニバーサルにできるのは、Vectorworks Mac がネイティブ Apple Silicon
> アプリだからです。

## インストールと実行（社内 / 未署名での利用）

このプラグインは社内利用向けで、**未署名**（Apple Developer ID 署名なし、
Vectorworks 開発者クレデンシャルなし）で配布されます。

### macOS

インストールして実行するには:

1. **バンドルをローカルディスクに置きます**（iCloud Drive は不可 — iCloud が
   ダウンロード隔離フラグを付け直すことがあります）。置き場所は Vectorworks 2026
   のユーザフォルダ内の `Plug-Ins` ディレクトリです（Vectorworks ▸ 環境設定 ▸
   *ユーザフォルダ* から探せます）。`.vwr` リソースはバンドル内に含まれているので、
   `SamplePlugin.vwlibrary` フォルダだけで十分です。

2. Gatekeeper がダウンロードしたバンドルをブロックしないよう、**macOS の隔離フラグを
   解除します**:

   ```sh
   xattr -dr com.apple.quarantine SamplePlugin.vwlibrary
   ```

   CI ビルドは既に **アドホック署名済み**です（Apple Silicon がバイナリをロードする
   ために必須。無料であり、Developer ID 署名ではありません）。ローカルビルドの場合は
   リンカが自動でアドホック署名します。それでも macOS が「壊れている」と言う場合は、
   自分で署名し直してください:

   ```sh
   codesign --force --deep --sign - SamplePlugin.vwlibrary
   ```

3. **Vectorworks を起動します。** プラグインが未署名のため、Vectorworks 2026 は起動時
   に「不明／未署名のプラグイン」警告を表示し、既定で無効化することがあります。警告を
   了解してプラグインを有効化してください — これは社内向け・クレデンシャルなしの
   プラグインでは想定どおりの挙動で、社内利用では問題ありません。

4. **コマンドをワークスペースに追加します:** ツール ▸ ワークスペース ▸ 現在の
   ワークスペースを編集 ▸ *メニュー*。**Sample** カテゴリの中に **起動確認** コマンド
   があるので、メニューにドラッグしてください。実行するとアラートが表示されます。

### Windows

Gatekeeper もアドホック署名も無いぶん手順は簡単です:

1. **`SamplePlugin.vlb` と `SamplePlugin.vwr` を一緒に**、Vectorworks 2026 のユーザ
   フォルダ内の `Plug-Ins` ディレクトリに置きます（Vectorworks ▸ 環境設定 ▸ *ユーザ
   フォルダ* から探せます）。2 つは同名・同フォルダである必要があります。自動アップ
   デートも使うなら `SamplePlugin.commit` と `vw-update.ps1` も一緒に置きます（CI の
   `SamplePlugin.vlb.zip` にはこれらがすべて入っています）。

2. **Vectorworks を起動します。** プラグインが未署名のため、Vectorworks 2026 は起動時
   に「不明／未署名のプラグイン」警告を表示し、既定で無効化することがあります。警告を
   了解してプラグインを有効化してください — 社内向け・クレデンシャルなしのプラグイン
   では想定どおりの挙動です。

3. **コマンドをワークスペースに追加します**（macOS の手順 4 と同じ）。

Vectorworks 開発者クレデンシャル（2026 の「サテライト」ファイル）は、警告の出ない
*署名済み*プラグインを配布する場合にのみ必要で、ビルドや社内での実行には不要です。

## テストとカバレッジ

アップデータ（`src/Updater.cpp`）のうち、SDK に依存しない純粋なロジック
（スクリプト出力のパース、コマンドラインのクォート、インストール先パスの導出）は
`src/UpdaterParse.h` に切り出してあります。これにより、Vectorworks SDK なしで
**どのプラットフォームでも**ユニットテストを実行できます。テスト本体は
`tests/`（`tests/UpdaterParseTests.cpp`）にあり、外部依存のない極小のテストハーネス
（`tests/TestFramework.h`）を使うため、テストフレームワークのダウンロードも不要です。
同梱スクリプトのバックエンド（`q-stable` / `q-dev` / `do-install`）も、ネットワーク
境界だけを差し替えて同じく SDK ／ネットワーク抜きにテストします — macOS 版
`scripts/vw-update.sh` は `tests/vw-update.test.sh`（bash＋`curl`/`plutil` スタブ）、
Windows 版 `scripts/vw-update.ps1` は `tests/vw-update.Tests.ps1`（PowerShell 7＋
`Invoke-GH`/`Invoke-WebRequest` スタブ）で、いずれも Linux ランナー上で動きます
（詳細は `tests/README.md`）。

ローカルでの実行（SDK 不要）:

```bash
cmake -S . -B build-tests -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

サニタイザ（AddressSanitizer + UBSan）を有効にして回す（メモリ不正・未定義動作の検出）:

```bash
cmake -S . -B build-san -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON -DVW_ENABLE_SANITIZERS=ON
cmake --build build-san
ctest --test-dir build-san --output-on-failure
```

CI の `test` ジョブは常にこの設定でテストを回すため、リファクタが招くメモリ不正
（境界外アクセス・use-after-free・リーク）や、updater パーサが GitHub 側の仕様変更で
崩れた入力を誤処理するケースは、その場でビルドを赤にできます。予期しない外部入力に
対する耐性は `tests/UpdaterRobustnessTests.cpp` の擬似ファズ／敵対的入力テストが担い、
サニタイザがその番人になります（詳細は `tests/README.md`）。

ビルドオプション:

- `VW_BUILD_PLUGIN`（既定 `ON`）… プラグイン本体をビルドします（SDK が必要で、
  macOS / Windows のみ）。テストだけをビルドしたいときは `OFF` にします。
- `VW_BUILD_TESTS`（既定 `OFF`）… ユニットテストをビルドします。
- `VW_ENABLE_COVERAGE`（既定 `OFF`）… テストに gcov 用の計測を付けます（GCC / Clang）。
- `VW_ENABLE_SANITIZERS`（既定 `OFF`）… テストを ASan + UBSan
  （`-fsanitize=address,undefined -fno-sanitize-recover=all`）でビルド・実行します
  （GCC / Clang）。

### カバレッジレポート（GitHub Actions で内製）

`.github/workflows/test.yml` は、テストを Linux ランナー（SDK のダウンロード不要
なので高速）で実行する **`test` ジョブ**と、それに続く **`coverage` ジョブ**の 2 つに
分かれています。`test` の失敗はテスト自体の失敗を、`coverage` の失敗はレポート生成の
失敗を意味するので、原因を切り分けやすくしています。`coverage` ジョブは `test` の成功後
にのみ実行され、`gcovr` で **Cobertura 形式**のカバレッジレポートを生成します。

カバレッジの可視化は **GitHub Actions だけで完結**しており、GitHub Code Quality など
外部サービスには依存しません。レポートは次の 2 つの情報を **1 つの表**にまとめた
**1 つの PR コメント**として投稿され（2 回目以降は同じコメントを自動更新）、常に
ビルド成果物としても保存されます。

- **全体カバレッジ（`src/`）**… `gcovr` の集計（行・分岐・関数）。
- **差分カバレッジ**… `diff-cover` により、この PR が変更した行のみをベースブランチ
  （マージベース）と比較したカバレッジ。新しく追加・変更したコードがテストされて
  いるかがひと目で分かります。表の最終行に集計値だけを取り込みます（`diff-cover` の
  Markdown 出力はそのまま貼らず、JSON 出力から値を抽出）。

各行は絵文字で状態を色分けします（🟢 良好 / 🟡 注意 / 🔴 低 / ⚪ 参考）。しきい値は
行 🟢≥95% 🟡≥90%、関数 🟢100%、差分 🟢100% 🟡≥95%、分岐は参考扱い（⚪）で、
ワークフロー内の `THRESHOLDS` に一箇所でまとめており、変更できます。**🔴 が 1 つでも
あると `coverage` ジョブは失敗します**（表示だけでなく CI でしきい値を強制）。この
判定はすべてのイベントで走るため、`main` への push やフォーク PR（コメントはスキップ
されますが判定は有効）でも同様に適用されます。

コメントの投稿には `pull-requests: write` 権限が必要で、トークンが読み取り専用となる
フォーク PR ではスキップされます（レポートはアーティファクトとしては常に保存されます）。
`main` への push では比較対象の差分がないため、差分カバレッジとコメント投稿は行わず、
レポートの生成・アーティファクト保存・しきい値判定を行います。

チェックアウトは**浅い（shallow）まま**で、差分カバレッジに必要な履歴だけを都度取得
します。全ブランチの全履歴を取る `fetch-depth: 0` は使わず、**ベースブランチのみ**を
マージベースに届くまで段階的に deepen するので、CI 時間はリポジトリの履歴の長さでは
なく、その PR の分岐量に応じた分だけで済みます。

ローカルでは同じレポートを次のように再現できます（差分カバレッジはベースブランチを
指定）:

```bash
cmake -S . -B build -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON -DVW_ENABLE_COVERAGE=ON
cmake --build build
ctest --test-dir build --output-on-failure
gcovr --root . --filter 'src/.*' build --cobertura coverage.xml --txt --print-summary
# 差分カバレッジ（例: origin/main と比較）
diff-cover coverage.xml --compare-branch origin/main --markdown-report diff-cover.md
```

## 継続的インテグレーション（CI）

`.github/workflows/build.yml` がプラグインをビルドします。`main` は保護された
デフォルトブランチで、機能開発は必ず PR 上で行うため、ブランチを二重にビルドしない
ようトリガを分けています。

- **`main` への push**（マージ）は **stable** リリースをビルドして公開します。
- **PR** はそのブランチをビルドして **dev** プレリリースを公開します。

ワークフローの内容:

- **2 つのビルドジョブ**を持ちます。`build-mac`（`macos-15`・Apple Silicon、Xcode 16.2）と
  `build-windows`（`windows-latest`・Visual Studio 2022）で、両者は**並行**して走ります。
  それぞれが対応するプラットフォームの SDK をダウンロードします。
- SDK は一度だけダウンロードし、（トリミングした）SDK を**キャッシュ**するので、大きな
  zip は以降の実行で再ダウンロードされません。強制的に再ダウンロードするにはワーク
  フロー内の `VW_SDK_CACHE_KEY`（各ジョブに 1 つ）を変更します。
- 各ジョブが `SamplePlugin` と `SamplePluginDev` の**両方**をビルドし、コミットで刻印
  （`-DVW_BUILD_VERSION`）して成果物を確認・アップロードします（macOS はさらにアドホック
  署名）。PR ではエフェメラルなマージコミットではなく、PR の **head** コミット（あなたが
  push したもの）をビルドします。
- **ダウンロード可能なリリースを公開**し、アップデータが取得できる安定した URL を用意
  します。1 つのリリースに **macOS と Windows 両方のアセット**が入ります:
  - `main` はローリングな **`stable`** リリースを更新します
    （`SamplePlugin.vwlibrary.zip` + `SamplePlugin.vlb.zip`）。
  - PR はブランチごとの **`dev-<branch>`** プレリリースを更新します
    （`SamplePluginDev.vwlibrary.zip` + `SamplePluginDev.vlb.zip`。トークンで公開でき
    ないフォーク PR ではスキップされます）。

  リリースの公開は独立した **`release` ジョブ**が担当します。このジョブは 2 つのビルド
  ジョブ（`build-mac` と `build-windows`）が**両方**完了してから走り（`needs:
  [build-mac, build-windows]`）、両ジョブがアップロードした成果物をまとめてダウンロード
  し、**macOS と Windows 両方のアセットを 1 つのリリースに**添付して公開します。公開を
  ビルドから切り出したことで、どちらのプラットフォームも単独でリリースを作らなくなり、
  作成とアタッチが競合することがありません。どちらもローリング方式で、毎回タグを最新
  ビルドに貼り直します。**stable** の公開は GitHub API の長時間障害があってもリトライ
  します（stable リリースの取りこぼしは気づかれにくいため）。**dev** の公開はリトライ
  しません — dev ビルドはブランチ作業中にしか使わないので、一時的なエラーが出たら
  ジョブを再実行すれば十分です。

`.github/workflows/cleanup-dev-release.yml` は、ブランチが削除されたときにその
`dev-<branch>` プレリリース（とタグ）を削除し、dev ビルドが溜まらないようにします。
`delete` イベントで起動されるため、デフォルトブランチ上のコピーから実行され、この
ワークフローが `main` に入った後に削除されたブランチだけを対象とします。

`.github/workflows/stable-release-healthcheck.yml` はスケジュール（6 時間ごと）で
安全網として実行されます。公開済みの `stable` リリースが `main` の先頭からずれている
場合 — つまり stable の公開を取りこぼした場合 — `main` で `build.yml` を再ディスパッチ
して再ビルド・再公開します。スケジュール／`delete` 系のワークフローと同様に
デフォルトブランチから実行されるため、`main` にマージされて初めて有効になります。

## コーディング規則の強制（Lint）

`.github/workflows/lint.yml` が、`main` への push とすべての PR で
コーディング規則を機械的に強制します。対象は C/C++ ソースにとどまらず、
**CMake ビルドファイル・GitHub ワークフロー・シェルスクリプト、そしてすべての
テキストファイルの空白／文字コード**まで、それぞれ専用のフォーマッタ／リンタで
チェックします。原因を切り分けやすいよう、チェックごとに独立したジョブに
分かれています。

C/C++ を対象とするジョブ:

- **`clang-format`** — `src/` と `tests/` の**すべての** C/C++ ソースを
  `.clang-format` に照らしてチェックします。`--dry-run --Werror` なので 1 か所
  でも規則から外れると失敗し、書き換えは行いません。SDK もビルドも不要なので
  高速で、SDK が要る（`#if GS_MAC` / `GS_WIN`）プラットフォーム固有のグルー
  コードも含めて**全ファイル**を対象にできます。
- **`clang-tidy`** — SDK 非依存のアップデータロジック（`src/UpdaterFlow.cpp` と
  それが取り込む `UpdaterParse.h` / `UpdaterHost.h`）に対して静的解析を行います。
  clang-tidy は翻訳単位を実際にコンパイルする必要があり、SDK が不要なこの Linux
  ランナー上では**実ロジックを持つ SDK 非依存コード**を対象にします。`.clang-tidy`
  は `WarningsAsErrors: "*"` なので、検出があれば CI が失敗します。

ソース以外のファイルを対象とするジョブ:

- **`cmake-format`** — `CMakeLists.txt` 群の整形を `.cmake-format.yaml`
  （タブ・100 桁・コメントは再整形しない）に照らして `--check` します。あわせて
  **`cmake-lint`** が CMake のバグを招きやすいパターンを検出します。
- **`actionlint`** — ワークフロー YAML 自体（構文・式・参照アクション・ランナー
  ラベル）を検証し、同梱の **shellcheck** で各 `run:` のインラインスクリプトも
  静的解析します。
- **`shellcheck`** — `scripts/` 配下のスタンドアロンなシェルスクリプトを解析
  します（ワークフロー内のインラインスクリプトは actionlint が担当）。
- **`PSScriptAnalyzer`** — Windows 版アップデータ（`scripts/vw-update.ps1`）の
  PowerShell 静的解析です。未承認の動詞・未使用パラメータ・危険な null 比較など
  バグを招きやすいパターンを検出します。clang-tidy（`src/` の実ロジックのみ）や
  shellcheck（`scripts/*.sh` のみ）と同じく、テストハーネス（`tests/`）ではなく
  `scripts/` 配下の**本番スクリプト**を対象にします。ルールは
  `PSScriptAnalyzerSettings.psd1`（デフォルト全ルールから、このスクリプトの意図的な
  設計と衝突する数個だけを除外）で管理し、残った検出はすべて CI を失敗させます。
- **`yamllint`** — ワークフローや Dependabot 設定など YAML の構造スタイル
  （インデント・キー重複・記号まわりの空白）を `.yamllint.yaml` に照らして
  チェックします。
- **`editorconfig-checker`** — **すべての**テキストファイルについて、末尾改行・
  行末空白なし・UTF-8・LF を強制します（フォーマッタがカバーしない衛生面）。
  対象規則は `.editorconfig`、実行する検査と除外は `.editorconfig-checker.json`
  で設定します。インデントは各フォーマッタ（clang-format / cmake-format）が
  タブ＋スペース整列で管理するため、この検査ではあえて無効化しています。

**SDK 依存コードの静的解析（`build.yml`）** — 同じ `.clang-tidy` ルールを、SDK が
ないとコンパイルできないプラグイン本体（`ModuleMain.cpp` / `Extensions/ExtMenu.cpp`
/ `Updater.cpp`）にも適用します。ビルドジョブは SDK を用意するので、**ビルド直前**に
clang-tidy を走らせて全ソースを網羅します。

- **macOS ジョブ** — `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` で生成した compile
  database に対して clang-tidy を実行し、`#if GS_MAC` 側の分岐を解析します。
- **Windows ジョブ** — Visual Studio ジェネレータは compile database を出力しない
  ため、解析専用に **Ninja + clang-cl** でビルドせずに再コンフィグして database を
  生成し、`#if GS_WIN` 側の分岐（`Updater.cpp` の `Widen` / `Narrow` /
  `OwnModulePath` / `RunBundledScript`）を解析します。

macOS が `GS_MAC`、Windows が `GS_WIN` の分岐をそれぞれ担当するので、両者を合わせて
**すべての行**が clang-tidy でチェックされます。

バージョンについて: SDK 非依存の `lint.yml` と macOS ジョブは clang 18 に固定して
います。Windows ジョブだけは**最新の LLVM**を使います — ランナーの MSVC 標準ライブラリ
ヘッダが「Clang 20 以降」を要求する（`static_assert` と Clang 20 の組み込み関数を使う）
ため、clang-cl / clang-tidy がそれを解析できる新しさである必要があるからです。

**採用しているルール:**

- フォーマット（`.clang-format`）— タブインデント（幅 4）、Allman ブレース
  （`{` を次行に置く）、名前空間本体をインデント、ポインタ／参照は型側に寄せる
  （`int* p`）、コード幅 100 桁で折り返し。コメントは再整形しません
  （`ReflowComments: false`）— 手作業で整形された重厚なコメント（日本語・罫線を
  含む）を壊さないためです。
- 静的解析（`.clang-tidy`）— `bugprone-*`、`performance-*`、`modernize-*`、
  `readability-*`、`cppcoreguidelines-*`、`clang-analyzer-*` を有効化し、
  スタイル系のノイズ（フォーマットは clang-format が担当）や大規模な無関係リ
  ファクタを要求するチェックは無効化しています。

**ローカルでの実行**（CI と同じチェック）:

```bash
scripts/lint.sh          # チェックのみ（違反があれば非ゼロ終了）
scripts/lint.sh --fix    # その場で自動修正（clang-format -i / clang-tidy --fix /
                         # cmake-format -i）。残りは検査のみ
```

`scripts/lint.sh` は CI と同じ全ツールを走らせ、未インストールのツールは
「skip」と表示して飛ばすので、手元に一部しか入っていなくても部分実行できます
（完全なゲートは CI 側）。各ツールのインストール方法は未インストール時に表示
されます。CI が使うバージョンは `lint.yml` 冒頭の `env:` に固定してあります。

`.editorconfig` も用意してあり、多くのエディタがインデント・改行・文字コードを
保存時に合わせるので、CI に到達する前から規則に近い状態を保てます。

> **さらに強化するには（任意）:** コミット前の自動実行に
> [`pre-commit`](https://pre-commit.com/)、追加の C++ 静的解析に
> [`cppcheck`](https://cppcheck.sourceforge.io/) を組み合わせられます。SDK 依存の
> プラグイン本体（`Updater.cpp` など）は CI（`build.yml`）で clang-tidy を掛けて
> いますが、ローカルで掛けたい場合は SDK を用意したうえで
> `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` でコンフィグし、生成された
> `compile_commands.json` に対して `clang-tidy` を実行してください。

## SDK ドキュメント（API 仕様）

**Vectorworks SDK の API 仕様は GitHub 上の Markdown リポジトリで公開されています。**
かつての開発者 Wiki（`developer.vectorworks.net`）は廃止され、現在は用途ごとに分かれた
公開 GitHub リポジトリに移行しています（一覧はランディングページ
[`DeveloperLandingPage.md`](https://github.com/Vectorworks/developer-scripting/blob/main/DeveloperLandingPage.md)
を参照）。旧 Wiki の URL（`index.php?title=SDK:...`）は現在このランディングページへ
301 リダイレクトされます。

| 内容 | リポジトリ |
| --- | --- |
| **C++ / VCOM SDK**（このテンプレートが対象） | <https://github.com/Vectorworks/developer-sdk> |
| Python / VectorScript / Marionette スクリプト | <https://github.com/Vectorworks/developer-scripting> |
| ワークシート関数 | <https://github.com/Vectorworks/developer-worksheets> |

すべて Markdown なので、Web ブラウザが使えない環境（CI やエージェントのサンドボックス
など）でも、GitHub へ到達できれば内容を確認できます。`git clone` でまるごと手元に
落とすこともできます:

```sh
git clone --depth 1 https://github.com/Vectorworks/developer-sdk
```

C++/VCOM SDK（[`developer-sdk`](https://github.com/Vectorworks/developer-sdk)）の主な
ドキュメント（`Info/` フォルダ）:

- [Using the SDK](https://github.com/Vectorworks/developer-sdk/blob/main/Info/Using%20the%20SDK.md) — SDK の使い方の全体像
- [Plug-in Module](https://github.com/Vectorworks/developer-sdk/blob/main/Info/Plug-in%20Module.md) — モジュールのエントリポイント（旧 `SDK:Module_Plug-in`。`src/ModuleMain.cpp` が対応）
- [VCOM (Vectorworks Component Object Model)](https://github.com/Vectorworks/developer-sdk/blob/main/Info/VCOM%20(Vectorworks%20Component%20Object%20Model).md) — VCOM の仕組み
- [Types](https://github.com/Vectorworks/developer-sdk/blob/main/Info/Types.md) — 基本型（`TXString`・`WorldPt` ほか）
- [The Vectorworks Environment](https://github.com/Vectorworks/developer-sdk/blob/main/Info/The%20Vectorworks%20Environment.md) — 実行環境
- バージョン別の情報は [`Versions/`](https://github.com/Vectorworks/developer-sdk/tree/main/Versions)（2026 / 2025 / … ）にあります。

## 自動アップデート

アップデートはコマンドラインではなく、**プラグイン自身がネイティブの Vectorworks
ダイアログ**（`gSDK->AlertInform` / `gSDK->AlertQuestion`、およびドロップダウン選択は
`VWFC::VWUI::VWDialog` + `VWPullDownMenuCtrl`）を表示して行います（`src/Updater.cpp`）。ネットワーク・インストールなどの実処理（GitHub API の参照・
ダウンロード・`Plug-Ins` へのインストール）は**プラットフォームごとの更新スクリプト**
に集約され、ビルド時にインストール物と一緒に**同梱**されます:

- **macOS** — `scripts/vw-update.sh`（bash）。バンドル内の
  `Contents/Resources/vw-update.sh` に入り、隔離解除とアドホック再署名も行います。
- **Windows** — `scripts/vw-update.ps1`（PowerShell）。`.vlb` の隣に入ります。

プラグインはこのスクリプトを**非対話モード**（`q-stable` / `q-dev` / `do-install`）で
呼び出して結果を受け取り、ユーザーへの表示はすべて自前のネイティブダイアログで行う
ため、利用者がターミナルを開く必要はありません。どちらの OS でも
`src/Updater.cpp` の同じフロー・ダイアログが動き、変わるのは「自分の場所を特定する
方法（macOS は `dladdr`、Windows は `GetModuleFileName`）」と「起動するスクリプト」
だけです。

チャンネルごとに挙動が異なります。

- **stable（`SamplePlugin` / main）** — **Vectorworks 起動時**に、より新しい安定版
  ビルドがないかを確認します（`src/ModuleMain.cpp` がモジュールロード時に一度だけ実行）。
  - 既に最新なら**何も表示しません**（毎回の起動を邪魔しません）。
  - 新しいビルドがあれば `AlertQuestion` で「インストールしますか？」と尋ね、選ばれた
    場合だけインストールします。
  - ネットワーク確認は時間制限付き（`vw-update.sh` の `--max-time`）で、オフラインや
    エラー時は静かに諦めます。

- **dev（`SamplePluginDev` / ブランチ）** — **Vectorworks 起動時**に、使用するビルドを
  **ネイティブのプルダウンダイアログ**（`VWFC::VWUI::VWDialog` + `VWPullDownMenuCtrl`、
  `src/Updater.cpp` の `CBuildPickerDialog`）で問い合わせます（`src/ModuleMain.cpp` が
  モジュールロード時に一度だけ実行）。1 つのドロップダウンに候補を一覧表示します:
  - 先頭は**現在ロードされているビルド**（branch / commit、「インストール済み」と明示）。
  - 続いて**他のブランチのプレリリース**（現在のビルドと同じコミットは除外）。
  - **インストール済み（先頭）を選ぶ／キャンセル** → 何もせず起動を続けます。
  - **別のブランチを選ぶ** → それをインストールします（反映は次回起動）。インストール
    完了メッセージを表示します。
  現在の実行ビルドの判定にはコンパイル時に埋め込まれた commit（`VW_BUILD_VERSION`）を
  使うため、ディスク上に別ビルドが未反映で置かれていても取り違えません。

  以前はこの確認を**コマンド実行時**に行っていましたが、プラグインが自身のコマンドを
  プログラム内から再実行しうると毎回ダイアログが出てしまいます。コンパイル済みビルドは
  そもそも起動時にしか差し替わらないため、確認は起動時に一度だけ行います。

コンパイル済みプラグインは起動時にしか読み込まれないため、新しいビルドが実際にロード
されるのは次回 Vectorworks を起動（または手動で再起動）したときです。macOS ではバンドル
の隔離解除とアドホック再署名をスクリプトが行います。Windows では実行中の `.vlb` を
削除できない（メモリにマップされている）ため、スクリプトは古い `.vlb` をいったん退避
（リネーム）してから新しいものを書き込みます。退避ファイル（`*.old-*`）は次回の更新時に
掃除します。

プラグイン経由の更新は、**実行中のモジュール自身が置かれているフォルダ**（＝Vectorworks
が実際に読み込んだ `Plug-Ins`）へインストールします（`src/Updater.cpp` が自分のパスを
解決し ― macOS は `dladdr`、Windows は `GetModuleFileName` ― `VW_PLUGINS_DIR` として
スクリプトに渡します）。ユーザフォルダを既定と違う場所に設定していても、読み込まれて
いるコピーを直接置き換えるので更新が確実に反映されます。なお手動 CLI（下記）は既定
パスを使うため、ユーザフォルダが独自の場合は `VW_PLUGINS_DIR` を実際の場所に合わせて
実行してください。

リポジトリは公開なので、認証や追加ツールは不要です。各スクリプトは OS 標準のものだけ
を使います — macOS は `curl`・`plutil`・`unzip`・`codesign`・`xattr`・`osascript`
（`osascript` は下記の手動 CLI パスのみ）、Windows は PowerShell 組み込みの
`Invoke-RestMethod` / `Invoke-WebRequest` / `Expand-Archive`。

プラグインを経由せず、スクリプトを直接実行することもできます（手動確認・トラブル
シュート用。macOS の CLI パスは osascript ダイアログ、Windows の CLI パスはコンソール
プロンプトを使います）:

```sh
# --- macOS (bash) -----------------------------------------------------------
# stable チャンネル（main → SamplePlugin）:
./scripts/vw-update.sh stable
# dev チャンネル — どのブランチのビルドを入れるか選ぶ（→ SamplePluginDev）:
./scripts/vw-update.sh dev
# 引数なし（または Finder でダブルクリック）: 最初にチャンネルを尋ねます。
./scripts/vw-update.sh
# プラグインが内部的に使う非対話モード（ダイアログなし・機械可読出力）:
./scripts/vw-update.sh q-stable                # stable の状態を表示
./scripts/vw-update.sh q-dev                   # dev ビルド一覧を表示
./scripts/vw-update.sh do-install <url> <name> # ダウンロードしてインストール
```

```pwsh
# --- Windows (PowerShell) ---------------------------------------------------
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 stable
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 dev
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1          # チャンネルを尋ねる
# 非対話モード（プラグインが使うもの。stable/dev/do-install は sh 版と同じ契約）:
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 q-stable
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 q-dev
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 do-install <url> <name>
```

環境変数で上書き可能: `VW_REPO`（owner/repo）、`VW_PLUGINS_DIR`（インストール先）。
2 つのチャンネルは別名のプラグインをインストールするので、stable と dev が互いを
上書きすることはありません。
