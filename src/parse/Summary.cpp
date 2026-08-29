//
//	parse/Summary.cpp
//
//	IFC 読み取りサマリの実装。Model の型別インデックス（parse/Step の byType）を数え、
//	結果を日本語テキストへ整形する。【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Summary.h"
#include "parse/Loader.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
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
		// 要素の表。**ここが唯一の一覧**で、要素を 1 つ足すときに
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

	ImportOutcome importOutcome(const core::Document& document, const core::DrawCounts& counts)
	{
		ImportOutcome outcome;
		for (const ElementDef& element : kElements)
		{
			outcome.commands += element.commands(document);
			outcome.placed += element.placed(counts);
		}

		if (!counts.valid)
			outcome.status = ImportStatus::Invalid;
		else if (outcome.commands == 0)
			outcome.status = ImportStatus::Empty;
		else if (counts.cancelled)
			// **中止は Warning より優先する。** 中止すれば描き切れないのが当たり前で、
			// そこで「問題あり」と言われると原因を探しに行ってしまう。
			outcome.status = ImportStatus::Cancelled;
		else if (outcome.placed != outcome.commands || !counts.diagnostics.empty())
			// 描き切れなかった（＝命令はあるのに図面に出ていない）か、描画側が異常を
			// 持ち帰った（リソースが無い・PIO を作れない等）。どちらもログに理由がある。
			outcome.status = ImportStatus::Warning;
		else
			outcome.status = ImportStatus::Success;
		return outcome;
	}

	namespace
	{
		// 所要時間を人の言葉にする（"1 分 11 秒" / "12.3 秒" / "0.4 秒"）。ミリ秒まで
		// 出さないのは、ここで見たいのが「待たされたかどうか」の桁だけだから——
		// フェーズごとの内訳は診断ログの行頭にある経過ミリ秒が持つ。
		std::string formatDuration(double seconds)
		{
			std::ostringstream out;
			if (seconds >= 60.0)
			{
				const long long total = std::llround(seconds);
				out << (total / 60) << " 分 " << (total % 60) << " 秒";
			}
			else
			{
				out << std::fixed << std::setprecision(1) << seconds << " 秒";
			}
			return out.str();
		}

		// ファイルの大きさを人の言葉にする（0 は「分からない」なので空）。
		std::string formatBytes(unsigned long long bytes)
		{
			if (bytes == 0)
				return {};
			std::ostringstream out;
			out << std::fixed << std::setprecision(1);
			if (bytes >= 1024ULL * 1024ULL)
				out << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
			else if (bytes >= 1024ULL)
				out << (static_cast<double>(bytes) / 1024.0) << " KB";
			else
				return std::to_string(bytes) + " バイト";
			return out.str();
		}

		// 要素 1 つぶんの件数表記（"270 本" / 描き切れなければ "3/12 本"）。完了ダイアログの
		// 一覧を畳んだ後も、ログの内訳と総数はこの同じ書き方で読める。
		std::string formatCount(std::size_t placed, std::size_t commands, const char* unit)
		{
			std::string text;
			if (placed != commands)
				text = std::to_string(placed) + "/" + std::to_string(commands);
			else
				text = std::to_string(placed);
			text += " ";
			text += unit;
			return text;
		}

		// 「取り消し」がどこまで効くか（判断材料は描画側が置く。core::DrawCounts の
		// undoArmed / undoPartial）。図形を 1 つでも描いたときだけ意味がある。
		std::string undoLine(const core::DrawCounts& counts)
		{
			// **1 行に収める。** 但し書き（何が戻らないか）は README とログの内訳に譲る——
			// ダイアログで説明を始めると、肝心の「成功したか」が読まれなくなる。
			if (!counts.undoArmed)
				return "「取り消し」では戻せません（保存せずに文書を閉じてください）。";
			if (counts.undoPartial)
				return "「取り消し」で戻るのは、今回新しく作ったレイヤの分だけです。";
			return "「取り消し」1 回で元に戻せます。";
		}

		// 結末の 1 行目。**ここだけ読めば終わったかどうかが分かる**ようにする。
		std::string statusHeadline(ImportStatus status)
		{
			switch (status)
			{
			case ImportStatus::Success:
				return "取り込みが完了しました。";
			case ImportStatus::Warning:
				return "取り込みは終わりましたが、うまくいかなかったところがあります。";
			case ImportStatus::Cancelled:
				return "キャンセルされたため、途中で中断しました"
					   "（描いたところまでは図面に残っています）。";
			case ImportStatus::Invalid:
				return "命令セットの検証に通らなかったため、何も描きませんでした。";
			case ImportStatus::Empty:
				break;
			}
			return "取り込める要素が見つかりませんでした"
				   "（ホームズ君構造EX が書き出した IFC か確かめてください）。";
		}

		// ログの「結果:」に出す短い語。ダイアログの 1 行目と同じ判断から出す。
		const char* statusWord(ImportStatus status)
		{
			switch (status)
			{
			case ImportStatus::Success:
				return "成功";
			case ImportStatus::Warning:
				return "問題あり";
			case ImportStatus::Cancelled:
				return "中断（キャンセル）";
			case ImportStatus::Invalid:
				return "失敗（命令セットの検証に不合格）";
			case ImportStatus::Empty:
				break;
			}
			return "対象なし（取り込める要素が無い）";
		}

		// 改行区切りの説明を、ログの箇条書き（2 字下げ）へ組み替える。
		std::string indentLines(const std::string& text)
		{
			std::string out;
			std::istringstream in(text);
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty())
					continue;
				out += "  " + line + "\n";
			}
			return out;
		}
	} // namespace

	std::string formatImportResult(const core::Document& document, const core::DrawCounts& counts,
								   const ImportInfo& info)
	{
		const ImportOutcome outcome = importOutcome(document, counts);

		std::ostringstream out;
		out << statusHeadline(outcome.status);

		// 「どのファイルを取り込んだのか」は、図面を何度も取り込む使い方では毎回の関心事。
		if (!info.fileName.empty())
			out << "\n\nファイル: " << info.fileName;
		// 描いた総数は 1 行だけ。**要素ごとの内訳はログにある**（M19）。
		if (outcome.status != ImportStatus::Invalid && outcome.status != ImportStatus::Empty)
		{
			out << (info.fileName.empty() ? "\n\n" : "\n")
				<< "描いたもの: " << formatCount(outcome.placed, outcome.commands, "件");
			// 所要が分からない（0）なら出さない。
			if (info.seconds > 0.0)
				out << " / 所要 " << formatDuration(info.seconds);
		}

		// **その場で操作が要ることだけ**を書き足す（Summary.h「例外として残す 2 行」）。
		//
		// 取り込み直後の伏図・軸組図は 1 回の「更新」が要る。VW はデザインレイヤを
		// **高さの降順**（上にあるものが前面）で描くので、床仕上げ天端が構造天端より上にある
		// 以上、取り込み直後は床・野地板が柱・梁を覆う。こちらで並べた重ね順は図面には
		// 入っていて、ユーザーが 1 回更新すればそちらで描き直される（経緯は
		// docs/DEV-NOTES.md「レイヤ・ストーリ・重ね順」）。黙って誤った絵を見せない。
		if (counts.sheets + counts.sections > 0)
			out << "\n\n※ 伏図・軸組図はビューポートを 1 回「更新」してください。";
		// 図形を 1 つでも描いたなら、ユーザーは「間違えたら取り消せばいい」と考えるのが
		// 自然なので、そのとおりに戻せるのかを 1 行で伝える。
		if (outcome.status != ImportStatus::Invalid && outcome.status != ImportStatus::Empty)
			out << "\n※ " << undoLine(counts);

		// 思ったとおりに終わらなかったときだけ、どこを読めばよいかを指す（ログはこの
		// ダイアログの中で開ける）。**「取り込める要素が無い」も含める**——ホームズ君の
		// IFC かどうかを疑う場面で、何を探して何が無かったのかはログにしか無い。
		if (outcome.status != ImportStatus::Success && outcome.status != ImportStatus::Cancelled)
			out << "\n\nくわしい内訳と原因はログにあります（下の「ログを表示」）。";

		// ログの場所は**必ず出す**——不具合の報告でファイルごと添えたいときの唯一の手掛かりで、
		// 一時ディレクトリは macOS では `/var/folders/…/T/` のような当てられない場所にある。
		if (!info.logPath.empty())
			out << "\n\nログ: " << info.logPath;
		return out.str();
	}

	std::string formatImportError(const std::string& detail, const ImportInfo& info)
	{
		std::ostringstream out;
		out << "インポート中に予期しないエラーが発生したため、途中で中断しました。\n"
			   "そこまでに描いたオブジェクトは図面に残っています"
			   "（要らなければ「取り消し」で戻せます）。";
		if (!info.fileName.empty())
			out << "\n\nファイル: " << info.fileName;
		// 原因の手掛かりは**必ず出す**。ネイティブの異常は再現条件が分からなくなりがちで、
		// ここで捨てるとユーザーからは「黙って途中で止まった」としか見えない。
		out << (info.fileName.empty() ? "\n\n" : "\n")
			<< "詳細: " << (detail.empty() ? std::string("原因不明") : detail);
		// ログの**最終行**が「どのフェーズまで進んでいたか」で、その直後が原因箇所になる
		// （core/Trace.h）。
		out << "\n\nどこまで進んでいたかはログにあります（下の「ログを表示」）。";
		if (!info.logPath.empty())
			out << "\nログ: " << info.logPath;
		return out.str();
	}

	// ------------------------------------------------------------------------
	// 診断ログの本文（M19「短い完了・厚いログ」）
	// ------------------------------------------------------------------------

	std::string formatLogHeader(const BuildInfo& build, const std::string& ifcPath,
								unsigned long long bytes, const std::string& startedAt)
	{
		const auto orUnknown = [](const std::string& value)
		{ return value.empty() ? std::string("不明") : value; };

		std::ostringstream out;
		out << "=== ホームズ君 IFC インポート ===\n";
		// **1 行目に日時。** 報告を受け取る側は、まずユーザーの記憶（「昼ごろ試した」）と
		// ログを突き合わせる。
		out << "日時: " << orUnknown(startedAt) << "\n";
		// **どのリビジョンが動いているか。** dev ビルドは PR ごとに中身が違うので、
		// これが無いと「直したはずの不具合」の報告を古いビルドと取り違える。
		out << "ビルド: " << orUnknown(build.plugin);
		if (!build.channel.empty())
			out << "（" << build.channel << "）";
		out << " / commit " << orUnknown(build.commit) << " / branch " << orUnknown(build.branch)
			<< "\n";
		out << "実行環境: " << orUnknown(build.platform) << "\n";
		// **対象ファイルはフルパスで。** 同じ名前の IFC を版ごとに持っているのが普通なので、
		// ファイル名だけでは特定できない。
		out << "対象: " << orUnknown(ifcPath);
		const std::string size = formatBytes(bytes);
		if (!size.empty())
			out << "（" << size << "）";
		return out.str();
	}

	std::string formatLogResult(const core::Document& document, const core::DrawCounts& counts,
								double seconds)
	{
		const ImportOutcome outcome = importOutcome(document, counts);

		std::ostringstream out;
		out << "=== 結果 ===\n";
		out << "結果: " << statusWord(outcome.status) << "\n";
		if (seconds > 0.0)
			out << "所要: " << formatDuration(seconds) << "\n";
		out << "描いたもの: " << formatCount(outcome.placed, outcome.commands, "件") << "\n";

		// 要素ごとの内訳。**命令の無い要素は行ごと出さない**（無い物の「0 件」は読む側の
		// 邪魔になるだけで、行が無いこと自体が「解析で 0 件」を意味する）。検証に落ちた
		// ときは 1 つも描いていないので、全要素が "0/n" と並ぶだけになる——理由は
		// 「結果:」の行が言っているので、内訳ごと省く。
		if (outcome.commands != 0 && counts.valid)
		{
			out << "内訳:\n";
			for (const ElementDef& element : kElements)
			{
				const std::size_t commands = element.commands(document);
				if (commands == 0)
					continue;
				out << "  " << element.label << ": "
					<< formatCount(element.placed(counts), commands, element.unit) << "\n";
			}
		}

		// 描画側が持ち帰った異常（リソースが無い・PIO を作れない等）。
		if (!counts.diagnostics.empty())
			out << "注意:\n" << indentLines(counts.diagnostics);
		// 異常ではないが後から知りたい記録（用紙の割り付けの内訳など）。
		if (!counts.notes.empty())
			out << "記録:\n" << indentLines(counts.notes);

		if (counts.valid && outcome.commands != 0)
			out << "取り消し: " << undoLine(counts) << "\n";
		return out.str();
	}
} // namespace HomeskzIfcImport::parse
