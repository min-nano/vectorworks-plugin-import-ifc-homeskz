# MCP ブリッジの受け付けを殻の OS タイマーへ移す（M41）

**ご要望**: Vectorworks が起動していなくても起動して、実機テストを始めたい（M40 の続き）。

**何が詰まっていたか。** M40 で Python の MCP サーバが Vectorworks を起動するようにしたが、
実機では起動のあとで橋が架からなかった。原因はブリッジの**時計**にあった——M30〜M40 は
パレットの HTML の `setInterval`（250 ms）から殻を呼んで受け付けていたが:

- **パレットを閉じる・Vectorworks がほかのアプリの裏に回ると、ちょうど 60 秒に 1 回まで
  間引かれた。** 出し直すとすぐ 250 ms に戻る。App Nap から外しても変わらなかった
  （PR #194 の最初の版。`NSProcessInfo beginActivityWithOptions:`）。埋め込みブラウザ（CEF）が
  隠れたページのタイマーを 1 分へ寄せる間引きと一致する。
- **図面が 1 枚も開いていない間は、パレットそのものが出ない**（メニューから出そうとしても
  出ない）。再起動の直後は図面が無いので、人が図面を開いてパレットを出すまで橋が架からない。
  Finder から `.sta` を開いて図面を出しても、パレットは自動では開き直されなかった。

Claude から実機確認を回すとき、Vectorworks はたいてい Claude のアプリの裏にいて、再起動の
直後は図面が無い——どちらも「人の手を待つ」に落ちていた。M38 の実機確認では、たまたま
Vectorworks を前面に置き、パレットを出したまま回していたので気付かなかった。

**SDK リファレンスで調べた**（[issue #204](https://github.com/min-nano/vectorworks-developer-sdk-reference/issues/204)。
[Findings「Timers and Notifications」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Timers%20and%20Notifications.md)）:
SDK にアイドル・周期の口は無く、CEF の間引きを止める口も、図面の無いときにパレットを出す
口も無い。しかし **OS のタイマー（mac: `CFRunLoopTimer` / Windows: `SetTimer`）は、コマンドが
戻った後も・裏に回っても・図面が無くても間引かれずに刻み、その刻みから `gSDK` を読み書き
できる**（mac / Windows とも実測）。

**どうしたか。** 殻（`Extensions/ExtMcpPalette.cpp`）に OS のタイマーを置き
（`StartMcpBridgeClock`）、`plugin_module_main` が開発版でだけ仕掛ける。刻みは従来の
受け付け（`ServeOnce`）をそのまま呼ぶ。パレットは様子を見せる窓になり、出ている間は
JS タイマーも同じ受け付けを呼ぶ（入れ子では入らないよう殻で止める）。

決めごと（どれも同 Findings の実測から）:

- **既定のモード（`kCFRunLoopDefaultMode`）にだけ載せる。** 共通モードに載せると
  Vectorworks のモーダルダイアログの最中にも刻む。その最中は Vectorworks が undo の記録を
  開いていることがあり、**刻みの中の書き込みはその記録へ混ざる**——利用者の 1 回の取り消しが、
  無関係な取り込みごと持っていく。
- **undo の記録が開いている刻みは見送る**（`IsCurrentlyBuildingAnUndoEvent()`）。信用できる
  のは「開いていない」の側だけなので、開いている間は何もしない。開いたままの置き土産で
  見送り続けることはありうるが、そのときもパレットが出ていれば JS の時計が従来どおり受け付ける。
- **間隔は当てにしない**（平均 250 ms 強・数秒空くことがある）。待ち時間は Python 側が時刻で測る。
- **起動の直後は 10 秒待つ**（起動の最中に本体を読み込みに行かない）。
- **App Nap から外すのはやめた**（効かなかった。OS のタイマーは外さなくても裏で刻む）。
- **勝手に取り込む時計ではない。** 刻みが起こすのは Claude が頼んだ道具だけで、M38 の
  「勝手に回る時計を持たない」は変わらない。

**殻の変更なので、入れたあとは Vectorworks の再起動が要る。**
