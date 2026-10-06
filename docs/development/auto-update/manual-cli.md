# スクリプトを直接実行する（手動 CLI）

リポジトリは公開なので、認証や追加ツールは不要です。各スクリプトは OS 標準のものだけ
を使います — macOS は `curl`・`plutil`・`unzip`・`codesign`・`xattr`・`osascript`
（`osascript` は下記の手動 CLI パスのみ）、Windows は PowerShell 組み込みの
`Invoke-RestMethod` / `Invoke-WebRequest` / `Expand-Archive`。

プラグインを経由せず、スクリプトを直接実行することもできます（手動確認・トラブル
シュート用。macOS の CLI パスは osascript ダイアログ、Windows の CLI パスはコンソール
プロンプトを使います）:

```sh
# --- macOS (bash) -----------------------------------------------------------
# 配置だけを行うインストーラ（リリースのアセットにもあるので、単独で落として実行できる）:
./scripts/vw-install.sh                        # 最新の stable を入れる
./scripts/vw-install.sh --tag dev-feature-x    # そのプレリリースを入れる
./scripts/vw-install.sh --zip <file>           # 手元の zip から入れる
# 取り除く（リリースのアセットにもある）:
./scripts/vw-uninstall.sh                      # 既定の場所から取り除く
./scripts/vw-uninstall.sh --name min-nano_structureDev
# stable チャンネル（main → min-nano_structure）:
./scripts/vw-update.sh stable
# dev チャンネル — どのブランチのビルドを入れるか選ぶ（→ min-nano_structureDev）:
./scripts/vw-update.sh dev
# 引数なし（または Finder でダブルクリック）: 最初にチャンネルを尋ねます。
./scripts/vw-update.sh
# プラグインが内部的に使う非対話モード（ダイアログなし・機械可読出力）:
./scripts/vw-update.sh q-stable                # stable の状態を表示
./scripts/vw-update.sh q-dev                   # dev ビルド一覧を表示
./scripts/vw-update.sh do-install <url> <name> # ダウンロードしてインストール
./scripts/vw-update.sh q-pr-state <branch>...  # ブランチごとに PR が開いているか
```

```pwsh
# --- Windows (PowerShell) ---------------------------------------------------
# 配置だけを行うインストーラ（リリースのアセットにもある）:
powershell -ExecutionPolicy Bypass -File scripts\vw-install.ps1
powershell -ExecutionPolicy Bypass -File scripts\vw-install.ps1 -Tag dev-feature-x
powershell -ExecutionPolicy Bypass -File scripts\vw-uninstall.ps1
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 stable
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 dev
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1          # チャンネルを尋ねる
# 非対話モード（プラグインが使うもの。q-stable/q-dev/do-install/q-pr-state は sh 版と同じ契約）:
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 q-stable
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 q-dev
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 do-install <url> <name>
powershell -ExecutionPolicy Bypass -File scripts\vw-update.ps1 q-pr-state <branch>...
```

環境変数で上書き可能: `VW_REPO`（owner/repo）、`VW_PLUGINS_DIR`（インストール先）。
2 つのチャンネルは別名のプラグインをインストールするので、stable と dev が互いを
上書きすることはありません。
