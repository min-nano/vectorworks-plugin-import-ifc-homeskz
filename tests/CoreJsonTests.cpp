//
//	CoreJsonTests.cpp
//
//	最小 JSON（src/core/Json.h）の単体テスト。**外から来たテキストを読む**ところなので、
//	素直な往復だけでなく「壊れた入力で落ちない・受け付けない」ことを重点的に押さえる。
//

#include "TestFramework.h"
#include "core/Json.h"

#include <string>

using HomeskzIfcImport::core::Json;
using HomeskzIfcImport::core::jsonQuote;

namespace
{
	// 読んで書き戻した 1 行を返す（往復の確認用）。
	std::string RoundTrip(const std::string& text)
	{
		Json value;
		std::string error;
		if (!Json::parse(text, value, error))
			return "<失敗: " + error + ">";
		return value.dump();
	}
} // namespace

TEST(json_dump_scalars)
{
	CHECK_EQ(Json::null().dump(), "null");
	CHECK_EQ(Json::boolean(true).dump(), "true");
	CHECK_EQ(Json::boolean(false).dump(), "false");
	// 整数で表せる数は整数として書く（件数や種別番号が 3.000000 と出ない）。
	CHECK_EQ(Json::number(3.0).dump(), "3");
	CHECK_EQ(Json::integer(-12).dump(), "-12");
	CHECK_EQ(Json::string("あ").dump(), "\"あ\"");
}

TEST(json_dump_escapes)
{
	// 引用符・傍線・制御文字だけを逃がし、UTF-8 はそのまま通す。
	CHECK_EQ(jsonQuote("a\"b\\c"), "\"a\\\"b\\\\c\"");
	CHECK_EQ(jsonQuote("1\n2\t3"), "\"1\\n2\\t3\"");
	CHECK_EQ(jsonQuote(std::string("\x01")), "\"\\u0001\"");
	CHECK_EQ(jsonQuote("柱伏図記号"), "\"柱伏図記号\"");
}

TEST(json_object_keeps_insertion_order)
{
	// **並びは入れた順**（CLAUDE.md「決定性を守る」）。綴り順へ並べ替えない。
	Json value = Json::object();
	value.set("z", Json::integer(1));
	value.set("a", Json::integer(2));
	value.set("m", Json::integer(3));
	CHECK_EQ(value.dump(), "{\"z\":1,\"a\":2,\"m\":3}");

	// 同じ鍵を入れ直しても位置は動かない（値だけ替わる）。
	value.set("z", Json::integer(9));
	CHECK_EQ(value.dump(), "{\"z\":9,\"a\":2,\"m\":3}");
}

TEST(json_array_and_nesting)
{
	Json items = Json::array();
	items.push(Json::string("1-FL"));
	items.push(Json::string("2-FL"));
	Json root = Json::object();
	root.set("layers", items);
	CHECK_EQ(root.dump(), "{\"layers\":[\"1-FL\",\"2-FL\"]}");
	CHECK(root.at("layers").isArray());
	CHECK_EQ(root.at("layers").items().size(), std::size_t(2));
	// 無い鍵は null（呼ぶ側に has() を撒かないための約束）。
	CHECK(root.at("missing").isNull());
	CHECK_EQ(root.at("missing").asString("既定"), std::string("既定"));
}

TEST(json_parse_round_trip)
{
	CHECK_EQ(RoundTrip("{\"a\":1,\"b\":[true,false,null],\"c\":\"あ\"}"),
			 "{\"a\":1,\"b\":[true,false,null],\"c\":\"あ\"}");
	CHECK_EQ(RoundTrip("  [ 1 , 2 , 3 ]  "), "[1,2,3]");
	CHECK_EQ(RoundTrip("{}"), "{}");
	CHECK_EQ(RoundTrip("[]"), "[]");
}

TEST(json_parse_numbers)
{
	Json value;
	std::string error;
	CHECK(Json::parse("[1, -2, 1.5, 2e3, -1.25e-2]", value, error));
	CHECK_EQ(value.items().size(), std::size_t(5));
	CHECK_EQ(value.items()[0].asNumber(), 1.0);
	CHECK_EQ(value.items()[1].asNumber(), -2.0);
	CHECK_EQ(value.items()[2].asNumber(), 1.5);
	CHECK_EQ(value.items()[3].asNumber(), 2000.0);
	CHECK_EQ(value.items()[4].asNumber(), -0.0125);
}

TEST(json_parse_string_escapes)
{
	Json value;
	std::string error;
	CHECK(Json::parse("\"a\\\"b\\\\c\\n\"", value, error));
	CHECK_EQ(value.asString(), std::string("a\"b\\c\n"));

	// \u は UTF-8 へ。サロゲート対も 1 文字にまとめる。
	CHECK(Json::parse("\"\\u67f1\"", value, error));
	CHECK_EQ(value.asString(), std::string("柱"));
	CHECK(Json::parse("\"\\ud83d\\ude00\"", value, error));
	CHECK_EQ(value.asString(), std::string("\U0001F600"));
}

TEST(json_parse_rejects_broken_input)
{
	Json value;
	std::string error;
	const char* const kBroken[] = {
		"",		 "{",	  "[1,2",	  "{\"a\"}", "{\"a\":}", "{a:1}",
		"truex", "\"abc", "[1,2] xx", "01a",	 "--1",		 "{\"a\":1,}",
	};
	for (const char* text : kBroken)
	{
		error.clear();
		CHECK(!Json::parse(text, value, error));
		CHECK(!error.empty());
	}
}

TEST(json_parse_rejects_deep_nesting)
{
	// **深い入れ子で再帰させない**（外から来たテキストでスタックを溢れさせない）。
	std::string text(200, '[');
	Json value;
	std::string error;
	CHECK(!Json::parse(text, value, error));
	CHECK(!error.empty());

	// 上限の内側（数段）は通る。
	CHECK(Json::parse("[[[[[1]]]]]", value, error));
}

TEST_MAIN();
