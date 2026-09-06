//
//	core/Json.h
//
//	**最小の JSON。** フェーズ非依存（SDK も STEP も知らない）で、値の組み立て・書き出し・
//	読み取りだけを持つ。用途は 1 つ——**MCP ブリッジ**（core/Bridge.h）が Claude 側の
//	MCP サーバと交わす要求／応答の器である。
//
//	【なぜ自前か】この 1 用途のために外部ライブラリを足すと、SDK 非依存ライブラリの
//	依存が増え、無 SDK でどこでもビルドできるという CI の土台（CLAUDE.md「テスト方針」）に
//	外部の都合が入り込む。要るのは「素直な JSON を読み書きする」だけで、STEP リーダを
//	自前で持っているのと同じ理由でここも自前にする。
//
//	【決定性】オブジェクトのメンバは**挿入順のまま**保持して、その順で書き出す
//	（CLAUDE.md「決定性を守る」）。連想コンテナに入れて綴り順へ並べ替えると、同じ図面から
//	同じ出力が出ることは保たれるものの、こちらが意図した並び——素性 → 中身——が崩れて
//	読みにくくなる。引きは線形探索だが、扱う要素数は数十なので問題にならない。
//
//	【外から来たものを読む】要求は**プラグインの外**（別プロセス）が書いたテキストである。
//	壊れた入力・悪意ある入力で落ちないことが要件なので:
//	  * 入れ子の深さに上限を設ける（深い配列で再帰させてスタックを溢れさせない）。
//	  * 失敗は例外ではなく戻り値と error 文字列で返す（境界を越えさせない。CLAUDE.md
//	    「エラーハンドリング」）。
//
//	【数値】書き出しはロケールに依存させない（`std::ostringstream` へ `std::locale::classic`
//	を被せる）。小数点がカンマになる環境で壊れた JSON を吐くのを防ぐため。
//

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::core
{
	// JSON の値 1 つ。null / 真偽 / 数 / 文字列 / 配列 / オブジェクトのいずれか。
	class Json
	{
	public:
		enum class Kind
		{
			Null,
			Bool,
			Number,
			String,
			Array,
			Object,
		};

		Json() = default;

		// --- 作る ----------------------------------------------------------
		static Json null();
		static Json boolean(bool value);
		static Json number(double value);
		static Json integer(long long value);
		static Json string(std::string value);
		static Json array();
		static Json object();

		// --- 見る ----------------------------------------------------------
		Kind kind() const
		{
			return fKind;
		}
		bool isNull() const
		{
			return fKind == Kind::Null;
		}
		bool isArray() const
		{
			return fKind == Kind::Array;
		}
		bool isObject() const
		{
			return fKind == Kind::Object;
		}

		// 型が違えば既定値を返す（呼ぶ側に型検査を撒かないための約束）。
		bool asBool(bool fallback = false) const;
		double asNumber(double fallback = 0.0) const;
		std::string asString(const std::string& fallback = std::string()) const;

		// 配列の要素（配列でなければ空）。
		const std::vector<Json>& items() const
		{
			return fItems;
		}

		// オブジェクトのメンバ（挿入順）。
		const std::vector<std::pair<std::string, Json>>& members() const
		{
			return fMembers;
		}

		// オブジェクトのメンバ 1 つ。**無ければ null を返す**（呼ぶ側が has() を挟まずに
		// 既定値つきの as*() へ繋げられる）。
		const Json& at(const std::string& key) const;
		bool has(const std::string& key) const;

		// --- 足す ----------------------------------------------------------
		// 配列へ 1 つ（配列でなければ何もしない）。
		void push(Json value);
		// オブジェクトへ 1 つ。**同じ鍵が既にあれば置き換える**（並びは最初に入れた位置の
		// まま。後から書き換えても順が動かないほうが読みやすい）。
		void set(std::string key, Json value);

		// --- 書き出す・読み取る --------------------------------------------
		// 1 行の JSON テキスト（整形しない。読むのは機械だけ）。
		std::string dump() const;

		// 読み取り。成功したら true。失敗したら out は触らず error に理由が入る。
		static bool parse(const std::string& text, Json& out, std::string& error);

	private:
		Kind fKind = Kind::Null;
		bool fBool = false;
		double fNumber = 0.0;
		std::string fString;
		std::vector<Json> fItems;
		std::vector<std::pair<std::string, Json>> fMembers;
	};

	// JSON の文字列リテラル 1 つ分（引用符と傍線を含む）。ログや手組みの応答で使う。
	std::string jsonQuote(const std::string& text);
} // namespace HomeskzIfcImport::core
