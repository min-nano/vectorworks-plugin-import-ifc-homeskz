# プラグイン識別子

このプラグインを一意に識別する値は次の通りです（出発点にしたネイティブプラグイン
テンプレート `vectorworks-plugin-native-template` のプレースホルダーを、これらへ
置き換えてあります）。フォークして別プラグインを作るときは、同じ箇所を自分の値へ
置き換えます。

| 種別 | 値 | 場所 |
| --- | --- | --- |
| プラグイン表示名 | `みんなの構造設計支援` / `みんなの構造設計支援Dev` | `resources/*/Strings/*.vwstrings`（コマンド名の中）、`src/UpdaterFlow.cpp`（ダイアログの文言）、`src/Extensions/ExtMcpPalette.cpp`（パレットの題）、`src/draw/Feedback.cpp`（実機テストの結果の題） |
| バンドル／出力名 | `min-nano_structure` / `min-nano_structureDev` | `CMakeLists.txt`、`src/BuildConfig.h`、`src/UpdaterFlow.cpp`、`resources/` フォルダ名、`scripts/vw-update.sh`、`scripts/vw-update.ps1`、`scripts/mcp/vw-mcp-server.py`（既定のプラグイン名）、`.github/workflows/build.yml`（`scripts/vw-install.*` は名前を決め打ちせず、アーカイブから読み取ります） |
| CMake のプロジェクト／ターゲット名 | `MinNanoStructure(Dev)` / `MinNanoStructureCore` | `CMakeLists.txt`、`tests/CMakeLists.txt` |
| バンドル ID（macOS） | `io.github.min-nano.structure` / `io.github.min-nano.structure-dev` | `CMakeLists.txt` |
| メニューカテゴリ | `みんなの構造設計支援` / `みんなの構造設計支援Dev`（コマンド名 `IFC (ホームズ君) 取り込み…` / `アップデータを確認 (みんなの構造設計支援)` / `MCP ブリッジを表示… (Dev)` / `実機テストを実行… (みんなの構造設計支援Dev)`。後ろの 2 つは dev だけに出る）。**このプラグインのコマンドは全部このカテゴリに入れる**——`.vwr` の `"category"` ただ 1 つを全メニュー定義が引く | `resources/*/Strings/*.vwstrings` |
| C++ 名前空間・クラス | `HomeskzIfcImport` / `CExtMenuImportIfc` / `CExtMenuCheckUpdate` / `CExtMenuMcpBridge` / `CExtMenuTest` / `CExtMcpPalette` / `CExtColumnMark` / `CExtShearWall` | `src/Extensions/Ext*.{h,cpp}`、`src/ModuleMain.cpp` |
| VCOM ユニバーサル名 | 取り込み: `CExtMenuImportIfc_HomeskzIfcImport(Dev)`／更新: `CExtMenuCheckUpdate_MinNanoStructure(Dev)`／MCP: `CExtMenuMcpBridge_MinNanoStructure(Dev)`／実機テスト: `CExtMenuTest_MinNanoStructure(Dev)`（登録は dev だけ）／MCP パレット: `CExtMcpPalette_MinNanoStructure(Dev)`（MCP の 2 つも登録は dev だけ。M38）。M24〜M37 の往復パレット `CExtFeedbackPalette_MinNanoStructure(Dev)` は廃止（綴りを別の拡張へ使い回さない） | `src/BuildConfig.h` |
| PIO のユニバーサル名 | 柱記号: `HomeskzColumnMark(Dev)`／耐力壁: `HomeskzShearWall(Dev)`。**安定版と開発版で必ず分ける**（同じだと両方を入れた環境で片方の登録しか生きず、安定版の取り込みが開発版の PIO を置く） | `src/Extensions/ExtColumnMark.h` / `src/Extensions/ExtShearWall.h` |
| 拡張機能 UUID | コマンド 4 つ（取り込み・更新・MCP・実機テスト）× stable / dev の 8 個＋PIO 2 つ × 2＋MCP パレット × 2（廃止した往復パレットの 2 個は使い回さない） | `src/Extensions/Ext*.cpp`（一意である必要があるため `uuidgen` で再生成） |
| リポジトリ | `min-nano/vectorworks-plugin-import-ifc-homeskz` | `scripts/vw-update.{sh,ps1}` / `scripts/vw-install.{sh,ps1}` の `VW_REPO` 既定値 |

> **名前空間 `HomeskzIfcImport` と取り込みコマンドのユニバーサル名・UUID は、改名後も
> 据え置いています。** ユニバーサル名と UUID は**コマンドの同一性そのもの**で、付け替えると
> 利用者のワークスペースからコマンドが消えます（登録し直しになります）。名前空間は
> 図面にも配布物にも現れない内部の綴りなので、改名の巻き添えで 200 ファイル超を書き換える
> 価値がありません。

`.vwstrings` は UTF-16LE（BOM 付き・CRLF 改行）です。編集時はエンコーディングを保持
してください。現在の識別子は次で一覧できます。

```sh
grep -rniE "homeskzifcimport|io\.github\.min-nano|CExtMenuImportIfc|CImportIfcMenu" \
  --exclude-dir=.git .
```
