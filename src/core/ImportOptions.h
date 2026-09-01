//
//	core/ImportOptions.h
//
//	取り込みの設定（インポート時の設定ダイアログで決める値）。いまのところ中身は
//	**置換するシンボルの対応**——「どの要素を図面のどのシンボルで置くか」だけで、
//	要素ごとの既定名（"アンカーボルト_M12" / "床束" / "鋼製火打" / "仕口" / 伏図記号）を
//	図面にある別のシンボルへ差し替えられるようにする（docs/DEV-NOTES.md M20）。
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
//	【役割の表はここ 1 つ】役割（SymbolRole）・画面に出す名前（label）・既定のシンボル名
//	（defaultSymbol）の対応は symbolRoles() ただ 1 つの表が持つ。シンボルを 1 つ増やすときに
//	触るのはその表の 1 行と、それを読む解析側の 1 行だけ（parse/Summary.cpp の kElements 表と
//	同じ考え方。CLAUDE.md「重複を作らない置き場所」）。
//
//	【SDK 非依存】core/ は VectorWorks SDK を include しない。
//

#pragma once

#include <array>
#include <cstddef>
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

	// 取り込み 1 回ぶんの設定。既定では役割の表の defaultSymbol がそのまま入るので、
	// **設定ダイアログを出さずに既定のまま使えば従来と同じ振る舞い**になる。
	struct ImportOptions
	{
		// 役割 → シンボル名。添字は SymbolRole の値（symbol() / setSymbol() を通すこと）。
		std::array<std::string, kSymbolRoleCount> symbols;

		ImportOptions();

		// 役割に対応するシンボル名。
		const std::string& symbol(SymbolRole role) const;

		// 役割のシンボル名を差し替える。**空文字は受け付けない**（空にすると
		// 「名前の無いシンボルを置け」という命令になり、描画側で必ず失敗する）——
		// 空を渡されたら既定名へ戻す。
		void setSymbol(SymbolRole role, const std::string& name);
	};
} // namespace HomeskzIfcImport::core
