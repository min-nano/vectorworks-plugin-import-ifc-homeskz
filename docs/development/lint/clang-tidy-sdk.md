# SDK 依存コードの静的解析（`tidy-mac` / `tidy-windows`）

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
うち 6 分 40 秒）がこの 1 ステップでした。そこで 4 段階で速くしています。

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
4. **前と同じ入力の翻訳単位は解析ごと飛ばす**（`clang-tidy-sdk.sh -c`）。
   [「clang-tidy の結果キャッシュ」](clang-tidy-cache.md)。

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
