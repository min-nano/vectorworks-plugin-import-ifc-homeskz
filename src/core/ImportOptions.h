//
//	core/ImportOptions.h
//
//	取り込みの設定（インポート時の設定ダイアログで決める値）。中身は 3 つ:
//	  * **置換するシンボルの対応**——「どの要素を図面のどのシンボルで置くか」。要素ごとの
//	    既定名（"アンカーボルト_M12" / "床束" / "鋼製火打" / "仕口" / 伏図記号）を図面に
//	    ある別のシンボルへ差し替えられるようにする（docs/DEV-NOTES.md M20）。
//	  * **図面枠（タイトルブロック）のスタイル**——シートレイヤ（伏図・軸組図）へ置く
//	    図面枠をどのスタイルで置くか。空なら置かない（docs/DEV-NOTES.md M28）。
//	  * **寸法規格**——伏図・軸組図へ自動で入れる寸法をどのスタイルで描くか。
//	    空なら寸法を入れない（docs/DEV-NOTES.md M31）。
//	  * **伏図のまとめ方**——横架材の高さごとに作る伏図のうち、どのレベルを前のレベルと
//	    同じ伏図にまとめるか。既定は「まとめない」＝高さごとに 1 枚（docs/DEV-NOTES.md
//	    「横架材の高さごとに伏図を作る」）。
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
//	触るのはその表の 1 行と、それを読む解析側の 1 行だけ（parse/Summary.cpp の kElements 表と
//	同じ考え方。CLAUDE.md「重複を作らない置き場所」）。
//
//	【SDK 非依存】core/ は VectorWorks SDK を include しない。
//

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <vector>
#include <string>

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
	};

	// 役割の数（＝設定ダイアログの行数）。enum の最後の値 + 1。
	inline constexpr std::size_t kSymbolRoleCount =
		static_cast<std::size_t>(SymbolRole::PlanMarkKoyazuka) + 1;

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
		bool canMerge = false; // 前のレベル（同じ階で 1 つ低い高さ）があるか
	};

	// 取り込み 1 回ぶんの設定。既定では役割の表の defaultSymbol がそのまま入り、どの役割も
	// 「取り込む」なので、**設定ダイアログを出さずに既定のまま使えば従来と同じ振る舞い**になる。
	//
	// 【役割ごとに「取り込むかどうか」を持つ理由】置きたいシンボルが図面に無いことは普通に
	// ある（テンプレートを当てていない図面・その要素を使わない案件）。そのとき**名前だけを
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
		// bugprone-exception-escape に掛かる（tidy-windows で実際に落ちた）。
		std::vector<PlanLevelKey> mergedPlanLevels;

		ImportOptions();

		// 役割に対応するシンボル名。**取り込まない役割の名前は意味を持たない**
		// （解析側は isEnabled() を先に見る）。
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
	};
} // namespace HomeskzIfcImport::core
