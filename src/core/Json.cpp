//
//	core/Json.cpp
//
//	最小 JSON の実装（意図と制約は core/Json.h）。書き出しは再帰、読み取りは深さ制限つきの
//	再帰下降。**外から来たテキストを読む**ので、壊れた入力で落ちないことを最優先にする。
//

#include "core/Json.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 入れ子の上限。**外から来たテキストで再帰を深くさせない**ための歯止め
		// （[[[[[… と書いただけでスタックを溢れさせられるのを防ぐ）。ブリッジが運ぶのは
		// 素直な 2〜3 段の構造なので、64 段あれば実用上は当たらない。
		constexpr int kMaxDepth = 64;

		// 空のときに返す不変の値（at() が「無ければ null」を返すための実体）。
		const Json& NullValue()
		{
			static const Json value;
			return value;
		}

		// --- 書き出し ------------------------------------------------------

		void AppendEscaped(std::string& out, const std::string& text)
		{
			out += '"';
			for (const char raw : text)
			{
				const auto ch = static_cast<unsigned char>(raw);
				switch (ch)
				{
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				case '\b':
					out += "\\b";
					break;
				case '\f':
					out += "\\f";
					break;
				default:
					if (ch < 0x20)
					{
						// 制御文字は \u00XX。ここだけ 16 進で書く。
						static const char* const kHex = "0123456789abcdef";
						out += "\\u00";
						out += kHex[(ch >> 4) & 0x0F];
						out += kHex[ch & 0x0F];
					}
					else
					{
						// **UTF-8 はそのまま通す。** 日本語のレイヤ名・クラス名を \u へ
						// 展開する理由が無い（JSON として正しく、読むときも楽）。
						out += raw;
					}
					break;
				}
			}
			out += '"';
		}

		void AppendNumber(std::string& out, double value)
		{
			// 非有限は JSON で表せない。**null に落とす**（例外にすると 1 つの異常値で
			// 応答全体が消える）。
			if (!std::isfinite(value))
			{
				out += "null";
				return;
			}
			// 整数で表せるものは整数として書く（件数・種別番号が 3.000000 と出ると読みにくい）。
			if (value == std::floor(value) && std::abs(value) < 1e15)
			{
				out += std::to_string(static_cast<long long>(value));
				return;
			}
			// **ロケールを固定する**（小数点がカンマになる環境で壊れた JSON を吐かない）。
			std::ostringstream oss;
			oss.imbue(std::locale::classic());
			oss.precision(std::numeric_limits<double>::max_digits10);
			oss << value;
			out += oss.str();
		}

		void AppendValue(std::string& out, const Json& value)
		{
			switch (value.kind())
			{
			case Json::Kind::Null:
				out += "null";
				return;
			case Json::Kind::Bool:
				out += value.asBool() ? "true" : "false";
				return;
			case Json::Kind::Number:
				AppendNumber(out, value.asNumber());
				return;
			case Json::Kind::String:
				AppendEscaped(out, value.asString());
				return;
			case Json::Kind::Array:
			{
				out += '[';
				bool first = true;
				for (const Json& item : value.items())
				{
					if (!first)
						out += ',';
					first = false;
					AppendValue(out, item);
				}
				out += ']';
				return;
			}
			case Json::Kind::Object:
			{
				out += '{';
				bool first = true;
				for (const auto& member : value.members())
				{
					if (!first)
						out += ',';
					first = false;
					AppendEscaped(out, member.first);
					out += ':';
					AppendValue(out, member.second);
				}
				out += '}';
				return;
			}
			}
		}

		// --- 読み取り ------------------------------------------------------

		// 再帰下降。fPos が次に読む位置で、失敗したら fError に理由を入れて false を返す。
		class Parser
		{
		public:
			explicit Parser(std::string_view text) : fText(text) {}

			bool run(Json& out)
			{
				skipSpace();
				if (!value(out, 0))
					return false;
				skipSpace();
				if (fPos != fText.size())
					return fail("末尾に余分な文字があります");
				return true;
			}

			const std::string& error() const
			{
				return fError;
			}

		private:
			// **参照ではなく view で持つ**（参照のメンバは clang-tidy が咎める。
			// 読むだけなので view で足りる）。
			std::string_view fText;
			std::size_t fPos = 0;
			std::string fError;

			bool fail(const std::string& why)
			{
				if (fError.empty())
					fError = why + "（" + std::to_string(fPos) + " 文字目）";
				return false;
			}

			bool atEnd() const
			{
				return fPos >= fText.size();
			}

			char peek() const
			{
				return fPos < fText.size() ? fText[fPos] : '\0';
			}

			void skipSpace()
			{
				while (fPos < fText.size())
				{
					const char ch = fText[fPos];
					if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
						++fPos;
					else
						break;
				}
			}

			bool literal(const char* word, Json made, Json& out)
			{
				const std::string text(word);
				if (fText.compare(fPos, text.size(), text) != 0)
					return fail("知らない綴りです");
				fPos += text.size();
				out = std::move(made);
				return true;
			}

			bool value(Json& out, int depth)
			{
				if (depth > kMaxDepth)
					return fail("入れ子が深すぎます");
				if (atEnd())
					return fail("値がありません");
				switch (peek())
				{
				case '{':
					return objectValue(out, depth);
				case '[':
					return arrayValue(out, depth);
				case '"':
				{
					std::string text;
					if (!stringValue(text))
						return false;
					out = Json::string(std::move(text));
					return true;
				}
				case 't':
					return literal("true", Json::boolean(true), out);
				case 'f':
					return literal("false", Json::boolean(false), out);
				case 'n':
					return literal("null", Json::null(), out);
				default:
					return numberValue(out);
				}
			}

			bool objectValue(Json& out, int depth)
			{
				++fPos; // '{'
				Json made = Json::object();
				skipSpace();
				if (peek() == '}')
				{
					++fPos;
					out = std::move(made);
					return true;
				}
				for (;;)
				{
					skipSpace();
					if (peek() != '"')
						return fail("鍵は文字列でなければなりません");
					std::string key;
					if (!stringValue(key))
						return false;
					skipSpace();
					if (peek() != ':')
						return fail("鍵の後に ':' がありません");
					++fPos;
					skipSpace();
					Json member;
					if (!value(member, depth + 1))
						return false;
					made.set(std::move(key), std::move(member));
					skipSpace();
					if (peek() == ',')
					{
						++fPos;
						continue;
					}
					if (peek() == '}')
					{
						++fPos;
						out = std::move(made);
						return true;
					}
					return fail("',' か '}' が必要です");
				}
			}

			bool arrayValue(Json& out, int depth)
			{
				++fPos; // '['
				Json made = Json::array();
				skipSpace();
				if (peek() == ']')
				{
					++fPos;
					out = std::move(made);
					return true;
				}
				for (;;)
				{
					skipSpace();
					Json item;
					if (!value(item, depth + 1))
						return false;
					made.push(std::move(item));
					skipSpace();
					if (peek() == ',')
					{
						++fPos;
						continue;
					}
					if (peek() == ']')
					{
						++fPos;
						out = std::move(made);
						return true;
					}
					return fail("',' か ']' が必要です");
				}
			}

			// \uXXXX 1 つ分を読む（読めたら code に符号位置を入れる）。
			bool hex4(unsigned int& code)
			{
				if (fPos + 4 > fText.size())
					return fail("\\u の後が足りません");
				code = 0;
				for (int i = 0; i < 4; ++i)
				{
					const char ch = fText[fPos + static_cast<std::size_t>(i)];
					unsigned int digit = 0;
					if (ch >= '0' && ch <= '9')
						digit = static_cast<unsigned int>(ch - '0');
					else if (ch >= 'a' && ch <= 'f')
						digit = static_cast<unsigned int>(ch - 'a') + 10U;
					else if (ch >= 'A' && ch <= 'F')
						digit = static_cast<unsigned int>(ch - 'A') + 10U;
					else
						return fail("\\u の後は 16 進 4 桁です");
					code = (code << 4U) | digit;
				}
				fPos += 4;
				return true;
			}

			// 符号位置を UTF-8 で足す。
			static void appendUtf8(std::string& out, unsigned int code)
			{
				if (code < 0x80U)
				{
					out += static_cast<char>(code);
				}
				else if (code < 0x800U)
				{
					out += static_cast<char>(0xC0U | (code >> 6U));
					out += static_cast<char>(0x80U | (code & 0x3FU));
				}
				else if (code < 0x10000U)
				{
					out += static_cast<char>(0xE0U | (code >> 12U));
					out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
					out += static_cast<char>(0x80U | (code & 0x3FU));
				}
				else
				{
					out += static_cast<char>(0xF0U | (code >> 18U));
					out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
					out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
					out += static_cast<char>(0x80U | (code & 0x3FU));
				}
			}

			bool stringValue(std::string& out)
			{
				++fPos; // '"'
				out.clear();
				while (!atEnd())
				{
					const char ch = fText[fPos];
					if (ch == '"')
					{
						++fPos;
						return true;
					}
					if (ch != '\\')
					{
						out += ch;
						++fPos;
						continue;
					}
					++fPos; // '\'
					if (atEnd())
						return fail("傍線の後が足りません");
					const char esc = fText[fPos++];
					switch (esc)
					{
					case '"':
						out += '"';
						break;
					case '\\':
						out += '\\';
						break;
					case '/':
						out += '/';
						break;
					case 'b':
						out += '\b';
						break;
					case 'f':
						out += '\f';
						break;
					case 'n':
						out += '\n';
						break;
					case 'r':
						out += '\r';
						break;
					case 't':
						out += '\t';
						break;
					case 'u':
					{
						unsigned int code = 0;
						if (!hex4(code))
							return false;
						// 上位サロゲートなら下位と組にして 1 文字にする（😀 等）。
						if (code >= 0xD800U && code <= 0xDBFFU && fPos + 1 < fText.size() &&
							fText[fPos] == '\\' && fText[fPos + 1] == 'u')
						{
							const std::size_t mark = fPos;
							fPos += 2;
							unsigned int low = 0;
							if (!hex4(low))
								return false;
							if (low >= 0xDC00U && low <= 0xDFFFU)
								code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
							else
								fPos = mark; // 組でなかった。単独の符号位置として扱う。
						}
						appendUtf8(out, code);
						break;
					}
					default:
						return fail("知らない傍線の綴りです");
					}
				}
				return fail("文字列が閉じていません");
			}

			bool numberValue(Json& out)
			{
				const std::size_t start = fPos;
				if (peek() == '-' || peek() == '+')
					++fPos;
				bool digits = false;
				while (!atEnd() && fText[fPos] >= '0' && fText[fPos] <= '9')
				{
					++fPos;
					digits = true;
				}
				if (!atEnd() && fText[fPos] == '.')
				{
					++fPos;
					while (!atEnd() && fText[fPos] >= '0' && fText[fPos] <= '9')
					{
						++fPos;
						digits = true;
					}
				}
				if (!digits)
					return fail("数として読めません");
				if (!atEnd() && (fText[fPos] == 'e' || fText[fPos] == 'E'))
				{
					++fPos;
					if (!atEnd() && (fText[fPos] == '-' || fText[fPos] == '+'))
						++fPos;
					bool expDigits = false;
					while (!atEnd() && fText[fPos] >= '0' && fText[fPos] <= '9')
					{
						++fPos;
						expDigits = true;
					}
					if (!expDigits)
						return fail("指数の桁がありません");
				}
				// **ロケールを固定して読む**（書き出しと対。core/Json.h「数値」）。
				std::istringstream iss(std::string(fText.substr(start, fPos - start)));
				iss.imbue(std::locale::classic());
				double value = 0.0;
				iss >> value;
				if (iss.fail())
					return fail("数として読めません");
				out = Json::number(value);
				return true;
			}
		};
	} // namespace

	// -----------------------------------------------------------------------
	Json Json::null()
	{
		return {};
	}

	Json Json::boolean(bool value)
	{
		Json made;
		made.fKind = Kind::Bool;
		made.fBool = value;
		return made;
	}

	Json Json::number(double value)
	{
		Json made;
		made.fKind = Kind::Number;
		made.fNumber = value;
		return made;
	}

	Json Json::integer(long long value)
	{
		return number(static_cast<double>(value));
	}

	Json Json::string(std::string value)
	{
		Json made;
		made.fKind = Kind::String;
		made.fString = std::move(value);
		return made;
	}

	Json Json::array()
	{
		Json made;
		made.fKind = Kind::Array;
		return made;
	}

	Json Json::object()
	{
		Json made;
		made.fKind = Kind::Object;
		return made;
	}

	bool Json::asBool(bool fallback) const
	{
		return fKind == Kind::Bool ? fBool : fallback;
	}

	double Json::asNumber(double fallback) const
	{
		return fKind == Kind::Number ? fNumber : fallback;
	}

	std::string Json::asString(const std::string& fallback) const
	{
		return fKind == Kind::String ? fString : fallback;
	}

	const Json& Json::at(const std::string& key) const
	{
		for (const auto& member : fMembers)
			if (member.first == key)
				return member.second;
		return NullValue();
	}

	bool Json::has(const std::string& key) const
	{
		return std::any_of(fMembers.begin(), fMembers.end(),
						   [&key](const std::pair<std::string, Json>& member)
						   { return member.first == key; });
	}

	void Json::push(Json value)
	{
		if (fKind != Kind::Array)
			return;
		fItems.push_back(std::move(value));
	}

	void Json::set(std::string key, Json value)
	{
		if (fKind != Kind::Object)
			return;
		for (auto& member : fMembers)
		{
			if (member.first == key)
			{
				member.second = std::move(value);
				return;
			}
		}
		fMembers.emplace_back(std::move(key), std::move(value));
	}

	std::string Json::dump() const
	{
		std::string out;
		AppendValue(out, *this);
		return out;
	}

	bool Json::parse(const std::string& text, Json& out, std::string& error)
	{
		error.clear();
		Parser parser(text);
		Json made;
		if (!parser.run(made))
		{
			error = parser.error();
			if (error.empty())
				error = "JSON として読めません";
			return false;
		}
		out = std::move(made);
		return true;
	}

	std::string jsonQuote(const std::string& text)
	{
		std::string out;
		AppendEscaped(out, text);
		return out;
	}
} // namespace HomeskzIfcImport::core
