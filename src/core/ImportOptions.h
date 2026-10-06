//
//	core/ImportOptions.h
//
//	取り込みの設定（インポート時の設定ダイアログで決める値）。中身は 6 つ:
//	  * **置換するシンボルの対応**——「どの要素を図面のどのシンボルで置くか」。要素ごとの
//	    既定名（"アンカーボルト_M12" / "床束" / "鋼製火打" / "仕口" / 伏図記号 / "継手"）を
//	    図面にある別のシンボルへ差し替えられるようにする（docs/DEV-NOTES.md M20）。
//	  * **図面枠（タイトルブロック）のスタイル**——シートレイヤ（伏図・軸組図）へ置く
//	    図面枠をどのスタイルで置くか。空なら置かない（docs/DEV-NOTES.md M28）。
//	  * **寸法規格**——伏図・軸組図へ自動で入れる寸法をどのスタイルで描画するか。
//	    空なら寸法を入れない（docs/DEV-NOTES.md M31）。
//	  * **伏図のまとめ方**——横架材の高さごとに作る伏図のうち、どのレベルを前のレベルと
//	    同じ伏図にまとめるか。既定は「まとめない」＝高さごとに 1 枚（docs/DEV-NOTES.md
//	    「横架材の高さごとに伏図を作る」）。
//	  * **軸組図から除外する通り**——解析が軸組図にする通りのうち、描画しないもの
//	    （docs/DEV-NOTES.md M34）。空なら従来どおり全部描画する。
//	  * **垂木の断面寸法**——IFC に垂木の寸法が無いので決め打ちしていた 45×45 を、取り込み
//	    ごとに一律で指定できるようにしたもの（docs/DEV-NOTES.md「垂木の断面を一律に指定する」）。
//	    既定は 45×45 で従来どおり。
//
//	【なぜ core/ に置くか】設定は**両フェーズにまたがる**唯一の入力である:
//	  * 決めるのは描画側（draw/SettingsDialog）——図面にどんなシンボルがあるかは
//	    VectorWorks にしか訊けない。
//	  * 使うのは解析側（parse/*）——命令セット（core::SymbolCommand::symbol）へ名前を
//	    書き込むのは解析だから。
//	つまり Document と同じく「フェーズ間で運ぶ値」であり、SDK も STEP も知らない場所＝
//	core/ が置き場所になる（CLAUDE.md「依存の向きは厳守する」）。**プレーンな構造体**で
//	表すのも Document と同じ方針。
//
//	【シンボル以外の設定もここへ入る】M28 で**図面枠（タイトルブロック）のスタイル**が
//	加わった。シンボルの役割と同じく「決めるのは描画側・使うのは解析側」なので置き場所は
//	同じだが、**役割の表には入らない**——置くのはシンボルではなくプラグインオブジェクトの
//	スタイルで、選択肢の集め方（図面のシンボル定義のうち subType 552 のもの）も、既定値の
//	有無も違う（下記 titleBlock）。
//
//	【役割の表はここ 1 つ】役割（SymbolRole）・画面に出す名前（label）・既定のシンボル名
//	（defaultSymbol）の対応は symbolRoles() ただ 1 つの表が持つ。シンボルを 1 つ増やすときに
//	変更するのはその表の 1 行と、それを読む解析側の 1 行だけ（parse/Summary.cpp の kElements 表と
//	同じ考え方。CLAUDE.md「重複を作らない置き場所」）。
//
//	【SDK 非依存】core/ は VectorWorks SDK を include しない。
//

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 置換するシンボルの「役割」。**要素そのものではなく、シンボルを 1 つ選ぶ単位**である
	// ことに注意——アンカーボルトは座金の有無で 2 つ、柱伏図記号は柱と小屋束で 2 つある。
	enum class SymbolRole : std::size_t
	{
		AnchorBoltM12 = 0, // アンカーボルト（座金付き。型名が "座金なし" でないもの）
		AnchorBoltM16,	  // アンカーボルト（座金なし）
		FloorPost,		  // 床束
		FireBrace,		  // 火打
		Joint,			  // 仕口
		PlanMarkColumn,	  // 伏図記号（柱＝管柱・通し柱）
		PlanMarkKoyazuka, // 伏図記号（小屋束）
		Splice,			  // 継手（M33）
	};

	// 【新しい役割は末尾へ追加する】値は往復の記憶（core/FeedbackSession の "role.<n>.…"）に
	// **番号で**書かれる。途中へ挟むと後ろの役割の番号がずれ、進行中の往復が別の役割の
	// シンボル名を読んでしまう（継手＝M33 を仕口の隣ではなく末尾に置いたのはこのため）。
	//
	// 役割の数（＝設定ダイアログの行数）。enum の最後の値 + 1。
	inline constexpr std::size_t kSymbolRoleCount =
		static_cast<std::size_t>(SymbolRole::Splice) + 1;

	// 役割 1 つの素性。label は設定ダイアログに出す行の名前で、defaultSymbol は
	// 何も選ばなかったときに使うシンボル名（＝この設定を入れる前の固定値）。
	struct SymbolRoleInfo
	{
		SymbolRole role;
		const char* label;
		const char* defaultSymbol;
	};

	// 役割の表（**唯一の定義**）。並びは設定ダイアログに出る順で、enum の値順と一致する。
	const std::array<SymbolRoleInfo, kSymbolRoleCount>& symbolRoles();

	// 役割の既定のシンボル名。
	const char* defaultSymbolName(SymbolRole role);

	// 役割の画面表示名。
	const char* symbolRoleLabel(SymbolRole role);

	// 伏図レベル（横架材の高さ）1 つを指す鍵。story は FL 階の 0 起点の番号（Elevation 昇順。
	// parse/Story の collectStories の並び）、height はその高さの横架材の天端の GL からの
	// 高さ（mm に丸めた整数）。**同じ IFC なら何度読んでも同じ鍵になる**ので、設定ダイアログ
	// （取り込みの前に IFC を 1 度読んで一覧を出す）と解析（もう 1 度読む）の間で運べる。
	struct PlanLevelKey
	{
		int story = 0;
		long long height = 0;

		auto operator<=>(const PlanLevelKey&) const = default;
	};

	// 設定ダイアログに出す伏図レベルの候補 1 つ（まとめる前の横架材の高さ 1 つ）。解析側が
	// IFC から集め（parse/BuildDocument の scanPlanLevelChoices）、描画側のダイアログが
	// 並べる——Document と同じ「フェーズ間で運ぶ値」なので core に置く。
	struct PlanLevelChoice
	{
		PlanLevelKey key; // そのレベルの鍵（まとめる設定の鍵。key.height が高さ）
		std::string planTitle; // その階の伏図の名前（"2階床伏図" / "2階小屋伏図"）
		std::string heightText; // 高さの表記（"FL-872" / "軒高-832"。core::planLevelHeightText）
		bool canMerge = false; // 前のレベル（同じ階で 1 つ低い高さ）があるか
	};

	// 垂木の既定断面（mm。幅×せい）。IFC に垂木の寸法情報が無いため決め打ちしていた値で、
	// 設定ダイアログで一律に差し替えられる（ImportOptions::rafterWidth / rafterHeight）。
	// **設定を触らなければこの値**なので、既定の取り込み結果は従来と変わらない。
	inline constexpr double kDefaultRafterWidth = 45.0;
	inline constexpr double kDefaultRafterHeight = 45.0;

	// 受け付ける垂木寸法の上限（mm）。これを超える値・0 以下・非有限は打ち間違いとみなして
	// 受け付けない（1 桁多い「450」は通るが、「4500」のような桁違いで屋根を埋め尽くさない
	// ための歯止め）。垂木として意味のある寸法はこれより十分小さい。
	inline constexpr double kMaxRafterSize = 1000.0;

	// 垂木の寸法（mm）として受け付けるか（0 < mm <= kMaxRafterSize、有限）。
	bool isValidRafterSize(double mm);

	// 垂木の寸法の文字列（設定ダイアログの入力欄・往復の記憶）を mm の数へ読む。
	// 受け付けるのは「数字列（小数点 1 つまで）」だけで、前後の空白と全角の数字・小数点
	// （日本語入力のまま打たれたもの）は読み替える。単位・符号・指数表記は受け付けない。
	// 読めない・isValidRafterSize を満たさないなら std::nullopt。
	//
	// 【なぜ strtod / from_chars を使わないか】strtod は小数点がロケールに従うので、
	// VectorWorks の中（利用者の環境のロケール）とテストとで読み方が変わりうる。浮動小数の
	// from_chars は mac の標準ライブラリが対応していない版がある。受け付ける形がごく狭いので
	// 自前で読む。
	std::optional<double> parseRafterSize(const std::string& text);

	// 垂木の寸法（mm）を表示用の文字列にする。小数点以下 1 桁までに丸め、末尾の 0 と
	// 小数点は取り除く（45 → "45"、45.5 → "45.5"）。parseRafterSize で読み戻せる形。
	std::string formatRafterSize(double mm);

	// 取り込み 1 回ぶんの設定。既定では役割の表の defaultSymbol がそのまま入り、どの役割も
	// 「取り込む」なので、**設定ダイアログを出さずに既定のまま使えば従来と同じ振る舞い**になる。
	//
	// 【役割ごとに「取り込むかどうか」を持つ理由】置きたいシンボルが図面に無いことは普通に
	// ある（テンプレートを適用していない図面・その要素を使わない案件）。そのとき**名前だけを
	// 持たせて「あるつもり」で置きに行っても必ず失敗する**ので、はじめから「この役割は
	// 取り込まない」と言えるようにする。解析側はその役割の命令を 1 つも作らない——描画側で
	// 失敗させて診断に出すのではなく、**そもそも指示を出さない**（docs/DEV-NOTES.md
	// 「取り込み設定の決め事（M20）」）。設定ダイアログの候補も、これで**図面に実在する
	// シンボルだけ**にできる。
	struct ImportOptions
	{
		// 役割 → シンボル名。添字は SymbolRole の値（symbol() / setSymbol() を通すこと）。
		std::array<std::string, kSymbolRoleCount> symbols;

		// 役割 → 取り込むか。既定はすべて true（従来どおり全要素を置く）。
		std::array<bool, kSymbolRoleCount> enabled{};

		// M28 図面枠（タイトルブロック）のスタイル名。**空＝図面枠を置かない**。
		//
		// 【なぜ「取り込むか」の真偽を別に持たないか】シンボルの役割は**既定名を持つ**
		// （ホームズ君のテンプレートが供給する固定の名前）ので、「取り込まないが名前は
		// 残る」という状態に意味がある。図面枠のスタイルは**利用者の図面ごとに名前が違い、
		// 既定と呼べる名前が無い**——「置く」なら必ず名前が要り、「置かない」なら名前は
		// 一切意味を持たない。したがって空文字ただ 1 つで表すほうが、真偽と名前の食い違い
		// （「置くのに名前が空」）を型の上で作らずに済む。
		//
		// 【既定が「置かない」である理由】この設定を入れる前は図面枠を置いていなかったので、
		// **設定ダイアログを出さずに既定のまま使えば従来と同じ振る舞い**になる（役割の表の
		// 既定名がそうであるのと同じ考え方）。
		std::string titleBlock;

		// M31 寸法規格の名前。**空＝寸法（とレベル記号）を入れない**。空ただ 1 つで
		// 「入れない」を表す理由も、既定が「入れない」である理由も図面枠（titleBlock）と
		// 同じ——寸法の見え方は利用者の図面ごとに違い、既定と呼べる名前が無い。設定
		// ダイアログを出さずに既定のまま使えば従来と同じ（寸法の無い）図になる。
		std::string dimension;

		// 伏図のまとめ方: **前のレベル（同じ階で 1 つ低い高さ）と同じ伏図にまとめる**
		// レベルの鍵。既定は空＝高さごとに 1 枚ずつ伏図を作る。まとめるかどうかは設計者が
		// 決めること（ご要望）なので、解析側は高さが 1mm でも違えば別のレベルとし、
		// ここに挙がったものだけを寄せる（parse/PlanLevel）。階をまたいではまとめない
		// ——レイヤは階に属するので、別の階の横架材を 1 つのレイヤへは置けない。
		//
		// **昇順・重複なしの vector で持つ**（setMergeWithPrevious が保つ）。std::set にすると
		// MSVC ではムーブ構築が例外を投げうる（番兵ノードを確保する）ので、この構造体と
		// それを持つ構造体（core::FeedbackSession ほか）の暗黙のムーブが clang-tidy の
		// bugprone-exception-escape に掛かる（tidy-windows で実際に失敗した）。
		std::vector<PlanLevelKey> mergedPlanLevels;

		// M34 軸組図から**除外する**通りの図番（core::SectionCommand の viewport.drawingNumber。
		// "X1" / "い" / 方向をまたいで重なったときの "1(2)" …）。**空＝全部描画する**。
		//
		// 【なぜ「描画する通り」ではなく「除外する通り」を持つか】候補（どの通りを軸組図に
		// するか）は IFC を解析して初めて決まる。「描画する」側を持つと、既定（何も選んで
		// いない）が「1 枚も描画しない」になり、設定ダイアログを出さずに取り込む経路（実機
		// テストの周・設定を出せなかったとき）で軸組図が消える。除外する側を持てば**既定の
		// 空が従来と同じ振る舞い**になる（役割の表の既定名・図面枠の空と同じ考え方）。
		//
		// 【なぜ図番で指すか】図番は解析が通りごとに一意に付ける（parse/Section.h の
		// uniqueSectionNumbers）ので、同じ IFC を解析し直せば同じ通りに同じ図番が付く
		// ——選ぶための解析と取り込むための解析とで、同じ通りを指せる。
		std::vector<std::string> skippedSections;

		// 垂木の断面（mm）。幅＝軒方向の寸法、せい＝屋根面に直交する寸法。**全垂木に一律**
		// （ご要望。屋根面ごと・階ごとには分けない）。既定は従来の決め打ち 45×45。
		// setRafterSize を通すこと（受け付けない値を入れない）。
		double rafterWidth = kDefaultRafterWidth;
		double rafterHeight = kDefaultRafterHeight;

		ImportOptions();

		// 役割に対応するシンボル名。**取り込まない役割の名前は意味を持たない**
		// （解析側は isEnabled() を先に参照する）。
		const std::string& symbol(SymbolRole role) const;

		// その役割を取り込むか。
		bool isEnabled(SymbolRole role) const;

		// 役割のシンボル名を差し替える。**空文字は受け付けない**（空にすると
		// 「名前の無いシンボルを置け」という命令になり、描画側で必ず失敗する）——
		// 空を渡されたら既定名へ戻す。取り込むかどうかは変えない。
		void setSymbol(SymbolRole role, const std::string& name);

		// その役割を取り込むかを決める。
		void setEnabled(SymbolRole role, bool enabled);

		// M28 図面枠のスタイル名（空なら置かない）。
		const std::string& titleBlockStyle() const;

		// 図面枠を置くか（＝スタイル名が空でないか）。
		bool hasTitleBlock() const;

		// 図面枠のスタイル名を決める。**空文字はそのまま受け付ける**——シンボルの
		// setSymbol と違って既定名が無く、空は「置かない」という意味を持つ（上記）。
		void setTitleBlockStyle(const std::string& name);

		// M31 寸法規格の名前（空なら寸法を入れない）。
		const std::string& dimensionStandard() const;

		// 寸法を入れるか（＝スタイル名が空でないか）。
		bool hasDimensions() const;

		// 寸法規格の名前を決める。空は「入れない」としてそのまま受け付ける（図面枠と同じ）。
		void setDimensionStandard(const std::string& name);

		// そのレベルを前のレベルと同じ伏図にまとめるか。
		bool mergesWithPrevious(const PlanLevelKey& key) const;

		// そのレベルを前のレベルと同じ伏図にまとめるかを決める。
		void setMergeWithPrevious(const PlanLevelKey& key, bool merge);

		// M34 その図番の通りを軸組図から除外するか。
		bool isSectionSkipped(const std::string& drawingNumber) const;

		// M34 除外する通りの図番を差し替える（重複・空文字は取り除き、名前順に並べ直す
		// ——ログに出す並びを入力順に依らせないため。CLAUDE.md「決定性を守る」）。
		void setSkippedSections(const std::vector<std::string>& drawingNumbers);

		// 垂木の断面を決める。**受け付けない値（isValidRafterSize を満たさない）はその寸法
		// だけ既定へ戻す**——シンボル名の空文字を既定名へ戻すのと同じ考え方で、描画できない
		// 寸法の命令を作らない。
		void setRafterSize(double width, double height);
	};
	// -----------------------------------------------------------------------
	// **図面にあるものから組む既定の設定**（実機テストの自動の 1 周目。M40）。
	//
	// 設定ダイアログを「まだ一度も決めていない」状態で開いたときの初期値と同じものを、
	// ダイアログを出さずに組む——Claude が MCP の `vw_run_test` から 1 周目を起こすとき、
	// 誰も操作していない Vectorworks にダイアログを出せないため（draw/Feedback.h）。
	//   * シンボルの役割 … 既定名（symbolRoles）のシンボルが図面に**ある役割だけ**取り込む
	//     （ダイアログの「いまの対応先が図面に無い行はチェックを外して開く」と同じ）。
	//   * 図面枠 … 図面にスタイルがあれば一覧の最初のもの（無ければ置かない）。
	//   * 寸法規格 … defaultDimensionStandardIndex が選ぶもの（無ければ入れない）。
	//   * 伏図のまとめ方・軸組図から除外する通り・垂木の断面 … ImportOptions の既定のまま
	//     （まとめない・全部描画する・45×45。どれもダイアログの初期値と同じ）。
	// 引数はどれも図面から集めた名前の一覧（draw/SettingsDialog が SDK で集める）。
	ImportOptions presetImportOptions(const std::vector<std::string>& symbolNames,
									  const std::vector<std::string>& titleBlockStyles,
									  const std::vector<std::string>& dimensionStandards);

	// 寸法規格の候補 names から、まだ決めていないときに選ぶものの添字。「JIS」があれば
	// それ（日本の構造図の既定として自然で、一覧の最初は JIS とは限らない）、無ければ 0。
	// **設定ダイアログの初期値と presetImportOptions が同じものを選ぶための唯一の置き場**。
	// names が空でも 0 を返す（呼び出し側が範囲を確かめる）。
	std::size_t defaultDimensionStandardIndex(const std::vector<std::string>& names);
} // namespace HomeskzIfcImport::core
