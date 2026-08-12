//
//	tests/Fixtures.h
//
//	テスト共通の小道具。フィクスチャの読み込みと実数の近似比較を 1 か所に置く。
//
//	フィクスチャのパス（HOMESKZ_FIXTURES_DIR）は CMake が各テストターゲットへ
//	コンパイル定義で渡す（tests/CMakeLists.txt）。フィクスチャを読むヘルパー
//	（fixture / allFixtures）は**その定義があるときだけ**現れるので、定義を受け取らない
//	ターゲットで使えばコンパイルエラーになる（登録漏れは黙って通らない）。近似比較
//	（near）はフィクスチャに依存しないので、フィクスチャを使わないテスト
//	（CoreRegionTests 等）もこのヘッダから使える。
//
//	【無 SDK】ここも他のテストと同じく VectorWorks SDK に触れない（CLAUDE.md「テスト方針」）。
//

#pragma once

#include "parse/Loader.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HomeskzIfcTests
{
	// 2 つの実数が許容誤差内で等しいか。座標・高さは mm 単位なので、既定の 1e-6 は
	// Python 版の round(…, 3) より細かい粒度にあたる。**この定義が唯一**で、各テストが
	// ローカルの near を持たない（同じ比較の閾値がテストごとに散らばらないようにする）。
	inline bool near(double a, double b, double tol = 1e-6)
	{
		return std::abs(a - b) < tol;
	}

#ifdef HOMESKZ_FIXTURES_DIR
	// フィクスチャのフルパス。**ディレクトリと名前を連結するのはここだけ**（各テストが
	// 自前で連結すると、置き場所を変えたときに直し漏れる）。ローダ自身をテストする
	// ケースのように、Model ではなくパスが要るときに使う（存在しないファイル名も可）。
	inline std::string fixturePath(std::string_view filename)
	{
		return std::string(HOMESKZ_FIXTURES_DIR) + "/" + std::string(filename);
	}

	// フィクスチャを読む（読み込めなければ ok=false。呼び出し側で CHECK 失敗させる）。
	//
	// 【1 プロセス 1 回だけパースする】実 IFC は 1 ファイル 2MB 前後あり、テストは
	// ASan+UBSan と gcov を載せた -O0 ビルドで走るので、パース 1 回のコストが大きい。
	// 一方ひとつのテスト実行ファイルは同じフィクスチャを何十回も読む（`allFixtures()`
	// を回すケースが並ぶため）。同じファイルからは毎回まったく同じ Model ができる以上、
	// 読み直しは純粋な待ち時間でしかないので、ファイル名をキーに **プロセス内で 1 回
	// だけ** 読んで使い回す（CI のテスト実行時間の主因がこれだった）。
	//
	// そのため戻り値は **const 参照**（コピーを返すと結局その分の時間とメモリを使う）。
	// 参照で返せるのは、parse/ の解析関数がどれも `const Model&` しか取らず——つまり
	// 誰も Model を書き換えないので——共有しても各テストが互いに影響しないから。
	// 読み込み失敗（ok=false）もそのままキャッシュする: 同じファイルなら結果は同じで、
	// 再試行しても答えは変わらない。
	//
	// 引数が `std::string_view`（値渡し）なのは GCC の `-Wdangling-reference` 対策。
	// 参照を返す関数を**一時オブジェクトの参照引数**付きで呼ぶと、GCC 13 は戻り値が
	// その一時を指すかもしれないと見て警告する（ここでは誤検知——返すのは static な
	// キャッシュの中身）。テストは `-Werror` で組むので、これはビルドを止める。値渡しの
	// string_view なら参照引数が無いので警告そのものが起きない（呼び出し側は文字列
	// リテラルでも `std::string` でもそのまま渡せる）。
	inline const HomeskzIfcImport::parse::Model& fixture(std::string_view filename, bool& ok)
	{
		// pair の second が loadIfc の成否。map はイテレータ・参照が無効化されないので、
		// 後から別のフィクスチャを読んでも、先に返した参照はそのまま生きている。
		// std::less<> は string_view のまま引ける（探すたびに std::string を作らない）。
		static std::map<std::string, std::pair<HomeskzIfcImport::parse::Model, bool>, std::less<>>
			cache;

		auto it = cache.find(filename);
		if (it == cache.end())
		{
			bool loaded = false;
			HomeskzIfcImport::parse::Model model =
				HomeskzIfcImport::parse::loadIfc(fixturePath(filename), &loaded);
			it = cache.emplace(std::string(filename), std::make_pair(std::move(model), loaded))
					 .first;
		}
		ok = it->second.second;
		return it->second.first;
	}

	// 検証済みのホームズ君 EX 実 IFC 一式（tests/fixtures/README.md）。**この一覧が唯一の
	// 定義**で、「全フィクスチャに対して回す」テストはここを参照する（各テストが独自の
	// 一覧を持つと、フィクスチャを足したときに一部のテストだけ素通りする）。
	inline const std::vector<std::string>& allFixtures()
	{
		static const std::vector<std::string> names = {
			"サンプル1 (住木邸新築工事).ifc",	"伏図次郎【2階】.ifc",
			"グレー本モデルプラン1【3階】.ifc", "グレー本モデルプラン2【3階】.ifc",
			"スキップフロア_サンプル.ifc",
		};
		return names;
	}
#endif // HOMESKZ_FIXTURES_DIR
} // namespace HomeskzIfcTests
