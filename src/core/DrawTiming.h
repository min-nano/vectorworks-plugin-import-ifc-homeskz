//
//	core/DrawTiming.h
//
//	**描画のどこで時間を使っているかを区間ごとに測る**ための道具。名前付きの区間へ
//	経過時間を積み、最後に 1 つの文章へ整形するだけの純ロジックで、時計以外は何も知らない。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない（CLAUDE.md「依存の向き」）。
//
//	【なぜ要るのか】取り込みの実測では**構造材（横架材＋柱）が全体の 51〜68%**（1 本
//	93〜130ms）で、次が軸組図・伏図、そして底盤という内訳になる（core/Progress の kCosts に
//	ある 1 件あたりの重さ × 実データの命令数）。ところが**その 1 本の中の何が重いのか**
//	——オブジェクトの生成か・パラメータ名の解決か・`ResetObject` そのものか——は、
//	フェーズ単位の時刻差からは分からない。実描画はローカルの VectorWorks でしか走らない
//	（CLAUDE.md「テスト方針」）ので、**実機フィードバックの往復 1 周で内訳が数字で返る**
//	形にしておくのが、次にどこを削るかを当てずっぽうで決めないための唯一の手立てになる。
//
//	【集計先が 1 つである理由】計測点は draw/ のあちこちに散る（構造材・データタグ・スラブ・
//	ビューポート）。そこへ集計先を引数で配って回ると、**開発ビルドだけのもののために本番の
//	関数の引数が増える**——`drawTiming()` の 1 つへ積むことでそれを避ける。取り込みは
//	VectorWorks のメインスレッドから 1 本で走るので排他は持たない（core/Trace.h と同じ
//	理由）。
//
//	【使う側の作法】区間は**入れ子にしない**。入れ子にすると同じ時間が 2 つの区間に
//	二重計上され、合計を読んだ人が必ず取り違える。ある処理をどの粒度で測るかは 1 か所で
//	決めて、その内側では測らない。囲み方（`VW_DRAW_TIME`）と、開発ビルドだけに
//	コンパイルするスイッチ（`VW_DRAW_TIMING`）は draw/Verify.h にある。
//
//	**そして、その作法は守られているかを自分で見る。** 計測点は draw/ のあちこちに散るので、
//	「外から呼ばれた共有の関数が、自分でも区間を開いていた」という取りこぼしが必ず起きる
//	（実際、この計測を入れた PR のレビューで 1 件見つかった——潰れた材の自己修復から
//	`CreatePath` を呼ぶ経路。draw/StructuralMember の `CreatePathUntimed`）。規約をコメント
//	にだけ書いても次に同じことが起きるので、**入れ子になったこと自体を数えて報告へ出す**
//	——二重計上は消せないが、**気付かないまま数字を信じることは無くなる**。
//
//	【本番ビルドにも残るもの】この翻訳単位そのものは常にコンパイルされる（core/ は
//	無 SDK テストの対象なので、スイッチで消すとテストできなくなる）。**呼ぶ側が
//	`VW_DRAW_TIMING` で消える**ので、本番ビルドでは誰も触らない表が 1 つ残るだけになる。
//

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 区間ごとの累計時間と回数。名前は自由文字列で、同じ名前へ積めば足し合わされる。
	class TimingTable final
	{
	public:
		// 1 区間ぶんの累計（整形と単体テストのために公開する）。
		struct Entry
		{
			std::string name;
			double milliseconds = 0.0;
			std::size_t count = 0;
		};

		// name の区間へ経過時間を積む（負の値は 0 として扱う——単調時計でも呼び出し側の
		// 取り違えで負が来うるので、合計が減らない形にしておく）。
		void add(std::string_view name, double milliseconds);

		// 積んだものを全部捨てる（取り込みの頭で呼び、周ごとに測り直す）。
		void clear();

		bool empty() const
		{
			return fEntries.empty();
		}

		// **時間の長い順**に並べた一覧（同じ時間なら最初に積まれた方が先。エンティティの
		// 列挙順に結果が左右されないため。CLAUDE.md「決定性を守る」）。
		std::vector<Entry> sorted() const;

		// 積んだものの合計（ミリ秒）。
		double total() const;

		// 診断ログへ載せる本文。1 行目が見出し（合計つき）、以降が 1 区間 1 行。
		// **入れ子になった区間があれば最後に警告の 1 行を足す**（下記 noteNested）。
		// **何も積まれていなければ空文字**を返す（呼び出し側は AppendLine へ渡すだけでよい）。
		std::string format(std::string_view heading) const;

		// --- 入れ子の見張り（上記「その作法は守られているかを自分で見る」）--------------
		//
		// TimingScope が構築・破棄のたびに呼ぶ。**入れ子は禁じ手なので防がない**——
		// 防ごうとすると「外側だけ数える／内側だけ数える」のどちらを選んでも
		// 合計が静かに狂う。**起きたことを名前で控えて報告へ出す**だけにする。

		// 区間へ入る。**既に別の区間の中だった**なら true（＝入れ子になった）。
		bool enterScope();

		// 区間から出る（入っていなければ何もしない）。
		void leaveScope();

		// 入れ子になった区間の名前を控える（同じ名前は 1 度だけ）。
		void noteNested(std::string_view name);

		// 入れ子になった区間の名前（登場順・重複なし）。無ければ空。
		const std::vector<std::string>& nested() const
		{
			return fNested;
		}

	private:
		std::vector<Entry> fEntries;
		std::vector<std::string> fNested;
		std::size_t fDepth = 0;
	};

	// 描画の計測の**唯一の集計先**（上記「集計先が 1 つである理由」）。
	TimingTable& drawTiming();

	// 構築から破棄までを 1 区間として積む（RAII）。途中で例外が出ても積まれる。
	// **名前は文字列リテラルを想定する**——寿命がスコープより長いものだけを渡すこと。
	class TimingScope final
	{
	public:
		explicit TimingScope(std::string_view name)
			: fName(name), fNested(drawTiming().enterScope()),
			  fStart(std::chrono::steady_clock::now())
		{
		}

		~TimingScope()
		{
			const std::chrono::duration<double, std::milli> elapsed =
				std::chrono::steady_clock::now() - fStart;
			drawTiming().leaveScope();
			drawTiming().add(fName, elapsed.count());
			// **入れ子だったことを報告へ残す**（TimingTable「入れ子の見張り」）。
			if (fNested)
				drawTiming().noteNested(fName);
		}

		TimingScope(const TimingScope&) = delete;
		TimingScope& operator=(const TimingScope&) = delete;
		TimingScope(TimingScope&&) = delete;
		TimingScope& operator=(TimingScope&&) = delete;

	private:
		std::string_view fName;
		bool fNested = false;
		std::chrono::steady_clock::time_point fStart;
	};
} // namespace HomeskzIfcImport::core
