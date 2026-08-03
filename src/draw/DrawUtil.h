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

#include "VWFC/VWObjects/VWParametricObj.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	// オブジェクトのクラスを名前で設定する（Python 版の vs.SetClass(handle, class) に対応）。
	// AddClass は既存なら索引を返し、無ければクラスを作る。クラス名が空なら何もしない
	// （無クラス＝既定クラスのまま）。
	void SetClassByName(MCObjectHandle object, const std::string& className);

	// 描画属性（線幅・色・パターン・矢印・透明度）をすべてクラス属性に従わせる。
	// SetObjectClass はクラスを割り当てるだけで各属性は by-instance の既定値のまま残るため、
	// 属性ごとに by-class を指定する（Python 版 _set_all_attributes_by_class と同じ意図。
	// ISDK の関数名は VS と異なる: PColors=ペン色 / FColors=面色 / PPat=線種 /
	// FPat=面パターン / Arrow=マーカー）。
	void SetAllAttributesByClass(MCObjectHandle object);

	// PIO のパラメータ名を解決する。universal 名で見つかればそれを使い、見つからなければ
	// ローカライズ名（OIP に出る日本語）で引き直す。**名前が 1 つ違うだけで setter は黙って
	// 無視される**（M6 の垂木で実証済み: 勾配・構造用途・ラベルが名前違いで効いていなかった）
	// ため、確実に見つかる方を選ぶ。どちらでも見つからなければ universal 名をそのまま返す。
	TXString ResolveParamName(const VWParametricObj& pio, const char* universalName,
							  const char* localizedName);

	// PIO に実数パラメータを書き、**読み戻して書けたか確かめる**。書けていれば true。
	// 角度・寸法のような数値パラメータでも、PIO の登録次第で実数ではなく文字列として
	// 保持されていることがあり、その場合 SetParamReal は黙って無視される。そこで実数で
	// 書けなかったときは文字列で入れ直す（M6 の垂木で「寸法を文字列で渡すと既定値のまま
	// だった」逆のケースが起きており、どちらに転んでも入るようにする）。
	bool SetParamRealChecked(VWParametricObj& pio, const TXString& param, double value,
							 double tolerance = 1e-6);

	// 構造材ツール（StructuralMember）へ渡す**断面プロファイルのグループ**を作る。
	// 矩形（[minX, minY]〜[maxX, maxY]）1 枚を閉じたポリゴンとしてグループへ入れて返す
	// （Python 版の BeginGroup / ClosePoly / Poly(…) / EndGroup に対応）。矩形の位置は
	// 呼び出し側の**断面基準点の規約**で決まる: 横架材は天端中央基準なので原点が上辺中央、
	// 柱は断面中心基準なので原点が中心（AxisAlign の設定と一致させる）。
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

	// 名前付きデザインレイヤを取得（無ければ作成）してアクティブにする。以後に生成する
	// オブジェクトはこのレイヤへ入る。取得・生成できなければ nil を返し、カレントレイヤも
	// 変えない。**通り芯の "共通" レイヤのように、その要素が自分で用意してよいレイヤ専用。**
	MCObjectHandle PrepareLayer(const std::string& layerName);

	// 既存の名前付きデザインレイヤをアクティブにする。**存在しなければ何もせず nil**
	// （レイヤを作らない）。ストーリ由来のレイヤ（"1-FL" / "n-垂木" / "n-野地板"）は
	// story 命令が作るので、無い＝そのストーリの生成がスキップされたということ。要素の
	// ために勝手にレイヤを作らない（Python 版 execute_floors / execute_rafters /
	// execute_roofs と同じ規約）。
	MCObjectHandle ActivateExistingLayer(const std::string& layerName);
} // namespace HomeskzIfcImport::draw
