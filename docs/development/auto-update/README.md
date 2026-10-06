# 自動アップデートの仕組み

（このフォルダのページの一覧は[開発ガイドの目次「自動アップデート」](../../DEVELOPMENT.md#自動アップデート)）

利用者から見た挙動は[「アップデート」](../../user-guide/update.md)にあります。ここは
その内部の仕組みです。

アップデートはコマンドラインではなく、**プラグイン自身がネイティブの Vectorworks
ダイアログ**（`gSDK->AlertInform` / `gSDK->AlertQuestion`、およびドロップダウン選択は
`VWFC::VWUI::VWDialog` + `VWPullDownMenuCtrl`）を表示して行います（`src/Updater.cpp`）。ネットワーク・インストールなどの実処理（GitHub API の参照・
ダウンロード・インストールの取り次ぎ）は**プラットフォームごとの更新スクリプト**
に集約され、ビルド時にインストール物と一緒に**同梱**されます（`Plug-Ins` への配置
そのものは、落とした zip に入っているインストーラへ委ねます。[配置の仕組み](installer.md)）:

- **macOS** — `scripts/vw-update.sh`（bash）。バンドル内の
  `Contents/Resources/vw-update.sh` に入ります。
- **Windows** — `scripts/vw-update.ps1`（PowerShell）。`.vlb` の隣に入ります。

**GitHub を読むときは、トークンがあれば必ず付けます**（`scripts/vw-token.{sh,ps1}` を
source して探します。無ければ従来どおり認証なしで続きます）。公開リポジトリなので認証は
要りませんが、**認証なしの GitHub REST は IP ごとに 1 時間 60 回**です（M24〜M37 の往復の
パレットは 1 分ごとに `q-dev` を呼んでちょうど上限に張り付き、実機で「リリース一覧を取得
できませんでした」として出ました。[M27](../../dev-notes/milestones/m27-api-rate-limit.md)）。トークンは環境変数
`HOMESKZ_IFC_FEEDBACK_TOKEN`・キーチェーン／DPAPI に登録済みのもの・`gh auth token` の順に
探します（登録の口は M38 で `vw-feedback` と一緒に外しました。開発機なら `gh` で足ります）。
失敗したときは HTTP の番号・curl の終了コード・API 制限なら**いつ戻るか**まで `error=` の行に載せます。

## 同梱スクリプトの約束

**スクリプトはどれも殻の ID（`VW_SHELL_ID`）に入れません**（`CMakeLists.txt` の
`VW_SHELL_INPUTS`）。インストーラ／アンインストーラはそもそも殻に入らず、同梱スクリプト
（`vw-update.*` / `vw-token.*`）も**プロセスへ読み込まれず、呼ぶたびにディスクから
読み直される**ので、置き換えれば次の呼び出しから効きます。狙いは「スクリプトを直した
だけで再起動を強いない」こと——それはこの仕組みが無くそうとしている手間そのものです。

**アセット名も決め打ちにしません。** 同梱スクリプトはまず `<プラグイン名>.vwlibrary.zip`
（Windows は `.vlb.zip`）を厳密に探し、無ければ**末尾がその拡張子のアセット**で拾い
直します（`plugin_zip_url` / `Get-PluginZipUrl`）。名前を変えた瞬間に、古いアップデータ
から何も落とせなくなる——つまりアップデートの経路そのものが切れる——のを避けるためです。

プラグインはこのスクリプトを**非対話モード**（`q-stable` / `q-dev` / `do-install`。
開発版の実機テストは一時ファイルの片付けに `q-pr-state` も使う。[一時ファイルと片付け](../live-test/scratch-files.md)）で
呼び出して結果を受け取り、ユーザーへの表示はすべて自前のネイティブダイアログで行う
ため、利用者がターミナルを開く必要はありません。どちらの OS でも
`src/Updater.cpp` の同じフロー・ダイアログが動き、変わるのは「自分の場所を特定する
方法（macOS は `dladdr`、Windows は `GetModuleFileName`）」と「起動するスクリプト」
だけです。
