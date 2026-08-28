//
//	draw/DrawUtil.h
//
//	Phase 2（VW 描画）の共通ヘルパー。要素ごとの draw モジュール（Grid / Story / Floor /
//	Rafter / Roof …）が等しく行う定型——クラス分け・描画属性の by-class 化・配置先レイヤの
//	用意——を 1 か所に集める。
//
//	【なぜ要るか】これらは以前、各 .cpp の無名名前空間に**逐語的な複製**として置かれていた
//	（SetClassByName は 4 か所、SetAllAttributesByClass は 3 か所）。属性を 1 つ足す・
//	by-class の指定を直すといった変更が、直した .cpp でしか効かない形になっていた。
//
//	【SDK 依存・include の順序】このヘッダは draw/StructuralMember.h とともに**SDK 型を
//	公開する共通ヘッダ**（MCObjectHandle を引数に取るため）。自分で PluginPrefix.h を
//	include するので、draw/*.cpp のどこから include しても成立する。逆に、要素ごとの
//	draw/*.h は従来どおり core::Document.h までしか参照しない（SDK を持たない翻訳単位＝
//	Extensions/ExtMenu から安全に include できるようにするため。CLAUDE.md「依存の向きは
//	厳守する」）。したがって**このヘッダを draw/*.h から include してはならない**。
//

#pragma once

#include "PluginPrefix.h"

#include "core/Document.h"
#include "core/Layout.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// オブジェクトのクラスを名前で設定する。AddClass は既存なら索引を返し、無ければクラスを作
	// る。クラス名が空なら何もしない（無クラス＝既定クラスのまま）。
	void SetClassByName(MCObjectHandle object, const std::string& className);

	// 描画属性（線幅・色・パターン・矢印・透明度）をすべてクラス属性に従わせる。
	// SetObjectClass はクラスを割り当てるだけで各属性は by-instance の既定値のまま残るため、
	// 属性ごとに by-class を指定する（ISDK の関数名は VS と異なる: PColors=ペン色 /
	// FColors=面色 / PPat=線種 / FPat=面パターン / Arrow=マーカー）。
	void SetAllAttributesByClass(MCObjectHandle object);

	// PIO のパラメータ名を解決する。universal 名で見つかればそれを使い、見つからなければ
	// ローカライズ名（OIP に出る日本語）で引き直す。**名前が 1 つ違うだけで setter は黙って
	// 無視される**（M6 の垂木で実証済み: 勾配・構造用途・ラベルが名前違いで効いていなかった）
	// ため、確実に見つかる方を選ぶ。どちらでも見つからなければ universal 名をそのまま返す。
	TXString ResolveParamName(const VWParametricObj& pio, const char* universalName,
							  const char* localizedName);

	// PIO の文字列パラメータを読む（無ければ・例外なら空）。**PIO 本体（Extensions/）が
	// 自分や他のオブジェクトのパラメータを覗くときの唯一の入口**——柱記号（ExtColumnMark）と
	// 耐力壁（ExtShearWall）がどちらも構造材の構造用途を読むので、try/catch ごとここに
	// 1 つだけ置く。
	std::string PioParamString(const VWParametricObj& pio, const char* name);

	// オブジェクトが構造材ツール（StructuralMember）なら、その**構造用途**（"4"＝柱 /
	// "5"＝小屋束 …。core::kStructuralUse*）を返す。構造材でなければ空。
	//
	// 記号 PIO と耐力壁 PIO が「対象レイヤの中から柱だけを拾う」のに共有する。
	std::string StructuralUseOf(MCObjectHandle object);

	// PIO に実数パラメータを書き、**読み戻して書けたか確かめる**。書けていれば true。
	// 角度・寸法のような数値パラメータでも、PIO の登録次第で実数ではなく文字列として
	// 保持されていることがあり、その場合 SetParamReal は黙って無視される。そこで実数で
	// 書けなかったときは文字列で入れ直す（M6 の垂木で「寸法を文字列で渡すと既定値のまま
	// だった」逆のケースが起きており、どちらに転んでも入るようにする）。
	bool SetParamRealChecked(VWParametricObj& pio, const TXString& param, double value,
							 double tolerance = 1e-6);

	// 構造材ツール（StructuralMember）へ渡す**断面プロファイルのグループ**を作る。
	// 矩形（[minX, minY]〜[maxX, maxY]）1 枚を閉じたポリゴンとしてグループへ入れて返す。
	// 矩形の位置は呼び出し側の**断面基準点の規約**で決まる: 横架材は天端中央基準なので原点が
	// 上辺中央、柱は断面中心基準なので原点が中心（AxisAlign の設定と一致させる）。
	//
	// **グループへは VWFC の VWGroupObj::AddObject で入れる**。gSDK->AddObjectToContainer を
	// 直に呼ぶと「レイヤに作ってから移す」形になり、移動に失敗すると**空のグループ**が残る。
	// 空のプロファイルは断面が無いのと同じで、PIO は生成できても実体が描かれない（＝
	// オブジェクトはあるのに画面に何も出ない）。入ったかどうかを GetFirstMemberObject で
	// 確かめ、空なら nil を返すので、呼び出し側はフォールバックへ回せる。
	// 幅・せいが 0 以下なら nil。
	MCObjectHandle CreateRectangleProfileGroup(double minX, double minY, double maxX, double maxY);

	// 名前付きプラグインスタイル（"木質構造材_横架材" 等）の RefNumber を引く。文書に無ければ
	// 0 を返す（＝スタイル無しで描く。スタイルの欠落で部材を失わない）。
	//
	// ISDK はスタイル名から RefNumber を引く呼び出しを持たないので、名前付きオブジェクト
	// （プラグインスタイルはシンボル定義）を GetNamedObject で引き、その InternalIndex を
	// RefNumber として渡す（どちらも SysName を表す Sint32。SDK ヘッダでも InternalIndex と
	// RefNumber は相互に渡し合う形で使われている）。
	RefNumber ResolvePluginStyle(const TXString& styleName);

	// 平面外形を閉じた 2D ポリゴンとして作る（スラブのプロファイル・フォールバック描画）。
	// 頂点が空なら nil。
	MCObjectHandle CreateClosedPolygon(const std::vector<core::Vec2>& boundary);

	// その名前のシンボル定義が図面に在るか（例外は false として扱う）。
	// **シンボル置換系（draw/Symbol）と耐力壁の伏図記号（draw/ShearWall）が共有する
	// 唯一の判定**。前者は「置けなかった理由を診断へ書き分ける」ために、後者は
	// 「無いときだけ登録する（在れば利用者が描き直したものとして触らない）」ために使う。
	bool HasSymbolDefinition(const std::string& name);

	// --- 複合オブジェクトの構成（スラブ＝床板 M5・底盤 M9／壁＝立上り M9 が共有する作法）---
	//
	// 床（draw/Floor）と底盤（draw/Footing）は**同じ手順**でスラブを描く（外形ポリゴン →
	// CreateSlab → クラス → 構成層・基準面 → SetSlabHeight → バインド → ResetObject）。
	// 違うのは構成層の中身だけなので、SDK を叩く部分はここに 1 つだけ置く（かつては
	// draw/Floor.cpp の無名名前空間にあり、底盤が同じものを 2 つ目に書く形になっていた）。
	//
	// ★**スタイルは作らない・当てない**（スラブ・壁とも）。構成層・基準面は**各オブジェクトへ
	// 直接**設定し、オブジェクトはスタイル無し（unstyled）のままにする。当初は「厚みごとに
	// スタイルを新規作成して当てる」形だったが、
	//   * インポートのたびに名前付きリソース（スラブ／ウォールスタイル）が増える。undo では
	//     消えないので、取り消してもリソースだけが残る（DrawUtil.h「取り込み全体の Undo」）。
	//   * 名前が埋まっていれば " (2)" … と連番になり、同じ構成のスタイルが図面に並ぶ。
	//   * 構成そのものは命令が全て持っているので、スタイルという間接段を挟む必要が無い。
	// という理由で、オブジェクトへ直接与える形へ改めた。個々のオブジェクトを後から手で
	// 編集しても他へ波及しない（スタイルで束ねていたときとの唯一の差）。

	// オブジェクト（スラブ／壁）の構成層を命令どおりに作り直す。
	//
	// 手順: **先頭に命令の層を順に挿入し、その後ろに残った元の層を削除する**。
	//   * 厚み 0 の層は VW が受け付けない（＝「潰す」では構成を確定できない）ので、
	//     余った層は必ず削除する。
	//   * 先に挿入してから削除するので、途中で層が 0 枚になる瞬間が無い
	//     （層が 1 枚も無いスラブ／スタイルは作れない）。
	//   * 削除に失敗したら（想定外の API 挙動）そこで打ち切り、元の層が残ったままでも
	//     スラブ自体は残す（1 枚の失敗で全体を止めない）。
	//
	// ★コンポーネントの索引は **0 始まり**（実機で確認: 索引 1 に挿入すると既定層の後ろへ
	// 入り、索引 = 層数 で削除すると範囲外で失敗した）。GetNumberOfComponents が返すのは
	// 「個数」なので、有効な索引は 0 … 個数−1。
	//
	// 併せて**層ごとのクラス（素材）と by-class 属性**を設定する。命令の drawClass
	// （"z構成要素-コンクリート" 等。core/Document.h「構成要素のクラス」）を層へ割り当て、
	// 層が持つ描画属性——**塗り**（SetComponentUseFillClassAttr）と**左右のペン**
	// （SetComponentUsePenClassAttr）——をすべてそのクラスの属性に従わせる。オブジェクト
	// 本体に対する SetClassByName ＋ SetAllAttributesByClass と同じ意図で、クラスを
	// 割り当てただけでは挿入時の既定値（0）が by-instance のまま残るため、明示的に指定する。
	void SetComponents(MCObjectHandle object,
					   const std::vector<core::ComponentCommand>& components);

	// スラブ（またはスラブスタイル）の高さ基準面を設定する。基準面は「どの構成要素か」
	// （SetDatumSlabComponent）＋「その上端か下端か」（SetComponentDatumIsTopOfComponent）の
	// 2 つで決まる。スラブスタイル設定ダイアログの「基準面」欄のポップアップとラジオが
	// それぞれこの 2 つに対応する。
	//   Top    … 最上層（索引 0）の**上端**＝スラブ天端
	//   Bottom … 最下層（索引 個数−1）の**下端**＝スラブ底面
	void SetSlabDatum(MCObjectHandle object, core::SlabDatum datum, short componentCount);

	// 壁（スタイル無し）へ構成層を組む。**スラブと違って基準面は持たず**、構成層の合計が
	// そのまま壁厚になる（立上りはコンクリート 1 層＝壁厚。parse/Footing）。
	//
	// 併せて**コア構成要素**を指定する（VW が結合部で構成要素を融合する基準になる。指定が
	// 無いと壁結合しても平面で層が繋がらず、取り合いに面線が残りうる。docs/DEV-NOTES.md M10）。
	// 立上りは構成が 1 層なので、その 1 枚（索引 0）がコアになる。索引は SetComponents と
	// 同じ **0 始まり**（上記 ★）と解釈している——VS 版 SetCoreWallComponent の説明は
	// 「0 はコア無しにする」だが、VS の構成要素索引は 1 始まりで ISDK のそれは 0 始まりなので、
	// ここは先頭の層を指す。スタイルへ同じ呼び出しをしていた M10 でも、これが原因で困った
	// 事象は出ていない（T 字の面線の原因は ResetObject 不足だった。docs/DEV-NOTES.md M10）。
	// 構成層が空なら何もしない。
	void SetWallComponents(MCObjectHandle wall,
						   const std::vector<core::ComponentCommand>& components);

	// --- 取り込み全体の Undo（docs/DEV-NOTES.md M15）--------------------------------------
	//
	// 【なぜレイヤを記録するのか】VectorWorks は取り込みの開始時に undo イベントを開かない
	// （実機の診断ログで確認。docs/DEV-NOTES.md M15）。そこで**自分でイベントを開き**、
	// `AddAfterSwapObject` で「あとで消してよいもの」を登録する。登録するのは
	// **このインポートが新しく作ったレイヤだけ**——レイヤを消せばその上の図形も消えるので、
	// 図形を 1 つずつ登録する必要は無く、**二重登録（レイヤと中身の両方）で undo が既に
	// 消えたものを消しにいく事故**も避けられる。
	//
	// 取り込みが作らないもの（クラス・ストーリ・レベルテンプレート）はリソースであり、
	// undo では戻らない。空のクラスが残るが、図面の見た目は取り込み前に戻る。
	//
	// 【既にあったレイヤ】2 回目の取り込みのように、**取り込み前から在ったレイヤ**へ描いた
	// 分は登録できない（そのレイヤごと消すわけにいかない）。その場合は取り消しが部分的に
	// なるので、完了ダイアログでその旨を伝える（ImportUndoScope::partial）。

	// 取り込みの図面変更をまるごと包む undo イベント。**構築で開き、破棄で閉じる**
	// （途中で例外が出ても閉じる）。記録が 1 件も無ければ、閉じるときにイベントごと
	// 捨てる——空のイベントを残すと「取り消し」が中途半端に効いて図面が壊れるため
	// （実機で確認。docs/DEV-NOTES.md M15）。
	class ImportUndoScope final
	{
	public:
		ImportUndoScope();
		~ImportUndoScope();
		ImportUndoScope(const ImportUndoScope&) = delete;
		ImportUndoScope& operator=(const ImportUndoScope&) = delete;
		ImportUndoScope(ImportUndoScope&&) = delete;
		ImportUndoScope& operator=(ImportUndoScope&&) = delete;

		// 取り消しで戻せる状態か（レイヤを 1 つでも登録できたか）。
		bool armed() const
		{
			return !fCreatedLayers.empty();
		}

		// **取り込み前から在ったレイヤ**へも描いたか（＝取り消しはその分だけ戻らない）。
		bool partial() const
		{
			return fUsedExistingLayer;
		}

	private:
		// 記録の実体はスコープが持ち、下の 2 つの自由関数が「いま開いているスコープ」を
		// 通して書き込む（要素側の draw モジュールはスコープを持ち回らずに済む。
		// インポートはメインスレッドから 1 本しか走らないので、開いているスコープは高々 1 つ）。
		friend void RecordCreatedLayer(MCObjectHandle layer);
		friend void NoteExistingLayerUsed(MCObjectHandle layer);

		bool contains(MCObjectHandle layer) const;

		std::vector<MCObjectHandle> fCreatedLayers; // このインポートが作ったレイヤ
		bool fUsedExistingLayer = false; // 取り込み前から在ったレイヤへも描いた
	};

	// このインポートが新しく作ったレイヤを undo イベントへ登録する（デザイン／シートの
	// どちらも）。イベントが開いていなければ何もしない。nil は無視。
	void RecordCreatedLayer(MCObjectHandle layer);

	// 取り込み前から在ったレイヤへ描いたことを控える（取り消しが部分的になる）。
	// レイヤを用意するヘルパー（下記 3 つ）が自分で呼ぶので、要素側は意識しなくてよい。
	void NoteExistingLayerUsed(MCObjectHandle layer);

	// **SDK に渡して消費させる下ごしらえのオブジェクト**（PIO のパス・プロファイル等）を
	// 「このインポートが追加したもの」として undo イベントへ申告する。
	//
	// 【なぜ要るか】通り芯は `CreateCustomObjectPath` にポリライン（パス）を渡して PIO を
	// 作る。SDK はそのポリラインを **undo 記録つきで削除**して PIO へ取り込むため、こちらが
	// イベントを開いていると「削除」がその記録に入り、**取り消しでポリラインが復活する**
	// （実機で確認: 取り込み直後は PIO だけなのに、取り消すとレイヤ「共通」に曲線だけが残った）。
	//
	// 対処は SDK の作法どおり「**自分が追加したものは申告する**」——`AddAfterSwapObject` の
	// 説明は "Use this callback after you add an object in your routine. A reference to h is
	// stored in the undo table, and that object is deleted when Undo is selected."
	// つまり申告しておけば、取り消しのときに**復活したそれが改めて消える**。
	//
	// レイヤ（RecordCreatedLayer）と違い、**レイヤの上に普通に置いた図形へは使わない**
	// ——レイヤごと消えるものを二重に登録しない（DrawUtil.h「なぜレイヤを記録するのか」）。
	// 使うのは「SDK へ渡して消えるもの」だけ。
	void RecordCreatedObject(MCObjectHandle object);

	// 名前付きデザインレイヤを取得（無ければ作成）してアクティブにする。以後に生成する
	// オブジェクトはこのレイヤへ入る。取得・生成できなければ nil を返し、カレントレイヤも
	// 変えない。**通り芯の "共通" レイヤのように、その要素が自分で用意してよいレイヤ専用。**
	MCObjectHandle PrepareLayer(const std::string& layerName);

	// 既存の名前付きデザインレイヤをアクティブにする。**存在しなければ何もせず nil**（レイヤ
	// を作らない）。ストーリ由来のレイヤ（"1-FL" / "n-垂木" / "n-野地板"）は story
	// 命令が作るので、無い＝そのストーリの生成がスキップされたということ。要素のために勝手に
	// レイヤを作らない。
	MCObjectHandle ActivateExistingLayer(const std::string& layerName);

	// --- シートレイヤとビューポート（伏図＝M13・軸組図＝M14 が共有する作法）------------
	//
	// 伏図（draw/Sheet）と軸組図（draw/Section）は、ビューポートの**種類が違うだけ**で
	// 前後の手当ては同じ（シートレイヤを用意 → 生成 → 表示レイヤを絞る → クラスを表示に
	// 戻す → 縮尺 → 図面タイトル・図番 → 更新）。SDK を叩く部分はここに 1 つだけ置く。
	// 断面ビューポートも例外ではなく、ISDK::CreateSectionViewport のコメントが
	// 「クラス・レイヤの表示はこの呼び出しでは扱わない。呼び出し後に設定し、そのあとで
	// ビューポートを更新すること」と明記している。

	// ビューポート共通の下ごしらえ。図面の全レイヤと、**表示に戻すクラス**の索引を持つ。
	//
	// 【クラスを表示へ戻す理由】ビューポートはクラスの表示を明示しないと**非表示のまま**（M13
	// のローカル確認で判明。レイヤは命令どおりなのに図形が 1 つも出なかった）。そこで**
	// ドキュメントの全クラスを表示へ戻す**——列挙は VWClass::ForEachClass（＝ISDK::
	// ForEachClass の VWFC 版）で行う。
	//
	// **［訂正の記録］**M13 では「ISDK にドキュメントの全クラスを列挙する呼び出しが無い」と
	// 判断し、図形が身に付けているクラスを全レイヤ走査で数え上げ、命令セットが名乗るクラス名
	// （当時の core::documentClassNames）も保険で足していた。**この前提が誤りで**、SDK には
	// ForEachClass がある（sdk-grep で確認）。走査による推し量りは、拾い漏れれば図形が消える
	// うえに、ビューポート注釈のように後から足したものを別経路で拾い直す必要もあった。
	// 全クラス表示なら「どのクラスが要るか」を推し量る必要そのものが無くなる。
	//
	// classes は**昇順・重複なしの vector**（集合として使うが std::set では持たない）。
	// Windows の clang-tidy が std::set を持つ構造体の暗黙の特殊メンバに
	// bugprone-exception-escape を出すため、列挙中だけ set を使い、結果は vector へ移す
	// （用途は「1 つずつ表示へ戻す」走査だけなので、連続領域の方が素直でもある）。
	struct ViewportSetup
	{
		std::vector<MCObjectHandle> layers;
		std::vector<InternalIndex> classes;
	};

	// 上の下ごしらえを行う。図面の規模なりに走査するので、**ビューポートを作るフェーズごとに
	// 1 回だけ**呼ぶこと（伏図・軸組図がそれぞれ 1 回。フェーズをまたいで持ち回さないのは、
	// 要素ごとの draw/*.h に SDK 型を出さない約束を守るため。DrawUtil.h 冒頭参照）。
	ViewportSetup PrepareViewportSetup();

	// シートレイヤを用意する（同じ番号のものがあれば再利用）。**シートレイヤ番号はレイヤ名が
	// 担う**。タイトルはレイヤの説明＝オブジェクト変数 159（ovLayerDescription。"only used
	// for sheet layers"）へ入れる。用意できなければ nil。
	MCObjectHandle PrepareSheetLayer(const std::string& number, const std::string& title);

	// ビューポートで**いまドキュメントにある全クラス**を表示へ戻す（戻せた数を返す）。
	// ConfigureViewport が使うのと同じ列挙・同じ表示種別で、**ビューポートを仕上げた後に
	// 増えたクラス**を拾い直すためのもの——注釈へ後から置いたデータタグは、スタイルが
	// 決める中身と一緒に新しいクラスを文書へ持ち込むことがある（draw/Tag）。
	std::size_t ShowAllViewportClasses(MCObjectHandle viewport);

	// ビューポートの投影をどう扱うか（ConfigureViewport の引数）。
	//
	// 【伏図は 2D/平面（Top/Plan）へ作り直す必要がある】`CreateViewport` が作る平面
	// ビューポートは、**オブジェクト情報パレット上は「2D/平面」と表示されるのに、
	// 実際の描画は 3D の「上」ビューのまま**という食い違いを起こす（実機で確認された症状。
	// 更新ボタンを押しても直らず、パレットでいったん「上」を選んでから「2D/平面」
	// へ戻すと正しく描かれる）。対処は**ユーザーの手動対処をそのまま SDK でなぞる**こと——
	// Project 2D（オブジェクト変数 1005）をいったん OFF にして**更新を挟み**、再度 ON
	// に戻して 2D/平面のキャッシュを作り直す。
	//
	// **軸組図（断面ビューポート）は Keep**——あちらは断面の向きで作られており、平面へ
	// 倒しては意味を成さない。
	enum class ViewportProjection
	{
		Keep, // いまの投影のまま触らない（軸組図＝断面ビューポート）
		Plan, // 2D/平面（Top/Plan）へ作り直す（伏図）
	};

	// ConfigureViewport の結果。**どちらも「効かなかったこと」を呼び出し側の診断行へ
	// 出すためのもの**で、図そのものは失敗しても残る。
	struct ViewportFinish
	{
		// 表示へ戻せたクラスの数（0 なら図形が 1 つも映らない）。
		std::size_t classesApplied = 0;
		// 2D/平面へ作り直せたか（`ViewportProjection::Keep` のときは常に true）。
		// **書けたかどうかは読み戻して確かめる**——SDK の setter は書けなかったときも
		// 黙って何もしないので、「設定したつもりで効いていない」は目視では見抜けない。
		bool planViewApplied = true;
	};

	// 生成済みのビューポートを命令どおりに仕上げる（表示レイヤの絞り込み → クラス表示 →
	// 縮尺 → ［伏図なら 2D/平面の作り直し］→ 図面タイトル・図番 → 更新）。
	//
	// 表示レイヤは「まず全部隠してから、命令に挙げたものだけ表示へ戻す」——ビューポートは
	// 既定でドキュメントの表示状態を引き継ぐため、挙げていないレイヤが映り込む。グレー表示
	// （2）は薄く残るので使わず、必ず非表示（1）にする。
	//
	// **投影の作り直しは「表示レイヤを絞った後・最後の更新の前」**に行う（上の
	// ViewportProjection）。作り直しは更新を 1 回挟むので、レイヤを絞る前に行うと図面の
	// 全レイヤを描くことになり、無駄に重い。順番を入れ替えないこと。
	//
	// scale は縮尺の分母（1/100 なら 100.0）。**用紙と建物の大きさから呼び出し側が決める**
	// （core::planLayout / core::sectionLayout。M18）。0 以下なら縮尺には触らない
	// ——ビューポートの既定のままになる。かつてはここが「映すデザインレイヤの縮尺」を読んで
	// 当てていたが、デザインレイヤの縮尺は用紙に対する図の大きさとは関係が無く、図が用紙から
	// はみ出しても気付けなかった。
	ViewportFinish ConfigureViewport(MCObjectHandle viewport, MCObjectHandle sheetLayer,
									 const ViewportSetup& setup,
									 const core::ViewportCommand& command,
									 ViewportProjection projection, double scale);

	// シートレイヤの 4 辺の余白（用紙 mm。生の値を持つときは SDK が返したままの単位）。
	struct SheetMargins
	{
		double left = 0.0;
		double right = 0.0;
		double bottom = 0.0;
		double top = 0.0;
	};

	// シートレイヤの用紙まわりの実測値（長さはすべて用紙 mm）。
	//
	//   printable       … **印刷可能領域**（＝図を置いてよい矩形）。割り付けはこれを使う
	//   paper           … 用紙の外形の大きさ（ovLayerSheetPaperWidth/Height＝167/168）
	//   sheet           … シートレイヤの大きさ（VWLayerObj::GetSheetWidht＝165/166）
	//   margins         … 解釈後の 4 辺の余白（mm）。読めなければすべて 0
	//   rawMargins      … ISDK::GetPageMargins が返した**生の値**（単位不明のまま）
	//   marginsRead     … 余白を意味のある値として解釈できたか
	//   marginsInInches … その解釈が「インチ」だったか（false なら mm とみなした）
	struct SheetPaper
	{
		core::PaperArea printable;
		core::Vec2 paper;
		core::Vec2 sheet;
		SheetMargins margins;
		SheetMargins rawMargins;
		bool marginsRead = false;
		bool marginsInInches = false;
	};

	// シートレイヤの用紙と印刷可能領域を読む。用紙が読めなければ core::kDefaultPaperSize
	// （A3 横）で代用する（用紙が読めないだけで図を捨てない）。
	//
	// ★**用紙の大きさと印刷可能領域は別物**（M18）。かつては用紙の大きさだけを読み、
	// 余白は四辺 15mm と決め打ちしていたが、余白は用紙ではなく**印刷の設定**が決めるので、
	// 仮定した瞬間に実際とずれる（狭く見積もれば図が 1 段階小さくなり、広く見積もれば
	// 印刷で切れる）。SDK には両方がある——用紙は ovLayerSheetPaperWidth/Height（167/168）、
	// 余白は ISDK::GetPageMargins（4 辺）で、シートレイヤの大きさ（165/166＝
	// VWLayerObj::GetSheetWidht。**インチ**なので 25.4 倍して mm にする）はまた別値。
	// **GetPageMargins だけ単位がヘッダに書かれていない**ので、インチと mm のどちらとして
	// 読むかは「用紙 − 余白」がシートレイヤの大きさと一致するかで決め、決められなければ
	// 用紙に収まる方を採る（実装のコメント参照）。**実機では図面の単位で返った**
	// （mm の図面で 2.963 → 420 − 5.969 = 414 ＝ シートレイヤの幅。M18 のローカル確認）
	// が、単位が図面依存である以上インチの図面ではインチで返るはずなので、**「mm 固定」に
	// はしない**。**採った解釈と生の値は診断へ出す**（draw/Sheet）ので、別の環境でも
	// 実機で確かめられる。
	//
	// ★**用紙は原点を中心に置かれている前提**で矩形を組む。用紙が図面座標のどこに在るかを
	// 返す呼び出しは無い（ObjectVariables にも位置の変数は無い。ci-debug で確認）。
	// VWLayerObj::GetSheetOrigin() はあるが、それが用紙の中心を指すのか隅を指すのかは
	// ヘッダからは決まらないので**使わない**——意味の分からない値を使うより、規約を 1 つ
	// 決めて実機で確かめる方がよい（docs/DEV-NOTES.md M18「用紙の位置」）。
	SheetPaper SheetPaperArea(MCObjectHandle sheetLayer);

	// ビューポートの外形（用紙 mm）を測る。中心と大きさを書き戻し、測れれば true。
	//
	// 【なぜ測るのか】ビューポートの実寸は**描いてみるまで分からない**（映る図形の広がりで
	// 決まる）。したがって「どこに置くか」は生成・更新の後に測ってから決める（データタグを
	// 置いた後に測って直すのと同じ考え方。draw/Tag）。大きさは**見積もった縮尺で本当に
	// 収まったか**を確かめて診断へ残すのにも使う（core/Layout.h の PlanLayout::plan）。
	bool MeasureViewport(MCObjectHandle viewport, core::Vec2& center, core::Vec2& size);

	// 生成済みのビューポートの**縮尺だけ**を差し替えて描き直す。書けたら true。
	//
	// ConfigureViewport で一度仕上げた後に縮尺を変えたいときに使う——伏図は
	// **凡例の実測**（draw/Legend の measureLegendWidth）を待って初めて最終的な縮尺が
	// 決まるが、その凡例に何が並ぶかはビューポートに映るものが決めるので、先に仮の縮尺で
	// 図を作らざるを得ない（draw/Sheet の 2 巡）。更新を 1 回余分に走らせるので、
	// **縮尺が実際に変わったときだけ**呼ぶこと。
	bool ApplyViewportScale(MCObjectHandle viewport, double scale);

	// ビューポートを用紙の上で delta（用紙 mm）だけ動かす。注釈（データタグ）は
	// ビューポートと一緒に動く。
	//
	// ★**測ってから動かすまでの間にデータタグを置くこと**（順序を入れ替えてはならない）。
	// タグは「注釈へ置いた実位置を測って目標との差だけ動かす」作りで（draw/Tag の
	// MovePendingTags）、**その実測はビューポートが用紙のどこに在るかに影響される**。
	// 先にビューポートを動かしてからタグを置くと、動かした分だけタグが図からずれる
	// （M18 のローカル確認で実測。伏図・軸組図とも全タグが同じ向きへ外れていた）。
	void MoveViewportBy(MCObjectHandle viewport, const core::Vec2& delta);

	// 「命令インデックス → 描いたオブジェクトのハンドル」の対応表の**中身**。所有者
	// （draw/ObjectHandles.h の ObjectHandles）は SDK 非依存のヘッダに置いてあり、
	// SDK 型を持つこの定義だけがここに来る（そちらのヘッダ冒頭を参照）。
	struct ObjectHandleTable
	{
		std::map<std::size_t, MCObjectHandle> handles;
	};
} // namespace HomeskzIfcImport::draw
