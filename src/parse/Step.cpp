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
	// -----------------------------------------------------------------------
	// Value / Entity の小さなアクセサ
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

	// -----------------------------------------------------------------------
	// Model
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

	// -----------------------------------------------------------------------
	// STEP パーサ本体
	// -----------------------------------------------------------------------

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

			// 開き ' に位置している前提で文字列内容を読む（'' → '）。
			std::string readString()
			{
				std::string out;
				++fPos; // 開き '
				while (!atEnd())
				{
					char const c = fText[fPos];
					if (c == '\'')
					{
						if (fPos + 1 < fText.size() && fText[fPos + 1] == '\'')
						{
							out.push_back('\'');
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
						out.push_back(c);
						++fPos;
					}
				}
				return out;
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
