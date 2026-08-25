//
//	parse/Step.cpp
//
//	最小 STEP リーダの実装。単一パスの再帰下降で DATA セクションのインスタンスを
//	読み、エンティティグラフ（型別・逆参照インデックス付き）を構築する。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//
//	対応する STEP 文法（ホームズ君サブセットに必要な範囲）:
//	  instance     := '#' int '=' ( simpleRecord | complexRecord ) ';'
//	  simpleRecord := typeName '(' argList ')'
//	  complexRecord:= '(' simpleRecord+ ')'              （複合エンティティ・稀）
//	  value        := ref | '$' | '*' | string | enum | list | typed | number
//	  ref          := '#' int
//	  string       := '\'' ( ... | '\'\'' ) '\''         （'' は ' のエスケープ）
//	                  ISO 10303-21 の拡張エスケープ（\X2\…\X0\ / \X\HH / \S\c / \P?\）は
//	                  デコードして UTF-8 にする（ホームズ君 IFC の日本語 Name は
//	                  \X2\5E8A\X0\ 形式＝UTF-16 で出力される。decodeStepString 参照）
//	  enum         := '.' ident '.'                      （.TRUE. / .T. など）
//	  list         := '(' ( value ( ',' value )* )? ')'
//	  typed        := typeName '(' value ')'             （IFCLABEL('x') など）
//	  number       := [+-]? digits ( '.' digits? ( ('e'|'E') [+-]? digits )? )?
//
//	寛容さ: 想定外の文字に出会ったら現在の文（次の ';'）まで読み飛ばして続行する。
//	数値変換の失敗は 0 として握りつぶす（CLAUDE.md「エラーハンドリング」）。
//

#include "parse/Step.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>

namespace HomeskzIfcImport::parse
{
	// ----------------------------------------------------------------------
	// - Value / Entity の小さなアクセサ
	// -----------------------------------------------------------------------

	double Value::asReal() const
	{
		if (type == ValueType::Real)
			return real;
		if (type == ValueType::Integer)
			return static_cast<double>(integer);
		return 0.0;
	}

	const Value& Entity::attribute(std::size_t index) const
	{
		// 範囲外は共有の Null 値を返す（呼び出し側の境界チェックを不要にする）。
		static const Value kNull;
		if (index >= attributes.size())
			return kNull;
		return attributes[index];
	}

	// ----------------------------------------------------------------------
	// - Model
	// -----------------------------------------------------------------------

	const Entity* Model::entity(int id) const
	{
		auto it = fEntities.find(id);
		return it == fEntities.end() ? nullptr : &it->second;
	}

	const Entity* Model::resolve(const Value& value) const
	{
		return value.isReference() ? entity(value.reference) : nullptr;
	}

	const std::vector<int>& Model::byType(const std::string& type) const
	{
		static const std::vector<int> kEmpty;
		auto it = fByType.find(type);
		return it == fByType.end() ? kEmpty : it->second;
	}

	const std::vector<int>& Model::referrers(int id) const
	{
		static const std::vector<int> kEmpty;
		auto it = fReferrers.find(id);
		return it == fReferrers.end() ? kEmpty : it->second;
	}

	namespace
	{
		// value（および入れ子）が参照する #id をすべて集める。逆参照インデックスの
		// 構築に使う。型付き値の引数・リスト要素も再帰的に辿る。
		void collectReferences(const Value& value, std::vector<int>& out)
		{
			if (value.isReference())
			{
				out.push_back(value.reference);
				return;
			}
			for (const Value& item : value.items)
				collectReferences(item, out);
		}
	} // namespace

	void Model::addEntity(Entity entity)
	{
		// 同一 #id の二重宣言は最初を採用（決定的）。挿入できなければ何もしない。
		int const id = entity.id;
		fEntities.emplace(id, std::move(entity));
	}

	void Model::buildIndices()
	{
		fByType.clear();
		fReferrers.clear();

		// fEntities は #id 昇順で反復されるので、各インデックス列も自然に昇順になり、
		// エンティティの出現順に依存しない決定的な結果になる。
		for (const auto& [id, entity] : fEntities)
		{
			fByType[entity.type].push_back(id);

			// 逆参照: この #id が参照する各 #id の被参照リストへ自分を足す。1 つの
			// エンティティが同じ #id を複数回参照しても被参照は 1 回だけにする。
			std::vector<int> refs;
			for (const Value& attr : entity.attributes)
				collectReferences(attr, refs);
			std::sort(refs.begin(), refs.end());
			refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
			for (int const target : refs)
				fReferrers[target].push_back(id);
		}
	}

	// ----------------------------------------------------------------------
	// - STEP パーサ本体
	// -----------------------------------------------------------------------

	namespace
	{
		// コードポイント 1 つを UTF-8 で末尾に足す（不正値は置換文字にせず捨てる）。
		void appendUtf8(std::string& out, unsigned int codePoint)
		{
			if (codePoint < 0x80U)
			{
				out.push_back(static_cast<char>(codePoint));
			}
			else if (codePoint < 0x800U)
			{
				out.push_back(static_cast<char>(0xC0U | (codePoint >> 6)));
				out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
			}
			else if (codePoint < 0x10000U)
			{
				out.push_back(static_cast<char>(0xE0U | (codePoint >> 12)));
				out.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
				out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
			}
			else if (codePoint <= 0x10FFFFU)
			{
				out.push_back(static_cast<char>(0xF0U | (codePoint >> 18)));
				out.push_back(static_cast<char>(0x80U | ((codePoint >> 12) & 0x3FU)));
				out.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
				out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
			}
		}

		// 16 進数字なら値を、そうでなければ -1 を返す。
		int hexValue(char c)
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			return -1;
		}

		// text[pos] から 4 桁の 16 進を読み、値を out に入れて true（読めなければ false）。
		bool readHex4(const std::string& text, std::size_t pos, unsigned int& out)
		{
			if (pos + 4 > text.size())
				return false;
			unsigned int value = 0;
			for (std::size_t i = 0; i < 4; ++i)
			{
				const int digit = hexValue(text[pos + i]);
				if (digit < 0)
					return false;
				value = (value << 4) | static_cast<unsigned int>(digit);
			}
			out = value;
			return true;
		}
	} // namespace

	std::string decodeStepString(const std::string& raw)
	{
		// ISO 10303-21 の拡張文字エスケープを UTF-8 へ直す。ホームズ君 IFC の日本語
		// （部材名・階名・断面名）は \X2\5E8A\X0\ 形式（UTF-16 コード単位の列）で出力
		// されるため、これをデコードしないと "床版" 等の名前判定が一切通らない。
		//   \X2\<hex…>\X0\ … UTF-16 コード単位の列（サロゲートペアも成立させる）
		//   \X\HH           … 1 バイト（ISO 8859-1 のコードポイント）
		//   \S\c            … c のコードポイント + 128（コードページ既定は Latin-1 とみなす）
		//   \P?\            … コードページ指示。意味を持たせず読み飛ばす
		// 上記以外のバックスラッシュはそのまま文字として残す（寛容さ: 壊れたエスケープで
		// 文字列全体を失わない）。
		if (raw.find('\\') == std::string::npos)
			return raw; // 一般的なケース（ASCII のみ）は走査せずそのまま返す

		std::string out;
		out.reserve(raw.size());
		std::size_t i = 0;
		while (i < raw.size())
		{
			if (raw[i] != '\\' || i + 2 >= raw.size())
			{
				out.push_back(raw[i]);
				++i;
				continue;
			}

			// \X2\…\X0\（UTF-16 列）
			if (raw.compare(i, 4, "\\X2\\") == 0)
			{
				std::size_t p = i + 4;
				unsigned int pending = 0; // 上位サロゲート（0 = 保留なし）
				while (true)
				{
					if (raw.compare(p, 4, "\\X0\\") == 0)
					{
						p += 4;
						break;
					}
					unsigned int unit = 0;
					if (!readHex4(raw, p, unit))
						break; // 壊れたエスケープ: そこまでを採ってこの区間を終える
					p += 4;
					if (unit >= 0xD800U && unit <= 0xDBFFU)
					{
						pending = unit;
						continue;
					}
					if (pending != 0 && unit >= 0xDC00U && unit <= 0xDFFFU)
					{
						const unsigned int cp =
							0x10000U + ((pending - 0xD800U) << 10) + (unit - 0xDC00U);
						appendUtf8(out, cp);
						pending = 0;
						continue;
					}
					pending = 0;
					appendUtf8(out, unit);
				}
				i = p;
				continue;
			}

			// \X\HH（1 バイト）
			if (raw.compare(i, 3, "\\X\\") == 0)
			{
				const int hi = (i + 4 < raw.size()) ? hexValue(raw[i + 3]) : -1;
				const int lo = (i + 4 < raw.size()) ? hexValue(raw[i + 4]) : -1;
				if (hi >= 0 && lo >= 0)
				{
					appendUtf8(out, static_cast<unsigned int>((hi << 4) | lo));
					i += 5;
					continue;
				}
			}

			// \S\c（コードポイント = c + 128）
			if (raw.compare(i, 3, "\\S\\") == 0 && i + 3 < raw.size())
			{
				appendUtf8(out, static_cast<unsigned char>(raw[i + 3]) + 128U);
				i += 4;
				continue;
			}

			// \P?\（コードページ指示）は読み飛ばす。
			if (raw[i + 1] == 'P' && raw.compare(i + 3, 1, "\\") == 0)
			{
				i += 4;
				continue;
			}

			out.push_back(raw[i]);
			++i;
		}
		return out;
	}

	namespace
	{
		// テキスト上を走る再帰下降パーサ。位置 fPos を進めながら値を読む。
		class Parser
		{
		public:
			explicit Parser(std::string_view text) : fText(text) {}

			// DATA セクションのインスタンスを順に読み、エンティティ列を返す。Model の
			// 構築（格納・インデックス化）は parseStep 側で行う（Parser は Model を知らない）。
			std::vector<Entity> parse()
			{
				std::vector<Entity> entities;
				skipTrivia();
				while (fPos < fText.size())
				{
					if (fText[fPos] == '#')
					{
						Entity entity;
						if (parseInstance(entity))
							entities.push_back(std::move(entity));
						else
							skipToStatementEnd();
					}
					else
					{
						// ヘッダ節・ISO 行など # 以外は 1 文単位で読み飛ばす。
						skipToStatementEnd();
					}
					skipTrivia();
				}
				return entities;
			}

		private:
			std::string_view fText;
			std::size_t fPos = 0;

			bool atEnd() const
			{
				return fPos >= fText.size();
			}
			char peek() const
			{
				return fText[fPos];
			}

			// 空白と /* ... */ コメントを読み飛ばす。
			void skipTrivia()
			{
				while (fPos < fText.size())
				{
					char const c = fText[fPos];
					if (std::isspace(static_cast<unsigned char>(c)))
					{
						++fPos;
					}
					else if (c == '/' && fPos + 1 < fText.size() && fText[fPos + 1] == '*')
					{
						fPos += 2;
						while (fPos + 1 < fText.size() &&
							   (fText[fPos] != '*' || fText[fPos + 1] != '/'))
							++fPos;
						fPos = std::min(fPos + 2, fText.size());
					}
					else
					{
						break;
					}
				}
			}

			// 現在の文の終端（トップレベルの ';'）まで読み飛ばす。文字列内の ';'
			// は無視する。エラー復帰と # 以外の文の読み捨てに使う。
			void skipToStatementEnd()
			{
				while (fPos < fText.size())
				{
					char const c = fText[fPos];
					if (c == '\'')
						skipString();
					else if (c == ';')
					{
						++fPos;
						return;
					}
					else
						++fPos;
				}
			}

			// 開き ' の位置から文字列を読み飛ばす（'' エスケープを考慮）。
			void skipString()
			{
				++fPos; // 開き '
				while (fPos < fText.size())
				{
					if (fText[fPos] == '\'')
					{
						if (fPos + 1 < fText.size() && fText[fPos + 1] == '\'')
							fPos += 2; // '' → 文字としての '
						else
						{
							++fPos; // 閉じ '
							return;
						}
					}
					else
						++fPos;
				}
			}

			// '#' int '=' record ';' を読む。成功したら entity を埋めて true。
			bool parseInstance(Entity& entity)
			{
				++fPos; // '#'
				int const id = readInt();
				if (id <= 0)
					return false;
				entity.id = id;

				skipTrivia();
				if (atEnd() || peek() != '=')
					return false;
				++fPos; // '='
				skipTrivia();
				if (atEnd())
					return false;

				if (peek() == '(')
				{
					// 複合エンティティ #id=(A(...)B(...));。型名は連結し、属性は
					// 各レコードの引数を連結する（本サブセットでは稀・簡易対応）。
					if (!parseComplexRecord(entity))
						return false;
				}
				else if (!parseSimpleRecord(entity))
				{
					return false;
				}

				skipTrivia();
				if (!atEnd() && peek() == ';')
					++fPos;
				return true;
			}

			// typeName '(' argList ')' を entity へ読む。
			bool parseSimpleRecord(Entity& entity)
			{
				std::string const name = readTypeName();
				if (name.empty())
					return false;
				entity.type = name;
				skipTrivia();
				if (atEnd() || peek() != '(')
					return false;
				return parseArgList(entity.attributes);
			}

			// '(' simpleRecord+ ')' を読む（複合エンティティ）。
			bool parseComplexRecord(Entity& entity)
			{
				++fPos; // 開き '('
				skipTrivia();
				while (!atEnd() && peek() != ')')
				{
					Entity part;
					if (!parseSimpleRecord(part))
						return false;
					if (entity.type.empty())
						entity.type = part.type;
					else
						entity.type += '.' + part.type;
					for (Value& v : part.attributes)
						entity.attributes.push_back(std::move(v));
					skipTrivia();
				}
				if (atEnd())
					return false;
				++fPos; // 閉じ ')'
				return true;
			}

			// '(' value ( ',' value )* ')' を out へ読む。開き '(' に位置している前提。
			bool parseArgList(std::vector<Value>& out)
			{
				++fPos; // '('
				skipTrivia();
				if (!atEnd() && peek() == ')')
				{
					++fPos;
					return true; // 空の引数リスト
				}
				while (!atEnd())
				{
					out.push_back(parseValue());
					skipTrivia();
					if (atEnd())
						return false;
					char const c = peek();
					if (c == ',')
					{
						++fPos;
						skipTrivia();
					}
					else if (c == ')')
					{
						++fPos;
						return true;
					}
					else
						return false; // 想定外 → 呼び出し側が文を読み飛ばす
				}
				return false;
			}

			// 1 つの値を読む。skipTrivia 済みを前提にしない（先頭で行う）。
			Value parseValue()
			{
				skipTrivia();
				Value value;
				// 防御的な EOF ガード。parseArgList は !atEnd のときだけ parseValue を
				// 呼ぶので、現行の呼び出し経路からは到達しない（カバレッジ上は未実行のまま
				// 残るが、将来 parseValue を別所から呼んだときの安全弁として残す）。
				if (atEnd())
					return value;

				char const c = peek();
				switch (c)
				{
				case '#':
					++fPos;
					value.type = ValueType::Reference;
					value.reference = readInt();
					return value;
				case '$':
					++fPos;
					value.type = ValueType::Null;
					return value;
				case '*':
					++fPos;
					value.type = ValueType::Derived;
					return value;
				case '\'':
					value.type = ValueType::String;
					value.text = readString();
					return value;
				case '(':
					value.type = ValueType::List;
					parseArgList(value.items);
					return value;
				case '.':
					value.type = ValueType::Enum;
					value.text = readEnum();
					return value;
				default:
					break;
				}

				if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
					return parseTypedOrKeyword();
				if (std::isdigit(static_cast<unsigned char>(c)) || c == '+' || c == '-')
					return parseNumber();

				// 解釈できない文字は 1 つ捨てて Null を返す（寛容）。
				++fPos;
				return value;
			}

			// typeName '(' value ')' の型付き値。'(' が続かなければ列挙記号として扱う。
			Value parseTypedOrKeyword()
			{
				Value value;
				std::string const name = readTypeName();
				skipTrivia();
				if (!atEnd() && peek() == '(')
				{
					value.type = ValueType::Typed;
					value.text = name;
					parseArgList(value.items);
					return value;
				}
				// 引数を伴わない裸のキーワード（想定外）。列挙記号として保持する。
				value.type = ValueType::Enum;
				value.text = name;
				return value;
			}

			// 整数か実数を読む。'.' か指数を含めば Real、さもなくば Integer。
			Value parseNumber()
			{
				std::size_t const begin = fPos;
				if (!atEnd() && (peek() == '+' || peek() == '-'))
					++fPos;
				bool isReal = false;
				while (!atEnd())
				{
					char const c = peek();
					if (std::isdigit(static_cast<unsigned char>(c)))
						++fPos;
					else if (c == '.')
					{
						isReal = true;
						++fPos;
					}
					else if (c == 'e' || c == 'E')
					{
						isReal = true;
						++fPos;
						if (!atEnd() && (peek() == '+' || peek() == '-'))
							++fPos;
					}
					else
						break;
				}
				std::string const token(fText.substr(begin, fPos - begin));

				Value value;
				try
				{
					if (isReal)
					{
						value.type = ValueType::Real;
						value.real = std::stod(token);
					}
					else
					{
						value.type = ValueType::Integer;
						value.integer = std::stoll(token);
					}
				}
				catch (const std::exception&)
				{
					// 変換失敗（空・桁溢れ等）は 0 として握りつぶす（寛容）。
					value.type = isReal ? ValueType::Real : ValueType::Integer;
				}
				return value;
			}

			// 連続する数字を整数として読む（#id・参照に使う）。失敗時は 0。
			int readInt()
			{
				std::size_t const begin = fPos;
				while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
					++fPos;
				if (fPos == begin)
					return 0;
				try
				{
					return std::stoi(std::string(fText.substr(begin, fPos - begin)));
				}
				catch (const std::exception&)
				{
					return 0;
				}
			}

			// 型名（英大文字・数字・下線）を読み、大文字化して返す。
			std::string readTypeName()
			{
				std::size_t const begin = fPos;
				while (!atEnd())
				{
					char const c = peek();
					if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
						++fPos;
					else
						break;
				}
				std::string name(fText.substr(begin, fPos - begin));
				for (char& ch : name)
					ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
				return name;
			}

			// 開き ' に位置している前提で文字列内容を読む（'' → '）。ISO 10303-21 の
			// 拡張エスケープは decodeStepString でまとめて UTF-8 へ直す。
			std::string readString()
			{
				std::string raw;
				++fPos; // 開き '
				while (!atEnd())
				{
					char const c = fText[fPos];
					if (c == '\'')
					{
						if (fPos + 1 < fText.size() && fText[fPos + 1] == '\'')
						{
							raw.push_back('\'');
							fPos += 2;
						}
						else
						{
							++fPos; // 閉じ '
							break;
						}
					}
					else
					{
						raw.push_back(c);
						++fPos;
					}
				}
				return decodeStepString(raw);
			}

			// 開き '.' に位置している前提で列挙記号（.TRUE. 等）の中身を読む。
			std::string readEnum()
			{
				++fPos; // 開き '.'
				std::size_t const begin = fPos;
				while (!atEnd() && peek() != '.')
					++fPos;
				std::string out(fText.substr(begin, fPos - begin));
				if (!atEnd())
					++fPos; // 閉じ '.'
				return out;
			}
		};
	} // namespace

	Model parseStep(const std::string& text)
	{
		Model model;
		Parser parser(text);
		for (Entity& entity : parser.parse())
			model.addEntity(std::move(entity));
		model.buildIndices();
		return model;
	}
} // namespace HomeskzIfcImport::parse
