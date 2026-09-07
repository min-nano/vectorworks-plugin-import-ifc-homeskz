//
//	FeedbackLoopHost.h
//
//	**モードレスの往復（M24）の、殻の中の実物。** SDK 非依存の駆動（src/FeedbackLoop.h）に
//	本物の副作用——本体（ペイロード）への問い合わせ・同梱スクリプト・自動アップデート
//	——を結び、殻の中で 1 つだけ生きる駆動を持つ。パレット（src/Extensions/
//	ExtFeedbackPalette.h）の JS はここを叩き、取り込みコマンド（src/Extensions/ExtMenu.cpp）は
//	往復に入ったときにここへ知らせる。
//
//	【SDK 依存】PayloadSession（本体の確保）・Updater（同梱スクリプト・アップデート）・
//	gSDK（パレットの表示）を使う。殻にコンパイルされる（CMakeLists.txt の VW_SHELL_INPUTS）。
//

#pragma once

#include "FeedbackLoop.h"

#include <string>

namespace HomeskzIfcImport
{
	// 殻の中で 1 つだけの駆動。
	FeedbackLoopDriver& TheFeedbackLoop();

	// **取り込みコマンドが往復に入った**（投稿できた）ときに呼ぶ。駆動を武装し
	// （次の Tick で間隔を待たずに見る）、パレットを表示する。
	void ArmFeedbackLoop();

	// パレットの JS から（src/Extensions/ExtFeedbackPalette.cpp）。いずれも殻の中の
	// 駆動へ取り次ぎ、いまの見え方を返す。
	FeedbackLoopView FeedbackLoopTick();
	FeedbackLoopView FeedbackLoopStop();
	FeedbackLoopView FeedbackLoopCheckNow();

	// パレットの表示・非表示（gSDK->SetWebPaletteVisibility）。
	void ShowFeedbackPalette(bool visible);

	// 見え方を JS へ渡す文字列（JSON）。キーは phase / message / repo / pr / branch /
	// round / build / secondsSinceCheck / secondsUntilNext。**手書きの JSON**——殻は
	// nlohmann::json の使い方を SDK のヘッダ越しにしか知らず、必要なのはこの 1 形だけ。
	std::string FeedbackLoopViewJson(const FeedbackLoopView& view, long long now);
} // namespace HomeskzIfcImport
