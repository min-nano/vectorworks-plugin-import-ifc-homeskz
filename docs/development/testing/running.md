# テストの実行（テストとカバレッジ）

テストはすべて **Vectorworks SDK 無し**で走ります（SDK は約 800 MB のダウンロードを
伴うため）。外部依存のない極小のテストハーネス（`tests/TestFramework.h`）を使うので、
テストフレームワークのダウンロードも不要です。同梱スクリプト・開発ツール・MCP サーバの
テストも、ネットワークや相手側を代役に差し替えて同じ Linux ランナーで回ります。

**何をテストしているか（領域ごとの対応表・方針・テストしていないもの）は
[`tests/README.md`](../../../tests/README.md)** にあります。描画側（`src/draw/`）は SDK と
実図面を要するため単体テストを持たず、実機での目視確認に委ねます（確認の作法は
[「実機確認の作法」](../../dev-notes/live-verification.md)）。

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

CI の `test` ジョブは常にこの設定（に `VW_ENABLE_COVERAGE=ON` と
`VW_REQUIRE_SCRIPT_TESTS=ON` を足したもの）で
テストを回すため、リファクタが招くメモリ不正
（境界外アクセス・use-after-free・リーク）や、updater パーサが GitHub 側の仕様変更で
崩れた入力を誤処理するケースは、その場でビルドを赤にできます。予期しない外部入力に
対する耐性は `tests/UpdaterRobustnessTests.cpp` の擬似ファズ／敵対的入力テストが担い、
サニタイザがその番人になります（詳細は[アップデータのテスト](updater.md)）。

ビルドオプション:

- `VW_BUILD_PLUGIN`（既定 `ON`）… プラグイン（殻と本体）をビルドします（SDK が必要で、
  macOS / Windows のみ）。テストだけをビルドしたいときは `OFF` にします。プラグインの
  ビルドにだけ効く `VW_BUILD_CHANNEL` は[「ローカルでのビルド」](../build.md)。
- `VW_BUILD_TESTS`（既定 `OFF`）… ユニットテストをビルドします。
- `VW_ENABLE_COVERAGE`（既定 `OFF`）… テストに gcov 用の計測を付けます（GCC / Clang）。
- `VW_ENABLE_SANITIZERS`（既定 `OFF`）… テストを ASan + UBSan
  （`-fsanitize=address,undefined -fno-sanitize-recover=all`）でビルド・実行します
  （GCC / Clang）。
- `VW_REQUIRE_SCRIPT_TESTS`（既定 `OFF`）… スクリプトのテストに要るツール（`bash` /
  `pwsh` / `python3` など）が無いときに、SKIP せず失敗させます（CI の `test` ジョブは `ON`。
  [スクリプトのテスト](updater-scripts.md)）。
