//
//	draw/TagStyle.h
//
//	断面寸法データタグスタイルの持ち主（1 回の取り込みにつき 1 つ）。
//
//	【なぜ持ち回すのか】スタイルは**取り込みのたびに文書へ新しく作る**（draw/Tag.cpp の
//	createTagStyle）。作るのは 1 つで、それを伏図（draw/Sheet）と軸組図（draw/Section）の
//	両方が使うので、「作ったスタイル」をフェーズをまたいで運ぶ入れ物が要る。**静的変数に
//	覚えさせてはならない**——スタイルは文書ごとの資源なので、次の文書への取り込みで
//	「作った覚えがあるのに文書には無い」という食い違いが出る（draw/Tag.h の
//	prepareDataTagPlugin と同じ理由）。executeDocument が 1 つ作って両フェーズへ渡す。
//
//	【SDK 非依存のヘッダ】draw/*.h は SDK 型を持てない（draw/DrawUtil.h 冒頭の約束）ので、
//	中身（RefNumber を持つ TagStyleRecord）は draw/Tag.cpp 側に置き、ここは所有者だけを
//	宣言する（draw/ObjectHandles.h と同じ pimpl）。
//

#pragma once

#include <memory>
#include <string>

namespace HomeskzIfcImport::draw
{
	// スタイル生成の結果。定義は draw/Tag.cpp（SDK 型 RefNumber を持つため）。
	struct TagStyleRecord;

	// 生成したデータタグスタイルの所有者。コピー不可（1 回の取り込みで 1 つのスタイルを
	// 共有する意図を型で示す。draw/ObjectHandles と同じ）。
	class TagStyle
	{
	public:
		TagStyle();
		~TagStyle();
		TagStyle(const TagStyle&) = delete;
		TagStyle& operator=(const TagStyle&) = delete;

		TagStyleRecord& record()
		{
			return *fRecord;
		}
		const TagStyleRecord& record() const
		{
			return *fRecord;
		}

	private:
		std::unique_ptr<TagStyleRecord> fRecord;
	};

	// 生成の結果を人が読める 1 行にする（作れていれば付いた名前、作れなければ躓いた段階）。
	// **実描画はローカルの VW でしか確認できない**ので、タグが空で出たときに「スタイルを
	// 作れなかったのか・レイアウトが載らなかったのか」を切り分けられるようにする。
	std::string tagStyleDiagnostics(const TagStyle& style);
} // namespace HomeskzIfcImport::draw
