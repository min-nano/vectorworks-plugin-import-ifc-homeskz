//
//	parse/Summary.h
//
//	IFC の読み取り結果サマリ。ホームズ君 IFC を読み込み、主要エンティティ型の件数を
//	数えて返す（M0 の「ローカル確認」用の診断: パースが動いている確証を件数で示す）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//	ここは Model（parse/Step）を読むだけの純ロジックなので、通常の C++ ツールチェインで
//	コンパイル・単体テストできる。ダイアログ表示（SDK 依存）はメニューコマンド側
//	（Extensions/ExtMenu）が担い、この文字列整形（formatSummary）までを無 SDK で
//	テストできるようにしておく（CLAUDE.md「テスト方針」: SDK から切り離せる部分は
//	無 SDK 側へ寄せる）。
//
//	命令セット（core::Document）とは別物であることに注意。Document は各要素の描画命令を
//	持つが、M0 時点ではまだ空。本サマリは Document を経由せず、生の Model から
//	「どの IFC 型が何件あるか」を直接数える診断であり、M1 以降で要素解析が育っても
//	独立した確認手段として残せる。
//

#pragma once

#include "parse/Step.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// 1 つの IFC 型の検出件数。ifcType は表示用のキャメルケース名（例: "IfcGridAxis"）、
	// label はホームズ君での役割を表す日本語ラベル（例: "通り芯"）。
	struct IfcTypeCount
	{
		std::string ifcType; // 表示用 IFC 型名（例: "IfcGridAxis"）
		std::string label;	 // 日本語ラベル（例: "通り芯"）
		std::size_t count = 0;
	};

	// IFC 読み取りサマリ。loaded はファイルを読み込めたか、entityCount は総エンティティ
	// 数、counts はホームズ君が使う主要型ごとの件数（固定順・欠けている型も count=0 で
	// 含める。件数表示が入力によってブレず、テストしやすいようにするため）。
	struct IfcSummary
	{
		bool loaded = false;
		std::size_t entityCount = 0;
		std::vector<IfcTypeCount> counts;
	};

	// 既に読み込んだ Model の主要型を数える。ホームズ君 IFC の骨格をなす型
	// （通り芯・階・横架材・柱・基礎・スラブ・金物）を固定順で数える（CLAUDE.md
	// 「移植の基本方針」で挙げる IfcGridAxis / IfcBuildingStorey / IfcBeam / IfcColumn /
	// IfcFooting / IfcSlab / IfcMechanicalFastener に対応）。loaded は true。
	IfcSummary summarizeModel(const Model& model);

	// ファイルを読み込んでサマリを返す。読み込みに失敗したら loaded=false・counts 空で
	// 返す（フェーズ境界は値で返し、例外をフェーズ外へ漏らさない）。
	IfcSummary summarizeIfc(const std::string& path);

	// サマリを人が読める複数行テキストへ整形する（メニューコマンドのダイアログ本文）。
	// 読み込み失敗時は失敗メッセージを返す。SDK 非依存なので単体テストで文言を検証できる。
	std::string formatSummary(const IfcSummary& summary);
} // namespace HomeskzIfcImport::parse
