# 開発ガイド

このプラグインをビルド・テスト・lint し、CI とリリースを回すための手順です。

- 利用者向けの説明（何をするプラグインか・使い方）は [`README.md`](../README.md)。
- Vectorworks SDK の実測知見と設計判断は [`DEV-NOTES.md`](DEV-NOTES.md)。
- 作業時の規約（ディレクトリ・命名・依存の向き・PR とマージの規則・CI の待ち方）は
  [`CLAUDE.md`](../CLAUDE.md)。

## ソースの構成

処理は **IFC 解析フェーズ（`src/parse/`）** と **VW 描画フェーズ（`src/draw/`）** に完全分離し、
両者は命令セット（`src/core/Document.h`）だけで接続します。`parse/` と `core/` は
**SDK を一切 include しない**ので、SDK 無しでコンパイル・単体テストできます
（設計の詳細は [`DEV-NOTES.md`](DEV-NOTES.md)「設計の考え方」）。

```
CMakeLists.txt              macOS / Windows 両対応の CMake ビルド。SDK 非依存の
                            静的ライブラリ HomeskzIfcCore（core/ + parse/）と、
                            SDK 依存のプラグイン本体（draw/ ほか）に分かれる
src/
  ModuleMain.cpp            モジュールのエントリポイント。拡張機能を登録し、
                            起動時にアップデート確認を仕掛ける
  Extensions/
    ExtMenu.{h,cpp}           「IFC (ホームズ君) 取り込み…」メニューコマンド。
                              ファイル選択 → parse → draw → 完了ダイアログを束ねる
    ExtColumnMark.{h,cpp}     柱・小屋束の記号 PIO（対象レイヤの構造材を走査して
                              断面記号 ×／／ と平面記号を描く）
  core/                     フェーズ非依存の土台（SDK も STEP も知らない純粋コード）
    Document.{h,cpp}          命令セットの構造体定義・validateDocument・描画結果の件数
    Geometry.{h,cpp}          自前の Vec2 / Vec3 / Mat4（配置行列）
    Layout.{h,cpp}            用紙の割り付け（縮尺の階梯と選び方・伏図の位置と凡例の列・
                              軸組図の上下 2 段とシートの分割）
    Region.{h,cpp}            部品が囲む平面領域の合成（ロフト床の外形）
    Progress.{h,cpp}          進捗の報告先・文言整形・バー配分（実測の重み）
    Trace.{h,cpp}             クラッシュ診断ログ（フェーズ単位・毎行フラッシュ）
  parse/                    Phase 1: IFC 解析（SDK 非依存）
    Step.{h,cpp}              最小 STEP リーダ（トークナイザ＋エンティティグラフ）
    Loader.{h,cpp}            ファイル読み込み（テキスト → STEP グラフ）
    IfcAttr.h                 IFC 属性インデックスの唯一の定義
    IfcGeometry.{h,cpp}       配置行列・断面・押し出し・屋根面の解決
    Context.{h,cpp}           解析中の共有キャッシュ（同じ前処理を繰り返さない）
    BuildDocument.{h,cpp}     解析のオーケストレーション
    Summary.{h,cpp}           完了・エラーダイアログの本文（要素の一覧は kElements 表 1 つ）
    StructuralClass.{h,cpp}   部材種別 → VW クラスの純ロジック
    Grid / Story / Floor / Member / Noboribari / Column / Rafter / Roof /
    Footing / AnchorBolt / FloorPost / FireBrace / Joint / ColumnMark /
    Sheet / Tag / Section      要素ごとの解析
  draw/                     Phase 2: VW 描画（SDK 依存）
    ExecuteDocument.{h,cpp}   命令セットを検証して要素ごとにディスパッチ
    DrawUtil.{h,cpp}          クラス分け・by-class 属性・レイヤ／シートレイヤの用意・
                              構成層・ビューポートの仕上げ（縮尺・用紙の大きさ・位置合わせ）・
                              Undo スコープの共通ヘルパー
    StructuralMember.{h,cpp}  構造材ツール 1 本の生成・設定（横架材／柱で共有）
    ObjectHandles.h           「命令インデックス → 描いたオブジェクトのハンドル」の対応表
    ProgressDialog.{h,cpp}    core::ProgressReporter を VW の進捗ダイアログへ橋渡し
    Symbol.{h,cpp}            ハイブリッドシンボルの配置（4 要素で共有する唯一の実装）
    Tag.{h,cpp}               断面寸法データタグ（伏図・軸組図で共有。スタイルは当てず、
                              タグの中身はタグ 1 本ずつへ直接組む）
    Grid / Story / Floor / Member / Column / Rafter / Roof / Footing /
    ColumnMark / Sheet / Legend / Section   要素ごとの描画
  Updater*.{h,cpp}          同梱した更新スクリプトを起動してアップデートを駆動する
  BuildConfig.h             stable / dev の識別切り替えスイッチ（VW_DEV_BUILD）
  PluginPrefix.h            共有プレフィックスヘッダ（SDK を取り込む）
  Module-Info.plist.in      バンドルの Info.plist テンプレート（macOS 専用）
tests/                      無 SDK の単体テスト（詳細は tests/README.md）
  TestFramework.h           依存ゼロの極小テストハーネス
  Fixtures.h / RoofSample.h 共有するフィクスチャ読み込み・近似比較・試験用屋根面
  fixtures/                 ホームズ君 EX 出力の実 IFC
resources/
  HomeskzIfcImport.vwr/…             stable プラグインのメニュー文字列
  HomeskzIfcImportDev.vwr/…          dev プラグインのメニュー文字列
scripts/
  vw-update.sh              CI ビルドをダウンロード／インストールする（macOS 用。
                            バンドルに同梱され、プラグインから起動される）
  vw-update.ps1             同上の Windows 版（PowerShell。.vlb の隣に同梱される）
  lint.sh                   ローカルで全 lint（clang / cmake / yaml / shell …）
                            を実行する（CI と同じチェック。--fix で自動修正）
  clang-tidy-sdk.sh         SDK 依存の翻訳単位（src/draw/ ほか）に clang-tidy を
                            かける（対象一覧の唯一の定義）
  fetch-vw-sdk.sh           Vectorworks SDK をランナーへ用意する（ダウンロード →
                            トリミング → 検証。build.yml と ci-debug.yml が共有）
  ci-common.sh              CI の完了待ちの共通土台（必ず有限時間で exit する歯止め）
  ci-wait.sh                PR ／ブランチ ／コミットの CI が終わった瞬間に exit する
  ci-debug.sh               CI デバッグ実行を起動し、完了まで待って結果を取り出す
                            （SDK が手元に無い環境から SDK 依存の調査を行うため）
  ci-debug-job.sh           同・ランナー側の本体。調査モードの実装はこちらにある
  vw-dump-pio-fields.py     VW 実機の「スクリプト編集」に貼って走らせる読み取り専用の
                            ダンプ（選択オブジェクトのパラメトリックレコード・付いて
                            いるレコード・オブジェクト変数・文書内のビューポート一覧）。
                            SDK に API の無い PIO の設定を、UI で手作業したものと
                            見比べて突き止めるための道具（CI では使わない）
.clang-format               C/C++ フォーマット規則（タブ・Allman ブレース等）
.clang-tidy                 C/C++ 静的解析チェックの設定（WarningsAsErrors）
.cmake-format.yaml          CMake の整形（cmake-format）＋ lint（cmake-lint）設定
.yamllint.yaml              YAML の構造スタイル（yamllint）設定
PSScriptAnalyzerSettings.psd1  PowerShell 静的解析（PSScriptAnalyzer）のルール設定
.editorconfig               エディタ側のインデント／改行／文字コード規則
.editorconfig-checker.json  上記を CI で強制する editorconfig-checker の設定
.github/workflows/build.yml CI: macOS（Apple Silicon）と Windows でビルドし、
                            リリース（main=stable / PR=dev）を公開する
.github/workflows/test.yml  CI: 無 SDK の単体テスト（ASan+UBSan）とカバレッジ
.github/workflows/lint.yml  CI: ソース／非ソースを問わずコーディング規則を強制
.github/workflows/codeql.yml            CI: CodeQL による静的解析（週次＋PR）
.github/workflows/cleanup-dev-release.yml  PR のクローズ時に dev プレリリースを片付ける
.github/workflows/stable-release-healthcheck.yml
                            stable リリースの取りこぼしを検知して再ビルドする
.github/workflows/ci-debug.yml  CI: 手動ディスパッチ専用のデバッグ実行（SDK 調査・
                            ビルド再現）。push / PR では起動しない
```

**依存の向きは厳守します。** `parse/` と `core/` は Vectorworks SDK を include せず、
`draw/` は STEP / IFC を include しません。両者をつなぐのは `core/Document.h` だけで、
この規律は CI（`core/` `parse/` を無 SDK でコンパイル・テストするジョブ）が担保します。

## プラグイン識別子

このプラグインを一意に識別する値は次の通りです（出発点にしたネイティブプラグイン
テンプレート `vectorworks-plugin-native-template` のプレースホルダーを、これらへ
置き換えてあります）。フォークして別プラグインを作るときは、同じ箇所を自分の値へ
置き換えます。

| 種別 | 値 | 場所 |
| --- | --- | --- |
| バンドル／出力名 | `HomeskzIfcImport` / `HomeskzIfcImportDev` | `CMakeLists.txt`、`src/BuildConfig.h`、`resources/` フォルダ名、`scripts/vw-update.sh`、`scripts/vw-update.ps1`、`.github/workflows/build.yml` |
| バンドル ID（macOS） | `io.github.min-nano.HomeskzIfcImport(Dev)` | `CMakeLists.txt` |
| メニューカテゴリ | `ファイル`（コマンド名 `IFC (ホームズ君) 取り込み…`） | `resources/*/Strings/*.vwstrings` |
| C++ 名前空間・クラス | `HomeskzIfcImport` / `CExtMenuImportIfc` / `CImportIfcMenu_EventSink` | `src/Extensions/ExtMenu.{h,cpp}`、`src/ModuleMain.cpp` |
| VCOM ユニバーサル名 | `CExtMenuImportIfc_HomeskzIfcImport(Dev)` | `src/BuildConfig.h` |
| 拡張機能 UUID | stable / dev 各 1 個 | `src/Extensions/ExtMenu.cpp`（一意である必要があるため `uuidgen` で再生成） |
| リポジトリ | `min-nano/vectorworks-plugin-import-ifc-homeskz` | `scripts/vw-update.sh` / `scripts/vw-update.ps1` の `VW_REPO` 既定値 |

`.vwstrings` は UTF-16LE（BOM 付き・CRLF 改行）です。編集時はエンコーディングを保持
してください。現在の識別子は次で一覧できます。

```sh
grep -rniE "homeskzifcimport|io\.github\.min-nano|CExtMenuImportIfc|CImportIfcMenu" \
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

   成果物は `build/HomeskzIfcImport.vwlibrary` です。

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

   成果物は `build/Release/HomeskzIfcImport.vlb`（DLL）と、その隣の
   `build/Release/HomeskzIfcImport.vwr`（リソース）です。ビルドスタンプの
   `HomeskzIfcImport.commit` と更新スクリプト `vw-update.ps1` も同じ場所に出力されます。

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


## テストとカバレッジ

テストはすべて **Vectorworks SDK 無し**で走ります（SDK は約 800 MB のダウンロードを
伴うため）。外部依存のない極小のテストハーネス（`tests/TestFramework.h`）を使うので、
テストフレームワークのダウンロードも不要です。対象は 2 系統あります。

- **インポート機能の解析側**（`src/core/` + `src/parse/`）… 2 フェーズ分離により SDK に
  触れないので、実際のホームズ君 IFC（`tests/fixtures/`）に対して要素ごとに単体テスト
  します（STEP リーダ・幾何・通り芯・ストーリ・床・垂木・野地板・構造クラス判定…）。
  描画側（`src/draw/`）は SDK と実図面を要するため単体テストを持たず、実機での目視確認に
  委ねます（確認の作法は [`DEV-NOTES.md`](DEV-NOTES.md)「実機確認の作法」）。
- **アップデータ**（`src/Updater.cpp`）… SDK に依存しない純粋なロジック（スクリプト出力の
  パース、コマンドラインのクォート、インストール先パスの導出）を `src/UpdaterParse.h` に
  切り出し、更新フロー本体は `IUpdaterHost` のフェイク越しに丸ごと動かします。同梱
  スクリプトのバックエンド（`q-stable` / `q-dev` / `do-install`）も、ネットワーク境界だけを
  差し替えて SDK ／ネットワーク抜きにテストします — macOS 版 `scripts/vw-update.sh` は
  `tests/vw-update.test.sh`（bash＋`curl`/`plutil` スタブ）、Windows 版
  `scripts/vw-update.ps1` は `tests/vw-update.Tests.ps1`（PowerShell 7＋
  `Invoke-GH`/`Invoke-WebRequest` スタブ）で、いずれも Linux ランナー上で動きます。
  再起動のコマンド（終了要求 → 終了待ち → 起動し直し）は純粋関数が組み立てるので、生成
  される shell / PowerShell そのものを `tests/UpdaterParseTests.cpp` で検証します。

**テストの一覧・方針・何をテストしていないかは `tests/README.md`** に詳しくあります。

ローカルでの実行（SDK 不要）:

```bash
cmake -S . -B build-tests -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON
cmake --build build-tests --parallel "$(nproc)"
ctest --test-dir build-tests --output-on-failure -j "$(nproc)"
```

`--parallel` / `-j` を付けないと、cmake も ctest も**既定で 1 コアしか使いません**。
テストは 20 本以上の独立した実行ファイルなので、コア数を渡すだけで素直に短くなります
（CI の `test` ジョブも同じ指定で回します。下記「カバレッジレポート」）。

サニタイザ（AddressSanitizer + UBSan）を有効にして回す（メモリ不正・未定義動作の検出）:

```bash
cmake -S . -B build-san -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON -DVW_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel "$(nproc)"
ctest --test-dir build-san --output-on-failure -j "$(nproc)"
```

CI の `test` ジョブは常にこの設定（に `VW_ENABLE_COVERAGE=ON` を足したもの）で
テストを回すため、リファクタが招くメモリ不正
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
分かれています。分担は**「計測」と「レポート」**で、**テストのビルドと実行は `test`
ジョブでの 1 回だけ**です。

- **`test`** … ASan + UBSan **と gcov 計測を同時に有効にして**ビルドし、テストを
  1 回実行します。この 1 回の実行が、正しさの判定（テスト失敗・サニタイザ検出）と
  カバレッジデータ（`.gcda`）の生成を兼ねます。ビルドもテスト実行も**ランナーの
  全コア**を使います（`--parallel "$(nproc)"` / `ctest -j "$(nproc)"`）。続けて
  `gcovr` で **Cobertura 形式**のレポート（`coverage.xml`）と集計 JSON を生成し、
  アーティファクト `coverage-report` として `coverage` ジョブへ渡します。
- **`coverage`** … `test` の成功後にのみ実行され、**コンパイルもテスト実行もしません**。
  受け取った計測結果に `diff-cover` で差分カバレッジを加え、表を PR コメントとして
  投稿し、しきい値を判定します。

`test` の失敗はテスト自体の失敗（サニタイザ検出・計測の失敗を含む）を、`coverage` の
失敗はレポート生成またはしきい値割れを意味するので、原因を切り分けやすいという性質は
そのままです。

**並列実行とカバレッジ**: テストを `-j` で同時に走らせても、計測結果は逐次実行と
**完全に一致**します。共有されている状態は、どのテスト実行ファイルもリンクしている
`HomeskzIfcCore` の `.gcda` カウンタだけで、libgcov はそこへ書き戻すときファイルを
ロックするためです（この PR で逐次実行と付き合わせて確認済み: 行 4883/4994・
分岐 4346/6300・関数 418/418 が両者で完全一致）。テスト実行ファイル同士は独立した
プロセスなので、それ以外に共有するものはありません。

サニタイザと gcov は同じ GCC のフラグとして共存できるため、以前のように
「サニタイザ付きのビルド＋実行」と「カバレッジ付きのビルド＋実行」を 2 回行う必要は
ありません。唯一の注意点は無害なもので、サニタイザが異常を検出して abort した場合は
`.gcda` が書き出されません——つまり計測が完全なのはテストが通ったときだけですが、
`coverage` ジョブが走るのはまさにその場合だけです。

なお、サニタイザの計装が入る分、`gcovr` が数える行・分岐の**総数**がごくわずかに
増えるため、割合もわずかに動きます。同一コード（#42 時点）に対する CI の実測では、
行 98.1%（4379/4463）→ 98.0%（4393/4484）、分岐 70.1% → 69.0%、関数は 100% のまま
でした（総数はコードが増えれば動くので、比較の意味があるのは前後の差だけです）。しきい値（行 🟢≥95%）に対しては十分な余裕があり、分岐は参考扱い（⚪）です。

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

`coverage` ジョブのチェックアウトは `fetch-depth: 0`（全履歴）です。差分カバレッジには
ベースブランチとのマージベースがローカルに必要で、浅いチェックアウトから段階的に
deepen する方法は履歴の少ないリポジトリでマージベースに届かず `diff-cover` が
「no merge base」で落ちることがあるためです。このリポジトリの履歴は小さいので、
全履歴を取る追加コストは無視できます。

ローカルでは同じレポートを次のように再現できます（CI と違いサニタイザは付けていません。
差分カバレッジはベースブランチを指定）:

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

- **4 つの並行ジョブ**を持ちます。ビルドの `build-mac`（`macos-15`・Apple Silicon、
  Xcode 16.2）と `build-windows`（`windows-latest`・Visual Studio 2022）、および
  静的解析の `tidy-mac` / `tidy-windows` です。4 つとも**同時**に走り、互いを待ちません
  （clang-tidy はビルドより時間がかかるので、ビルドの中に置かず並走させています。
  詳細は下記「SDK 依存コードの静的解析」）。
- SDK は一度だけダウンロードし、（トリミングした）SDK を**キャッシュ**するので、大きな
  zip は以降の実行で再ダウンロードされません。強制的に再ダウンロードするにはワーク
  フロー内の `VW_SDK_CACHE_KEY`（プラットフォームごとに 1 つ）を変更します。SDK を
  用意する手順そのものは `scripts/fetch-vw-sdk.sh` に 1 つだけあり、4 ジョブと
  `ci-debug.yml` が共有します（キャッシュがヒットしていれば検証だけして抜けます）。
- 各ジョブは**その実行が公開するチャンネルだけ**をビルドします（`-DVW_BUILD_CHANNEL`。
  `main` は `HomeskzIfcImport`、PR は `HomeskzIfcImportDev`）。コミットで刻印
  （`-DVW_BUILD_VERSION`）して成果物を確認・アップロードします（macOS はさらにアドホック
  署名）。PR ではエフェメラルなマージコミットではなく、PR の **head** コミット（あなたが
  push したもの）をビルドします。
- **ダウンロード可能なリリースを公開**し、アップデータが取得できる安定した URL を用意
  します。1 つのリリースに **macOS と Windows 両方のアセット**が入ります:
  - `main` はローリングな **`stable`** リリースを更新します
    （`HomeskzIfcImport.vwlibrary.zip` + `HomeskzIfcImport.vlb.zip`）。
  - PR はブランチごとの **`dev-<branch>`** プレリリースを更新します
    （`HomeskzIfcImportDev.vwlibrary.zip` + `HomeskzIfcImportDev.vlb.zip`。トークンで公開でき
    ないフォーク PR では `release` ジョブごとスキップされます）。

  リリースの公開は独立した **`release` ジョブ**が担当します。このジョブは 4 つのジョブ
  （`build-mac` / `build-windows` / `tidy-mac` / `tidy-windows`）が**すべて**完了してから
  走り（`needs: [build-mac, build-windows, tidy-mac, tidy-windows]`）、両ビルドジョブが
  アップロードした成果物をまとめてダウンロード
  し、**macOS と Windows 両方のアセットを 1 つのリリースに**添付して公開します。公開を
  ビルドから切り出したことで、どちらのプラットフォームも単独でリリースを作らなくなり、
  作成とアタッチが競合することがありません。静的解析ジョブも `needs` に入っているので、
  **ビルドが通っていても clang-tidy が通らなければリリースは公開されません**（解析は
  ビルドと並走しているため、このゲートを保っても所要時間は増えません）。どちらもローリング方式で、毎回タグを最新
  ビルドに貼り直します。**stable** の公開は GitHub API の長時間障害があってもリトライ
  します（stable リリースの取りこぼしは気づかれにくいため）。**dev** の公開はリトライ
  しません — dev ビルドはブランチ作業中にしか使わないので、一時的なエラーが出たら
  ジョブを再実行すれば十分です。

  公開に至らない実行 — トークンでリリースを作れない**フォーク PR** や、`main` 以外の
  ref での `workflow_dispatch` — では、**`release` ジョブ自体が起動しません**（ジョブの
  `if` で振り分けているので、成果物のダウンロードも走りません）。スキップされたジョブの
  チェック結論は `skipped` で失敗ではないため、ブランチ保護や `ci-wait` の判定には
  影響しません。

`.github/workflows/cleanup-dev-release.yml` は、**PR がクローズされたとき**（マージの
有無を問わない）にその `dev-<branch>` プレリリース（とタグ）を削除し、dev ビルドが
溜まらないようにします。プレリリースは**PR が開いているあいだの成果物**（公開するのは
`build.yml` の `pull_request` 実行だけ）なので、PR が閉じた時点が役目の終わりです。
以前はブランチ削除（`delete` イベント）を合図にしていましたが、それだと**ブランチを
残したままマージした PR**や**マージせず閉じた PR**のプレリリースが残り続け、逆に PR を
持たないブランチの削除でも起動していました。

`pull_request` の `closed` で起動するため、ワークフローの実体は PR のマージ ref
（head を base にマージしたもの）側のコピーから実行されます。したがってこの変更が
`main` に入って初めて有効になり、それ以前から開いている PR も、マージ ref が新しい
`main` に対して作り直された時点でこの版を拾います。フォークからの PR は
（そもそも `build.yml` がプレリリースを公開しないので）ジョブの `if` で除外します。

これと対になる取り決めが `build.yml` 側にもあります。ビルドの実行中に PR が閉じられると、
片付けの側は「まだ公開されていないプレリリース」を探して空振りするので、その後にビルドが
公開すると**誰も消さないプレリリース**が残ります。これを避けるため、dev の公開ステップは
**PR がまだ open か**を公開の前後で確かめます（以前はブランチの存在を見ていました）。

`.github/workflows/stable-release-healthcheck.yml` はスケジュール（6 時間ごと）で
安全網として実行されます。公開済みの `stable` リリースが `main` の先頭からずれている
場合 — つまり stable の公開を取りこぼした場合 — `main` で `build.yml` を再ディスパッチ
して再ビルド・再公開します。スケジュール起動のワークフローはデフォルトブランチから
実行されるため、`main` にマージされて初めて有効になります。

#### 手動ディスパッチ（`workflow_dispatch`）を持つワークフロー

「Run workflow」ボタンは**必要なものにだけ**付けています。PR とマージで自動的に走る
チェック系（`lint.yml` / `test.yml` / `codeql.yml`）は手動起動する用途が無く、失敗した
実行を回し直したいだけなら Actions 画面の **Re-run** で足りるためディスパッチを持ちません。

| ワークフロー | 手動ディスパッチ | 理由 |
| --- | :---: | --- |
| `build.yml` | あり | `stable-release-healthcheck.yml` が stable の取りこぼしを検知して再ディスパッチする（リリース経路） |
| `stable-release-healthcheck.yml` | あり | 6 時間の次回スケジュールを待たずに stable のずれを直したいとき |
| `ci-debug.yml` | あり（専用） | 手動ディスパッチ**のみ**で起動する調査用ワークフロー |
| `lint.yml` / `test.yml` / `codeql.yml` | なし | push / PR（+ CodeQL は週次スケジュール）で自動的に走る |
| `cleanup-dev-release.yml` | なし | PR のクローズ（`pull_request` の `closed`）専用 |

### CI デバッグ（`ci-debug.yml`）

`.github/workflows/ci-debug.yml` は、**手動ディスパッチ専用**の「CI 上で 1 コマンドだけ
動かす」ワークフローです。SDK が手元に無い環境（クラウド上の開発セッションや、SDK を
インストールしていないマシン）から、**SDK 依存のビルドエラーの再現**や
**「この API は SDK にあるか」という調査**を行うためのものです。

`push` / `pull_request` では**決して起動せず**、リリースも公開しません（`contents: write`
を持たない）。SDK キャッシュは `build.yml` と同じキーで **読み取り専用**に復元するので、
デバッグ実行が本番ビルドのキャッシュを汚すこともありません。

起動から結果取得までは `scripts/ci-debug.sh` が一手に引き受けます。ディスパッチ →
実行中の run の特定 → **完了まで待機** → 結果ブロックだけを抽出、までを 1 コマンドで
行い、完了と同時に終了します（`GITHUB_TOKEN` / `GH_TOKEN` が必要）。

```bash
# SDK ヘッダを検索する（この API は SDK にあるか？）
scripts/ci-debug.sh run --mode sdk-grep --args 'GetLayerByName'

# ビルドエラーを再現する（--platform で mac / windows / linux を選ぶ）
scripts/ci-debug.sh run --mode build --platform windows
```

**モードの一覧・結果ブロックの読み方（`BEGIN/END PAYLOAD` マーカーと `truncated`）・
読み取り専用トークンしか無い環境での 2 手順・モードの増やし方は、`CLAUDE.md` の
「CI デバッグ」節が単一の真実です。** そちらを参照してください（この README では
重ねて説明しません）。

### CI の完了待ち（`scripts/ci-wait.sh`）

PR やブランチの CI（`build.yml` / `lint.yml` / `test.yml` …）が終わるのを待つ側にも
同じ道具立てを用意しています。`scripts/ci-wait.sh` は対象のチェックが全部終わった
**瞬間に exit** し、最終行に結果を出します。

```bash
scripts/ci-wait.sh --pr 34        # PR の head（待機中に push が入ったら追随する）
scripts/ci-wait.sh --ref main     # ブランチ / タグ
scripts/ci-wait.sh                # いま checkout しているブランチ
```

待機の土台（`scripts/ci-common.sh`）は `ci-debug.sh` と共有で、**どんな異常でも必ず
有限時間で exit する**ことを最優先に作ってあります（HTTP の時間上限・締切判定・
ウォッチドッグの三重）。加えて「チェックがまだ 1 件も登録されていない」「新しい push で
古い run がキャンセルされた」を green と取り違えません。この性質は
`tests/ci-wait.test.sh`（ctest の `CiWaitScriptTests`）で回帰テストしています。
結果の読み方（`conclusion=` の一覧）は `CLAUDE.md`「CI の完了を待つ」節が単一の真実です。

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
- **`clang-tidy`** — **SDK 非依存の全翻訳単位**、すなわち 2 フェーズのインポート
  コード（`src/core/*.cpp` / `src/parse/*.cpp`）と アップデータロジック
  （`src/UpdaterFlow.cpp`。取り込む `UpdaterParse.h` / `UpdaterHost.h` も
  `HeaderFilterRegex` で対象）に対して静的解析を行います。clang-tidy は翻訳単位を
  実際にコンパイルする必要があり、SDK が不要なこの Linux ランナー上では**実ロジックを
  持つ SDK 非依存コード**を対象にします。`core/` `parse/` はハードコードした一覧では
  なく**グロブ**で拾うので、新しい parse モジュールを足した瞬間から対象になります
  （`scripts/lint.sh` も同一の一覧を使います）。`.clang-tidy` は
  `WarningsAsErrors: "*"` なので、検出があれば CI が失敗します。

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

**SDK 依存コードの静的解析（`build.yml` の `tidy-mac` / `tidy-windows`）** — 同じ
`.clang-tidy` ルールを、SDK がないとコンパイルできない側（`src/draw/*.cpp` と
`ModuleMain.cpp` / `Extensions/ExtMenu.cpp` / `Updater.cpp`）にも適用します。
`src/draw/` はグロブで拾うため、要素を追加しても対象漏れが起きません
（`core/` `parse/` を `lint.yml` がグロブで拾うのと同じ理屈）。

- **`tidy-mac`** — `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` で生成した compile
  database に対して clang-tidy を実行し、`#if GS_MAC` 側の分岐を解析します。
- **`tidy-windows`** — Visual Studio ジェネレータは compile database を出力しない
  ため、解析専用に **Ninja + clang-cl** でビルドせずに再コンフィグして database を
  生成し、`#if GS_WIN` 側の分岐（`Updater.cpp` の `Widen` / `Narrow` /
  `OwnModulePath` / `RunBundledScript`）を解析します。

macOS が `GS_MAC`、Windows が `GS_WIN` の分岐をそれぞれ担当するので、両者を合わせて
**すべての行**が clang-tidy でチェックされます。

**なぜ独立したジョブなのか（速度）** — clang-tidy はビルドを走らせずに解析するので
PCH が使えず（`VW_ENABLE_PCH`）、1 翻訳単位ごとに SDK のアンブレラヘッダを丸ごと
読み直すため 1 本あたり 30〜40 秒かかります。当初はこれをビルドジョブの中で 15 本
直列に回していて、**Windows ジョブの 12 分 41 秒のうち 10 分 10 秒**（mac は 7 分 44 秒の
うち 6 分 40 秒）がこの 1 ステップでした。そこで 3 段階で速くしています。

1. **翻訳単位をランナーのコア数ぶん並列に解析する**（Windows 4 コア・mac 3 コア）。
   対象一覧と並列実行は **`scripts/clang-tidy-sdk.sh`** が持ち、両ジョブが同じ
   スクリプトを呼ぶので一覧は 1 か所にしかありません。
2. **ビルドと並走させる**。解析をビルドジョブから独立したジョブへ出したことで、
   ビルドの後ろに積まれなくなりました。
3. **翻訳単位を複数のランナーへ分ける**（`strategy.matrix.shard`）。1 台の中の並列は
   コア数で頭打ちになり、実測で 4 コアのランナーは 4 並列でも 2.5 倍程度しか出ません
   （並列効率 63%）。そこから先を縮めるにはランナーを増やすしかないので、
   `clang-tidy-sdk.sh -s I/N` で対象そのものを分割します。割り当てはラウンドロビンで、
   **全シャードの和がちょうど元の一覧**になります（重複も漏れもありません）。
   分割数を変えるのは `matrix.shard` のリストを 1 か所いじるだけです
   （`strategy.job-total` がそのままスクリプトへ渡ります）。`fail-fast: false` なので、
   片方のシャードで検出が出てももう片方は最後まで走ります — 1 つ直すたびに次が
   出てくる、という進み方を避けるためです。

**ゲートは緩めていません。** `release` ジョブの `needs` には 2 つのビルドジョブに加えて
`tidy-mac` / `tidy-windows` も入っているので、**ビルドが成功していても clang-tidy が
通らなければリリースは公開されません**。解析はビルドと同時に走っているため、この
ゲートを保っても所要時間は増えません。

解析用の compile database は、**その実行がビルドするチャンネル 1 つ**に絞って生成します
（`-DVW_BUILD_CHANNEL`。PR は `dev`、`main` は `stable`）。既定の `both` のままだと
1 ソースにつき database のエントリが 2 つでき、clang-tidy が同じファイルを 2 回解析して
所要時間が倍になっていました（Windows で約 9 分）。チャンネル間の差は `VW_DEV_BUILD`
の定義だけ（`ModuleMain.cpp` / `Extensions/ExtMenu.cpp` の 3 分岐）で、PR が dev 側、
`main` が stable 側を解析するので、パイプライン全体では両方が解析されます。

バージョンについて: SDK 非依存の `lint.yml` と `tidy-mac` は clang 18 に固定して
います。`tidy-windows` だけは**ランナーイメージに入っている LLVM**（現在 20 系）を
そのまま使います — ランナーの MSVC 標準ライブラリヘッダが「Clang 20 以降」を要求する
（`static_assert` と Clang 20 の組み込み関数を使う）ため、clang-cl / clang-tidy が
それを解析できる新しさである必要があるからです。以前は `choco install llvm` で最新版を
入れ直していましたが、実測すると**既に入っているものの入れ直しに 31 秒**かかるだけだった
ので、インストールはやめてバージョンが 20 以上であることを確認するだけにしました
（将来ランナーの LLVM が MSVC ヘッダの要求より古くなったら、パースエラーの山ではなく
その旨のメッセージで落ちます）。Ninja も同様にイメージに入っているものを使います。

`tidy-windows` に vcvars（`msvc-dev-cmd`）のステップもありません。clang-cl は MSVC
ツールチェインと Windows SDK をレジストリ／vswhere から自力で見つけるので、`INCLUDE` /
`LIB` を環境へ流し込む必要がなく、その 12〜17 秒も不要でした（compile database は
どちらでもバイト単位で同一になります）。代わりに clang-cl は**絶対パス**で指定して
います — ランナーの PATH には Visual Studio 同梱の LLVM（`VC\Tools\Llvm\x64\bin`）も
入っており、`clang-cl` という名前がどちらに解決されるかを運任せにしないためです。

> **このジョブの所要時間を測るときの注意:** clang-tidy ステップの実時間は、同じ作業
> でも**ランナーによって 1.4 倍ほど振れます**（同一の 1 翻訳単位が、あるホストでは
> 25 秒、別のホストでは 37 秒）。したがって**2 つの run を比べてもチューニングの
> 良し悪しは分かりません**。実際この節の内容は、その誤りによって一度「修正」され、
> 元に戻された経緯があります。比較するときは A と B を**同一ジョブ内で交互に**測り、
> 最後にもう一度 A を測ってドリフトの対照とすること。

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


## 自動アップデートの仕組み

利用者から見た挙動は [`README.md`](../README.md)「自動アップデート」にあります。ここは
その内部の仕組みです。

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

- **stable（`HomeskzIfcImport` / main）** — **Vectorworks 起動時**に、より新しい安定版
  ビルドがないかを確認します（`src/ModuleMain.cpp` がモジュールロード時に一度だけ実行）。
  - 既に最新なら**何も表示しません**（毎回の起動を邪魔しません）。
  - 新しいビルドがあれば `AlertQuestion` で「インストールしますか？」と尋ね、選ばれた
    場合だけインストールします。インストール後は**再起動を促すのではなく尋ねます**
    （下記）。
  - ネットワーク確認は時間制限付き（`vw-update.sh` の `--max-time`）で、オフラインや
    エラー時は静かに諦めます。

- **dev（`HomeskzIfcImportDev` / ブランチ）** — **Vectorworks 起動時**に、使用するビルドを
  **ネイティブのプルダウンダイアログ**（`VWFC::VWUI::VWDialog` + `VWPullDownMenuCtrl`、
  `src/Updater.cpp` の `CBuildPickerDialog`）で問い合わせます（`src/ModuleMain.cpp` が
  モジュールロード時に一度だけ実行）。1 つのドロップダウンに候補を一覧表示します:
  - 先頭は**現在ロードされているビルド**（branch / commit、「インストール済み」と明示）。
  - 続いて**他のブランチのプレリリース**（現在のビルドと同じコミットは除外）。
  - **インストール済み（先頭）を選ぶ／キャンセル** → 何もせず起動を続けます。
  - **別のブランチを選ぶ** → それをインストールし、続けて再起動を尋ねます（下記）。
  現在の実行ビルドの判定にはコンパイル時に埋め込まれた commit（`VW_BUILD_VERSION`）を
  使うため、ディスク上に別ビルドが未反映で置かれていても取り違えません。

  以前はこの確認を**コマンド実行時**に行っていましたが、プラグインが自身のコマンドを
  プログラム内から再実行しうると毎回ダイアログが出てしまいます。コンパイル済みビルドは
  そもそも起動時にしか差し替わらないため、確認は起動時に一度だけ行います。

### インストール後の再起動（stable / dev 共通）

コンパイル済みプラグインは起動時にしか読み込まれないため、インストールしただけでは
新しいビルドは動きません。そこでインストールが成功したときの表示は**通知ではなく質問**
にしてあり、**「再起動」ボタン**をその場に出します（`src/UpdaterFlow.cpp` の
`OfferRestart`）。

- **「再起動」** → 起動の完了後に Vectorworks を終了し、終了しきってから起動し直します
  （`src/Updater.cpp` の `CVectorworksUpdaterHost::Restart`）。終了要求は OS 経由なので
  **押した直後ではなく、起動が終わってから**効きます。開いているファイルは**通常どおり
  保存を確認**してから閉じられ、保存ダイアログで取り消せば Vectorworks は落ちません
  （その場合もインストール済みのファイルはディスクに残るため、次回の起動で反映されます）。
- **「後で」** → 何もせず起動を続けます。反映は次に Vectorworks を起動したときです。

インストールに失敗したときは（当然）再起動を尋ねず、失敗の理由だけを表示します。再起動を
**用意できなかった**とき（アプリを特定できない／ヘルパーを起動できない）は「手動で再起動して
ください」と案内します——押しても何も起きないように見えるのを避けるためです。

#### 再起動を SDK に任せない理由（実機で確かめた失敗）

SDK の `CloseAllFilesAndQuitVectorworks` には「終了」と「終了後に起動し直す」
（`bRestart`）がありますが、**どちらも使いません**。macOS 実機では次のように失敗しました。

1. `bRestart: true`（終了＋再起動を SDK に任せる）→ 古いインスタンスが終了しきる前に新しい
   インスタンスが立ち上がり、**「サポートファイルの読み込みに失敗しました。」**で落ちる。
2. `bRestart: false`（終了だけ SDK に任せ、起動し直しは自前）→ **同じダイアログが出る**。
   アップデート確認は**プラグインのロード中**（スプラッシュ表示中）に走るため、Vectorworks
   本体がまだ自分を終了させられる状態になっていないため、と考えられます。SDK には
   「起動完了後に実行する」フックが無く（`RegisterNotificationProcedure` の通知一覧にも
   起動完了に相当するものは無い）、いつ呼べば安全かを当てにいくのは筋が悪い。

そこで**終了も起動し直しも、切り離した（detached）ヘルパープロセスに任せます**。プラグイン
がすることは、そのヘルパーを起動することだけです。ヘルパーは

1. **OS の通常の終了要求**を送る（macOS: 自分のバンドル ID 宛の `quit` Apple event ＝ ⌘Q と
   同じもの。Windows: メインウィンドウへ `CloseMainWindow()` ＝ 閉じるボタンと同じもの）。
   OS はこれを**イベントループが回り始めてから**、つまり Vectorworks が処理できる状態に
   なってから配送します——「いつ安全か」を推測する必要がありません。保存の確認も通常どおり。
2. プロセスが**消えるまで待ち**（既定 300 秒。保存ダイアログで取り消して終了しなかった場合は
   あきらめる——使用中のアプリを勝手に起動し直さないため）、
3. 2 秒おいてから **macOS は `open -a <app>`**（LaunchServices 経由＝ダブルクリックと同じ）、
   **Windows は `Start-Process <exe>`** で起動し直します。

ヘルパーに渡すコマンドは**同梱スクリプトのモードではなく、その場で組み立てた 1 行**です
（`src/UpdaterParse.h` の `MacRelaunchCommand` / `WinRelaunchCommand`。純粋関数なので生成
される shell / PowerShell はそのまま単体テストしてあります）。理由は、**インストール直後の
ディスク上のスクリプトは「いま入れたビルドに同梱されていた版」**であり、実行中のコードより
古いことがあるからです。実際に古い版を呼んでしまい
**「エラー: 不明なチャンネル: 'relaunch'」**というダイアログが出ました。インラインのコマンド
なら呼び出し側と食い違いようがありません。

新しいビルドが実際にロードされるのは、この再起動（または手動での再起動）以降です。

インストールそのものは OS ごとに事情が違います。macOS ではバンドルの隔離解除とアドホック
再署名をスクリプトが行います。Windows では実行中の `.vlb` を削除できない（メモリにマップ
されている）ため、スクリプトは古い `.vlb` をいったん退避（リネーム）してから新しいものを
書き込みます。退避ファイル（`*.old-*`）は次回の更新時に掃除します。

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
# stable チャンネル（main → HomeskzIfcImport）:
./scripts/vw-update.sh stable
# dev チャンネル — どのブランチのビルドを入れるか選ぶ（→ HomeskzIfcImportDev）:
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
