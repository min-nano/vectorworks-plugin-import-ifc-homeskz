# テストの実行（テストとカバレッジ）

テストはすべて **Vectorworks SDK 無し**で走ります（SDK は約 800 MB のダウンロードを
伴うため）。外部依存のない極小のテストハーネス（`tests/TestFramework.h`）を使うので、
テストフレームワークのダウンロードも不要です。対象は 2 系統あります。

- **インポート機能の解析側**（`src/core/` + `src/parse/`）… 2 フェーズ分離により SDK に
  触れないので、実際のホームズ君 IFC（`tests/fixtures/`）に対して要素ごとに単体テスト
  します（STEP リーダ・幾何・通り芯・ストーリ・床・垂木・野地板・構造クラス判定…）。
  描画側（`src/draw/`）は SDK と実図面を要するため単体テストを持たず、実機での目視確認に
  委ねます（確認の作法は[「実機確認の作法」](../../dev-notes/live-verification.md)）。
- **アップデータ**（`src/Updater.cpp`）… SDK に依存しない純粋なロジック（スクリプト出力の
  パース、コマンドラインのクォート、インストール先パスの導出）を `src/UpdaterParse.h` に
  切り出し、更新フロー本体は `IUpdaterHost` のフェイク越しに丸ごと動かします。同梱
  スクリプトのバックエンド（`q-stable` / `q-dev` / `do-install`）も、ネットワーク境界だけを
  差し替えて SDK ／ネットワーク抜きにテストします — macOS 版 `scripts/vw-update.sh` は
  `tests/vw-update.test.sh`（bash＋`curl`/`plutil` スタブ）、**配置を担うインストーラ**
  `scripts/vw-install.sh` は `tests/vw-install.test.sh`、**取り除くアンインストーラ**
  `scripts/vw-uninstall.sh` は `tests/vw-uninstall.test.sh`、Windows 版はそれぞれ
  `tests/vw-update.Tests.ps1` / `tests/vw-install.Tests.ps1` /
  `tests/vw-uninstall.Tests.ps1`（PowerShell 7＋
  `Invoke-GH`/`Invoke-WebRequest` スタブ）で、いずれも Linux ランナー上で動きます。
  更新の流れそのもの（尋ねる・入れる・再起動を尋ねる／本体だけ読み直す）は
  `IUpdaterHost` の偽物を差し込んで `tests/UpdaterFlowTests.cpp` で検証します——
  **手動確認と取り込み時確認の違い**（最新・オフラインを伝えるか黙るか）もここです。

**テストの一覧・方針・何をテストしていないかは [`tests/README.md`](../../../tests/README.md)** に詳しくあります。

ローカルでの実行（SDK 不要）:

```bash
cmake -S . -B build-tests -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON
cmake --build build-tests --parallel "$(nproc)"
ctest --test-dir build-tests --output-on-failure -j "$(nproc)"
```

`--parallel` / `-j` を付けないと、cmake も ctest も**既定で 1 コアしか使いません**。
テストは 20 本以上の独立した実行ファイルなので、コア数を渡すだけで素直に短くなります
（CI の `test` ジョブも同じ指定で回します。[「カバレッジレポート」](../ci/coverage.md)）。

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
サニタイザがその番人になります（詳細は[アップデータのテスト](updater.md)）。

ビルドオプション:

- `VW_BUILD_PLUGIN`（既定 `ON`）… プラグイン本体をビルドします（SDK が必要で、
  macOS / Windows のみ）。テストだけをビルドしたいときは `OFF` にします。
- `VW_BUILD_TESTS`（既定 `OFF`）… ユニットテストをビルドします。
- `VW_ENABLE_COVERAGE`（既定 `OFF`）… テストに gcov 用の計測を付けます（GCC / Clang）。
- `VW_ENABLE_SANITIZERS`（既定 `OFF`）… テストを ASan + UBSan
  （`-fsanitize=address,undefined -fno-sanitize-recover=all`）でビルド・実行します
  （GCC / Clang）。
