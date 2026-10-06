# ローカルでのビルド

CMake 3.20+ と、対象プラットフォームの **Vectorworks 2026 SDK** が必要です。SDK は
`VW_SDK_DIR` を **`SDKLib` を含むフォルダ**に向けて渡します（`-DVW_SDK_DIR=...` また
は環境変数）。

## macOS

Xcode（Vectorworks 2026 は公式に **Xcode 16.2** を対象）と **mac SDK** が必要です。

1. SDK をダウンロードして展開します:
   <https://release.vectorworks.net/latest/Vectorworks/2026-NNA-eng-mac-SDK.zip>
   （約 800 MB）。展開すると `SDKLib/` を含むフォルダができます。

2. コンフィグとビルド:

   ```sh
   cmake -S . -B build -DVW_SDK_DIR=/path/to/2026-NNA-eng-mac-SDK
   cmake --build build --config Release
   ```

   成果物は `build/min-nano_structure.vwlibrary` です。

既定では Apple Silicon（`arm64`）向けにビルドします。ユニバーサルバイナリにするには:

```sh
cmake -S . -B build -DVW_SDK_DIR=/path/to/sdk \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

## Windows

Visual Studio 2022（v143 ツールセット、x64）と **win SDK** が必要です。

1. SDK をダウンロードして展開します:
   <https://release.vectorworks.net/latest/Vectorworks/2026-NNA-eng-win-SDK.zip>
   展開すると `SDKLib/` を含むフォルダができます。

2. コンフィグとビルド（既定の Visual Studio ジェネレータを使います）:

   ```pwsh
   cmake -S . -B build -A x64 -DVW_SDK_DIR=C:/path/to/2026-NNA-eng-win-SDK
   cmake --build build --config Release
   ```

   成果物は `build/Release/min-nano_structure.vlb`（DLL）と、その隣の
   `build/Release/min-nano_structure.vwr`（リソース）です。ビルドスタンプの
   `min-nano_structure.commit` / `min-nano_structure.branch`（どのコミット・どのブランチの
   ビルドか）と更新スクリプト `vw-update.ps1` も同じ場所に出力されます。

macOS の `.vwlibrary` バンドルと違い、Windows のプラグインは `<name>.vlb` 本体と
同名の `<name>.vwr` を**同じフォルダに一緒に**置く必要があります（`.commit` / `.branch` /
`.shell-id` と `vw-update.ps1` も同梱すると自動アップデートが機能します）。

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
