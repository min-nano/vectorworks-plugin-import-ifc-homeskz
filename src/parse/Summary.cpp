//
//	parse/Summary.cpp
//
//	IFC 読み取りサマリの実装。Model の型別インデックス（parse/Step の byType）を数え、
//	結果を日本語テキストへ整形する。【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Summary.h"
#include "parse/Loader.h"

#include <array>
#include <cstddef>
#include <sstream>
#include <string>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// ホームズ君 IFC が使う主要エンティティ型の一覧（表示順）。key は byType へ渡す
		// 大文字の型名（parse/Step は型名を常に大文字で保持する）、displayType は
		// ダイアログに出すキャメルケース名、label はホームズ君での役割を表す日本語。
		// 対応: CLAUDE.md「移植の基本方針」が挙げる IfcGridAxis / IfcBeam / IfcColumn /
		// IfcFooting / IfcSlab / IfcBuildingStorey / IfcMechanicalFastener。
		struct TypeDef
		{
			const char* key;		 // byType のキー（大文字）
			const char* displayType; // 表示用 IFC 型名
			const char* label;		 // 日本語ラベル
		};

		constexpr std::array<TypeDef, 7> kTypes = {{
			{"IFCGRIDAXIS", "IfcGridAxis", "通り芯"},
			{"IFCBUILDINGSTOREY", "IfcBuildingStorey", "階（ストーリ）"},
			{"IFCBEAM", "IfcBeam", "横架材（梁・桁・土台）"},
			{"IFCCOLUMN", "IfcColumn", "柱・束"},
			{"IFCFOOTING", "IfcFooting", "基礎"},
			{"IFCSLAB", "IfcSlab", "スラブ・床版"},
			{"IFCMECHANICALFASTENER", "IfcMechanicalFastener", "金物"},
		}};
	} // namespace

	IfcSummary summarizeModel(const Model& model)
	{
		IfcSummary summary;
		summary.loaded = true;
		summary.entityCount = model.size();
		summary.counts.reserve(kTypes.size());
		for (const TypeDef& def : kTypes)
		{
			summary.counts.push_back(
				IfcTypeCount{def.displayType, def.label, model.byType(def.key).size()});
		}
		return summary;
	}

	IfcSummary summarizeIfc(const std::string& path)
	{
		bool ok = false;
		const Model model = loadIfc(path, &ok);
		if (!ok)
			return IfcSummary{}; // loaded=false・counts 空（読み込み失敗）
		return summarizeModel(model);
	}

	std::string formatSummary(const IfcSummary& summary)
	{
		if (!summary.loaded)
			return "IFC ファイルを読み込めませんでした。";

		std::ostringstream out;
		out << "IFC を読み込みました。検出した要素:\n";
		for (const IfcTypeCount& c : summary.counts)
			out << "\n  " << c.label << " (" << c.ifcType << "): " << c.count;
		out << "\n\nエンティティ総数: " << summary.entityCount;
		return out.str();
	}

	// ------------------------------------------------------------------------
	// インポート完了ダイアログの本文（M15「完了文言の集約」）
	// ------------------------------------------------------------------------

	namespace
	{
		// 完了ダイアログに並べる要素の表。**ここが唯一の一覧**で、要素を 1 つ足すときに
		// 触るのはこの表の 1 行だけ（以前は Extensions/ExtMenu.cpp の文字列連結と命令数の
		// 足し算の 2 か所を手で伸ばしていた。docs/DEV-NOTES.md M15「完了文言の集約」）。
		//
		// commands は「解析が出した命令の数」を Document から、placed は「描画が実際に
		// 描けた数」を DrawCounts から取り出す関数。キャプチャの無いラムダは関数ポインタへ
		// 変換できるので constexpr の表に置ける。並びは draw/ExecuteDocument のディスパッチ順
		// （ストーリ → 通り芯 → … → 伏図 → 軸組図）に揃えてあり、読む側が描かれた順にたどれる。
		//
		// placed をメンバポインタ（`std::size_t core::DrawCounts::*`）にしないのは、
		// `counts.*element.placed` という書き方を CodeQL が追えず、初期化済みのローカルを
		// 「未初期化かもしれない」と誤検出したため（cpp/uninitialized-local）。2 列とも
		// 同じ「取り出す関数」に揃えるほうが読みやすくもある。
		struct ElementDef
		{
			const char* label;								// 表示名（例: "横架材"）
			const char* unit;								// 助数詞（例: "本"）
			std::size_t (*commands)(const core::Document&); // 命令数
			std::size_t (*placed)(const core::DrawCounts&); // 描けた数
		};

		constexpr std::array<ElementDef, 17> kElements = {{
			{"ストーリ", "層", [](const core::Document& d) { return d.stories.size(); },
			 [](const core::DrawCounts& c) { return c.stories; }},
			{"通り芯", "本", [](const core::Document& d) { return d.grids.size(); },
			 [](const core::DrawCounts& c) { return c.grids; }},
			{"立上り", "本", [](const core::Document& d) { return d.walls.size(); },
			 [](const core::DrawCounts& c) { return c.walls; }},
			{"壁結合", "箇所", [](const core::Document& d) { return d.wallJoins.size(); },
			 [](const core::DrawCounts& c) { return c.wallJoins; }},
			{"底盤", "枚", [](const core::Document& d) { return d.slabs.size(); },
			 [](const core::DrawCounts& c) { return c.slabs; }},
			{"床", "枚", [](const core::Document& d) { return d.floors.size(); },
			 [](const core::DrawCounts& c) { return c.floors; }},
			{"横架材", "本", [](const core::Document& d) { return d.members.size(); },
			 [](const core::DrawCounts& c) { return c.members; }},
			{"柱", "本", [](const core::Document& d) { return d.columns.size(); },
			 [](const core::DrawCounts& c) { return c.columns; }},
			{"垂木", "本", [](const core::Document& d) { return d.rafters.size(); },
			 [](const core::DrawCounts& c) { return c.rafters; }},
			{"野地板", "枚", [](const core::Document& d) { return d.roofs.size(); },
			 [](const core::DrawCounts& c) { return c.roofs; }},
			{"アンカーボルト", "本", [](const core::Document& d) { return d.anchorBolts.size(); },
			 [](const core::DrawCounts& c) { return c.anchorBolts; }},
			{"床束", "本", [](const core::Document& d) { return d.floorPosts.size(); },
			 [](const core::DrawCounts& c) { return c.floorPosts; }},
			{"火打", "本", [](const core::Document& d) { return d.fireBraces.size(); },
			 [](const core::DrawCounts& c) { return c.fireBraces; }},
			{"仕口", "箇所", [](const core::Document& d) { return d.joints.size(); },
			 [](const core::DrawCounts& c) { return c.joints; }},
			{"柱記号", "個", [](const core::Document& d) { return d.columnMarks.size(); },
			 [](const core::DrawCounts& c) { return c.columnMarks; }},
			{"伏図", "枚", [](const core::Document& d) { return d.sheets.size(); },
			 [](const core::DrawCounts& c) { return c.sheets; }},
			{"軸組図", "枚", [](const core::Document& d) { return d.sections.size(); },
			 [](const core::DrawCounts& c) { return c.sections; }},
		}};
	} // namespace

	std::size_t documentCommandCount(const core::Document& document)
	{
		std::size_t total = 0;
		for (const ElementDef& element : kElements)
			total += element.commands(document);
		return total;
	}

	std::string formatImportResult(const core::Document& document, const core::DrawCounts& counts,
								   const std::string& logPath)
	{
		std::ostringstream out;
		if (!counts.valid)
		{
			// 検証に落ちたときは何も描いていない（draw/ExecuteDocument）。件数を並べても
			// すべて 0 になるだけなので、理由だけを返す。
			out << "命令セットの検証に通らなかったため、何も描きませんでした。";
		}
		else if (documentCommandCount(document) == 0)
		{
			// 解析は通ったが取り込める要素が 1 つも無かった（ホームズ君以外の IFC・
			// 空のファイル等）。要素名を並べて「何を探したか」を示す。
			out << "取り込める要素（ストーリ・通り芯・基礎・床・横架材・柱・屋根組・"
				   "シンボル・柱記号・伏図・軸組図）が見つかりませんでした。";
		}
		else
		{
			out << "以下を描きました。\n";
			for (const ElementDef& element : kElements)
			{
				const std::size_t commands = element.commands(document);
				if (commands == 0)
					continue; // 命令の無い要素は行ごと出さない（行が無い＝解析で 0 件）
				const std::size_t placed = element.placed(counts);
				out << "\n  " << element.label << ": ";
				if (placed != commands)
					out << placed << "/" << commands; // 描けなかったぶんが分かる形
				else
					out << placed;
				out << " " << element.unit;
			}
		}

		// 中止されたときは件数が命令数に届かないのが正常なので、そう明示する（「描けなかった」
		// と読み違えないように）。描けたところまでは図面に残っている。
		if (counts.cancelled)
			out << "\n\n（キャンセルされたため、途中で中断しました。）";
		// **取り消しがどこまで効くかを黙っていない。** 図形を 1 つでも描いたなら、ユーザーは
		// 「間違えたら取り消せばいい」と考えるのが自然なので、そのとおりに戻せるのか・
		// 一部しか戻らないのか・まったく戻らないのかを 1 行で伝える（判断の材料は描画側が
		// 置く。core::DrawCounts の undoArmed / undoPartial）。
		if (counts.valid && documentCommandCount(document) != 0)
		{
			if (!counts.undoArmed)
				out << "\n\n※ "
					   "この取り込みは「取り消し」では戻せません。取り込み前に戻したいときは、"
					   "保存せずに文書を閉じてください。";
			else if (counts.undoPartial)
				out << "\n\n※ "
					   "「取り消し」で戻せるのは、この取り込みが新しく作ったレイヤの分だけです。"
					   "取り込み前から在ったレイヤへ描いた分は残ります。";
			else
				out << "\n\n※ この取り込みは「取り消し」1 回で元に戻せます"
					   "（クラス・スタイル・ストーリの定義は残ります）。";
		}
		// **取り込み直後の伏図・軸組図は 1 回の「更新」が要る。** VW はデザインレイヤを
		// **高さの降順**（上にあるものが前面）で描くので、床仕上げ天端が構造天端より上にある
		// 以上、取り込み直後は床・野地板が柱・梁を覆う。こちらで並べた重ね順は図面には入って
		// いて、**ユーザーが 1 回更新すればそちらで描き直される**（ファイルを開き直しても同じ）。
		// 黙って誤った絵を見せないよう、図を 1 枚でも作ったなら必ず伝える
		// （経緯は docs/DEV-NOTES.md「レイヤ・ストーリ・重ね順」）。
		if (counts.sheets + counts.sections > 0)
			out << "\n\n※ 伏図・軸組図は、取り込み直後は床・野地板が柱・梁を覆って見えます。"
				   "ビューポートを 1 回「更新」すると、正しい重ね順で描き直されます"
				   "（ファイルを開き直しても直ります）。";
		// 描画側で起きた異常があれば足す（横架材の断面が入らない等。draw/Member 参照）。
		if (!counts.diagnostics.empty())
			out << "\n" << counts.diagnostics;
		// 診断ログが有効なら場所を案内する（有効なのは dev ビルドか HOMESKZ_IFC_TRACE 指定時
		// だけなので、ふだんの完了ダイアログには出ない）。
		if (!logPath.empty())
			out << "\n\n診断ログ: " << logPath;
		return out.str();
	}

	std::string formatImportError(const std::string& detail, const std::string& logPath)
	{
		std::ostringstream out;
		out << "インポート中に予期しないエラーが発生したため、途中で中断しました。\n"
			   "そこまでに描いたオブジェクトは図面に残っています"
			   "（要らなければ「取り消し」で戻せます）。";
		// 原因の手掛かりは**必ず出す**。ネイティブの異常は再現条件が分からなくなりがちで、
		// ここで捨てるとユーザーからは「黙って途中で止まった」としか見えない。
		out << "\n\n詳細: " << (detail.empty() ? std::string("原因不明") : detail);
		// 診断ログが有効なら場所を案内する。ログの**最終行**が「どのフェーズまで進んで
		// いたか」で、その直後が原因箇所になる（core/Trace.h）。
		if (!logPath.empty())
			out << "\n診断ログ: " << logPath;
		return out.str();
	}
} // namespace HomeskzIfcImport::parse
