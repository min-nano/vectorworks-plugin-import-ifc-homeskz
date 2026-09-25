//
//	draw/ProgressDialog.h
//
//	VectorWorks の進捗ダイアログへ橋渡しする ProgressReporter（core/Progress.h）の実装。
//	インポートの入口（Extensions/ExtMenu）がこれを 1 つ作り、Phase 1（parse::buildDocument）と
//	Phase 2（draw::executeDocument）の両方へ同じものを渡す。
//
//	【なぜ要るか】インポートの体感時間はほぼすべて描画で、その間 VectorWorks は再描画も
//	イベント処理もしないため**フリーズしたように見える**。進捗ダイアログは (1) いま何を
//	何件目まで進めたかを見せ、(2) 1 件ごとに yield して VW に描き直す機会を与え、
//	(3) キャンセルを受け付ける——この 3 つを 1 か所で担う（core/Progress.h「なぜ要るか」）。
//
//	【SDK 依存】実装（draw/ProgressDialog.cpp）は PluginPrefix.h（VectorWorks SDK）と
//	VWFC の進捗ダイアログ（VWFC::Tools::CProgressDlg）を include する。このヘッダは
//	core/Progress.h までしか参照せず、SDK 型を pimpl の内側へ隠すので、SDK を持たない
//	翻訳単位からも安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Progress.h"

#include <memory>
#include <string>

namespace HomeskzIfcImport::draw
{
	// 進捗ダイアログ。構築で開き、close()（またはデストラクタ）で閉じる。
	//
	// **完了ダイアログを出す前に close() すること。** 進捗ダイアログを開いたまま
	// モーダルの完了通知を出すと、2 枚のダイアログが重なる。
	class ProgressDialog final : public core::ProgressReporter
	{
	public:
		// title はダイアログのタイトル、topText は上段に出す 1 行（インポート対象の
		// ファイル名を想定）。canCancel=true でキャンセルボタンを出す（押されたことは
		// cancelled() が返し、描画側がその時点で切り上げる）。
		ProgressDialog(const std::string& title, const std::string& topText, bool canCancel = true);
		~ProgressDialog() override;

		// 明示的に閉じる（2 回呼んでも安全）。
		void close();

	protected:
		// core::ProgressReporter のフック。見出しの更新・1 件ぶんの前進（＝yield）・
		// キャンセルの問い合わせを、それぞれ SDK の進捗ダイアログへ流す。
		void onBeginPhase(const core::ProgressStatus& status, double share) override;
		void onStep(const core::ProgressStatus& status) override;
		bool onCancelled() override;

	private:
		// SDK 型（VWFC::Tools::CProgressDlg）をヘッダから隠すための pimpl。
		struct Impl;
		std::unique_ptr<Impl> fImpl;
	};
} // namespace HomeskzIfcImport::draw
