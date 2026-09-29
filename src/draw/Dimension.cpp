//
//	draw/Dimension.cpp
//
//	寸法・レベル記号の描画（意図と作法は draw/Dimension.h）。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するので、プラグインビルド（SDK あり）でのみコンパイルする。
//
//	使う SDK API:
//	  * gSDK->CreateLinearDimension(p1, p2, startOffset, 0, (0,0), dimType) … 直線寸法
//	  * gSDK->SetObjectVariable(h, ovDimStandardName, 名前)                … 寸法規格
//	  * gSDK->CreateChainDimension(h1, h2)                                  … 連続寸法へ繋ぐ
//	  * gSDK->AddViewportAnnotationObject(viewport, h)                      … 注釈へ移す
//	  * gSDK->CreateCustomObject("Elevation Benchmark2", 点, 0, true)       … レベル基準線
//	  * VWParametricObj::SetPointObjectPos / SetParamString                 … 位置・Axis
//	  * gSDK->GetCustomObjectProfileGroup / SetCustomObjectProfileGroup     … マーカーレイアウト
//	  * gSDK->CreateTextBlock / AddObjectToContainer                        … 名前のテキスト
//

#include "PluginPrefix.h"
#include "draw/Dimension.h"
#include "draw/DrawUtil.h"
#include "draw/Verify.h"
#include "core/Document.h"
#include "core/Layout.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 直線寸法の種類（dimType ＝ ovDimClass）。0 は fix_ang（角度固定）。**水平／垂直は
		// dimType ではなく 2 点の取り方で決まる**（Findings「Dimensions」）ので、測点を
		// 水平・垂直に並べて渡せばよい。
		constexpr short kDimTypeFixedAngle = 0;

		// 縮尺が決まらなかった（用紙が読めない等）ときに寸法線までの距離を測る縮尺の分母。
		// 取り込みの既定の縮尺（1/100）と同じ。
		constexpr double kFallbackScale = 100.0;

		// レベル基準線の universal 名（ローカライズ名「レベル基準線」。Findings
		// 「Level Objects」の実測表）。
		constexpr const char* kLevelMarkPlugin = "Elevation Benchmark2";

		// ストーリレベルへ結ぶ 3 つ組（Findings「Level Objects」）。**3 つ揃って初めて効く**
		// （Datum 単独では GroundPlane へ倒される）。Axis は既定の ZAxis3DMode のまま触らない
		// ——YAxis2DMode にすると関連付けが外れる。結べば高さはストーリレベルから来るので、
		// 記号をドラッグしても数値は動かない（round 3 のご指摘）。
		constexpr const char* kParamStoryName = "__StoryName";
		constexpr const char* kParamLevelTypeName = "__LevelTypeName";
		constexpr const char* kParamDatum = "Datum";
		constexpr const char* kDatumStoryLevel = "StoryLevel";

		// 描いた高さ（読み取り用の静的文字。Findings「Level Objects」のパラメータ表）。
		constexpr const char* kParamShownElevation = "Elevation";

		// PIO 自身が挿入点から左へ引く水平引出線（既定 True）。切って、基準線は
		// レイアウトに自分で引く（DrawLevelLine）。
		constexpr const char* kParamHorizontalLeader = "UseHorizontalLeader";

		// 描いた高さが命令の高さからこれ以上ずれていたら「合わない」と数える（mm）。
		constexpr double kLevelHeightTol = 0.5;

		// 測点 a → b の直線寸法を 1 本作る。axis=Horizontal なら (a, base)→(b, base) を、
		// Vertical なら (base, a)→(base, b) を測る。offset は根元から寸法線までの距離
		// （符号は図面の座標軸で + が上／右。draw/Dimension.h）。
		MCObjectHandle CreateOne(core::DimensionAxis axis, double a, double b, double base,
								 double offset)
		{
			const bool horizontal = axis == core::DimensionAxis::Horizontal;
			const WorldPt p1 = horizontal ? WorldPt(a, base) : WorldPt(base, a);
			const WorldPt p2 = horizontal ? WorldPt(b, base) : WorldPt(base, b);
			return gSDK->CreateLinearDimension(p1, p2, offset, 0.0, Vector2(0.0, 0.0),
											   kDimTypeFixedAngle);
		}

		// 仕上がった寸法（連続寸法または繋げなかった直線寸法）を注釈へ移す。移せなければ
		// 消す（アクティブレイヤに寸法だけが浮かないように）。
		bool Annotate(MCObjectHandle viewport, MCObjectHandle object)
		{
			if (gSDK->AddViewportAnnotationObject(viewport, object))
				return true;
			gSDK->DeleteObject(object, true);
			return false;
		}

#if VW_DRAW_VERIFY
		// 検算（dev だけ）: 寸法 1 本の見え方に効くオブジェクト変数を読んで 1 行にする
		// （round 1: 軸組図だけ寸法値が出なかった。伏図と並べて違いを探す）。
		std::string DescribeDimension(MCObjectHandle dimension)
		{
			const auto real = [dimension](short selector) -> std::string
			{
				TVariableBlock value;
				Real64 number = 0.0;
				if (gSDK->GetObjectVariable(dimension, selector, value) == 0 ||
					!value.GetReal64(number))
					return "?";
				std::array<char, 32> buffer{};
				std::snprintf(buffer.data(), buffer.size(), "%g", number);
				return buffer.data();
			};
			const auto flag = [dimension](short selector) -> std::string
			{
				TVariableBlock value;
				bool on = false;
				if (gSDK->GetObjectVariable(dimension, selector, value) == 0 ||
					!value.GetBoolean(on))
					return "?";
				return on ? "on" : "off";
			};
			std::string name = "?";
			if (TVariableBlock value;
				gSDK->GetObjectVariable(dimension, ovDimStandardName, value) != 0)
			{
				TXString text;
				if (value.GetTXString(text))
					name = static_cast<const char*>(text);
			}
			std::string style = "クラス";
			if (!gSDK->GetTextStyleByClass(dimension))
			{
				TXString styleName;
				gSDK->InternalIndexToNameN(gSDK->GetTextStyleRef(dimension), styleName);
				style = static_cast<const char*>(styleName);
			}
			return "値表示 " + flag(ovDimShowValue) + " / 文字 " + real(ovDimTextSizeInPoints) +
				   "pt・" + real(ovDimFontSize) + "mm / 文字スタイル " + style + " / 規格 " + name;
		}
#endif

		// 寸法の文字の当て方（ビューポート 1 枚ぶん。drawViewportDimensions が決める）。
		//   style    … 寸法規格の文字スタイル（ref number。0 なら当てない）
		//   fontSize … 文字の図面上の大きさ（mm）＝ 文字スタイルの紙の pt × 25.4/72 ×
		//              ビューポートの縮尺（0 なら書かない）
		struct DimensionText
		{
			InternalIndex style = 0;
			double fontSize = 0.0;
		};

		// 列 1 本を置く。1 本でも注釈へ置けたら true。scale は寸法線までの距離に使う縮尺の
		// 分母。
		bool PlaceChain(MCObjectHandle viewport, const core::DimensionChainCommand& chain,
						const std::string& standard, double scale, const DimensionText& text,
						DimensionCounts& counts)
		{
			const double line = core::dimensionLineCoord(chain.base, chain.side, chain.tier, scale);
			const double offset = line - chain.base;

			// 繋ぎかけの連続寸法（または 1 本目の直線寸法）と、その中の寸法の本数。
			MCObjectHandle current = nil;
			std::size_t inCurrent = 0;
			std::size_t placed = 0;
			const auto flush = [&]()
			{
				if (current == nil)
					return;
				if (Annotate(viewport, current))
					placed += inCurrent;
				else
					counts.failed += inCurrent;
				current = nil;
				inCurrent = 0;
			};

			for (std::size_t i = 0; i + 1 < chain.stops.size(); ++i)
			{
				const MCObjectHandle dimension =
					CreateOne(chain.axis, chain.stops[i], chain.stops[i + 1], chain.base, offset);
				if (dimension == nil)
				{
					// 1 本作れなければそこで列が切れる（前の区間までを先に仕上げる）。
					++counts.failed;
					flush();
					continue;
				}
				// **規格は繋ぐ前に当てる**（連続寸法そのものには効かない）。図面に無い
				// 名前は false で弾かれ、文書の既定の規格のまま残る（数えて診断へ）。
				if (!SetTextVariable(dimension, ObjectVariable::DimStandardName, standard))
					++counts.standardRejected;
				// 寸法値は**明示して出す**（round 1: 軸組図の注釈で値が出なかった。伏図では出た）。
				SetBooleanVariable(dimension, ObjectVariable::DimShowValue, true);
				// **文字スタイルを明示し、文字の大きさをビューポートの縮尺で書いて引き直す**
				// （Findings「Dimensions」#157 / #161）。
				//   * 注釈に置く寸法の文字の大きさは、作るとき（SetTextStyleRef を呼ぶとき）の
				//     アクティブレイヤの縮尺で焼き付く。軸組図はシートレイヤ（1:1）がアクティブ
				//     なので、そのままでは 1/125 の図の上で紙 0.02mm になり値が見えなかった
				//     （round 1〜3。OIP で文字スタイルを選び直すと出たのは、注釈の縮尺で焼き直す
				//     ため）。
				//   * 大きさ（ovDimFontSize）は**書いただけでは絵に出ない**——ResetObject で
				//     引き直す。読み戻しは書いた値を返すので、呼び忘れても気付けない。
				//   * **繋ぐ前に**済ませる。文字スタイルが明示してあれば、繋いで作り直された
				//     中の直線寸法も書いた大きさを保つ（〈クラスの文字スタイル〉のままだと、
				//     繋ぐときのアクティブレイヤの縮尺で焼き直される。#155）。
				//   * 文字スタイルは寸法規格のもの（「寸法(6pt)」）をそのまま使い、縮尺ごとの
				//     文字スタイルは作らない（名前付きリソースを増やさない。docs/DEV-NOTES.md M31）。
				if (text.style != 0)
				{
					gSDK->SetTextStyleRef(dimension, text.style);
					if (text.fontSize > 0.0)
						SetRealVariable(dimension, ObjectVariable::DimFontSize, text.fontSize);
					else
						++counts.textSizeUnread;
					gSDK->ResetObject(dimension);
				}
				else
					++counts.textStyleMissing;
#if VW_DRAW_VERIFY
				if (counts.dimensionProbe.empty())
					counts.dimensionProbe = DescribeDimension(dimension);
#endif
				SetClassByName(dimension, kDimensionClass);

				if (current == nil)
				{
					current = dimension;
					inCurrent = 1;
					continue;
				}
				const MCObjectHandle joined = gSDK->CreateChainDimension(current, dimension);
				if (joined == nil)
				{
					// 繋げなかった。ここまでを仕上げ、この寸法から新しく繋ぎ直す。
					++counts.unjoined;
					flush();
					current = dimension;
					inCurrent = 1;
					continue;
				}
				current = joined;
				++inCurrent;
			}
			// 連続寸法も寸法クラスへ（中の直線寸法は作ったときに置いてある）。
			if (current != nil && inCurrent > 1)
				SetClassByName(current, kDimensionClass);
			flush();

			counts.dimensions += placed;
			return placed > 0;
		}

		// レベル記号の形を決めるもの（ビューポート 1 枚ぶん。drawViewportDimensions が決める）。
		//   textSize       … 名前の文字の大きさ（用紙 mm。寸法の文字と同じ。0 なら既定の
		//                    レイアウトの名前の大きさのまま）
		//   dimensionScale … 寸法線までの距離に使った縮尺の分母
		//   markScale      … 記号を描くビューポートの縮尺の分母（用紙 mm → 注釈空間）
		struct LevelMarkStyle
		{
			double textSize = 0.0;
			double dimensionScale = kFallbackScale;
			double markScale = kFallbackScale;
		};

		// 線を 1 本作って container へ入れる。入らなければ消す。
		bool AddLine(MCObjectHandle container, double x1, double y1, double x2, double y2)
		{
			const MCObjectHandle line = gSDK->CreateLine(WorldPt(x1, y1), WorldPt(x2, y2));
			if (line == nil)
				return false;
			if (gSDK->AddObjectToContainer(line, container))
				return true;
			gSDK->DeleteObject(line, true);
			return false;
		}

#if VW_DRAW_VERIFY
		// 検算（dev だけ）: container の中身を「型＋外形」で並べる（入れ子のグループも辿る。
		// 深さは有限に留める）。
		std::string DescribeMembers(MCObjectHandle container, int depth = 0);

		// 検算（dev だけ）: 外形を "[左,右]x[下,上]" の 1 語にする。
		std::string DescribeBounds(MCObjectHandle object)
		{
			WorldRect bounds;
			if (!gSDK->GetObjectBounds(object, bounds))
				return "?";
			std::array<char, 96> buffer{};
			std::snprintf(buffer.data(), buffer.size(), "[%.1f,%.1f]x[%.1f,%.1f]", bounds.left,
						  bounds.right, std::min(bounds.top, bounds.bottom),
						  std::max(bounds.top, bounds.bottom));
			return buffer.data();
		}

		std::string DescribeMembers(MCObjectHandle container, int depth)
		{
			constexpr int kMaxDepth = 2;
			std::string out;
			for (MCObjectHandle h = gSDK->FirstMemberObj(container); h != nil;
				 h = gSDK->NextObject(h))
			{
				const short type = gSDK->GetObjectTypeN(h);
				out += " 型" + std::to_string(type) + DescribeBounds(h);
				if (type == kGroupNode && depth < kMaxDepth)
					out += "{" + DescribeMembers(h, depth + 1) + " }";
			}
			return out.empty() ? std::string(" （なし）") : out;
		}
#endif

		// **マーカーレイアウトを組み直して渡し直す**（draw/Dimension.h「レベル基準線の作法」）。
		// 既定の中身（高さとストーリレベル名のテキスト・記号のポリライン）を消し、挿入点＝(0, 0)
		// から右へ「▽＋名前」を置く（core/Layout.h「軸組図のレベル記号の形と位置」）。**基準線は
		// 置かない**——線は起点が決まってから DrawLevelLine が足す。起点を決めるのに
		// 要る記号の幅（用紙 mm）を markWidth へ返す。
		// **中身を入れ替えるだけでは絵に出ない**ので、新しい図形を作って入れ、古いものを
		// 消してから SetCustomObjectProfileGroup で渡す（Findings「Level Objects」の実測手順）。
		bool RebuildLevelLayout(MCObjectHandle mark, const core::LevelMarkCommand& level,
								const LevelMarkStyle& style, double& markWidth,
								[[maybe_unused]] std::string& probe)
		{
			markWidth = 0.0;
			const MCObjectHandle layout = HeldProfileGroup(mark);
			if (layout == nil)
				return false;

			// 既定の中身を控える（型 0 は群の終端なので残す）。寸法の文字の大きさが読めな
			// かったときは、既定の名前のテキストの大きさを使う。
			std::vector<MCObjectHandle> old;
			WorldCoord fallbackSize = 0.0;
			for (MCObjectHandle h = gSDK->FirstMemberObj(layout); h != nil; h = gSDK->NextObject(h))
			{
				const short type = gSDK->GetObjectTypeN(h);
				if (type == 0)
					continue;
				old.push_back(h);
				if (type == kTextNode && fallbackSize <= 0.0)
					gSDK->GetTextSize(h, 0, fallbackSize);
			}
#if VW_DRAW_VERIFY
			if (probe.empty())
			{
				probe = "組み直す前のレイアウト";
				for (const MCObjectHandle h : old)
					probe += " 型" + std::to_string(gSDK->GetObjectTypeN(h)) + DescribeBounds(h);
			}
#endif
			const double size = style.textSize > 0.0 ? style.textSize : fallbackSize;

			// 名前。左揃えで作り、測ってから ▽ の右・線の上へ動かす。
			const TXString chars(level.name.c_str());
			const MCObjectHandle text = gSDK->CreateTextBlock(chars, WorldPt(0.0, 0.0), false, 0);
			if (text == nil)
				return false;
			if (!gSDK->AddObjectToContainer(text, layout))
			{
				gSDK->DeleteObject(text, true);
				return false;
			}
			gSDK->SetTextJustification(text, kTextLeftJustify);
			if (size > 0.0)
				gSDK->SetTextSize(text, 0, static_cast<Sint32>(chars.GetLength()), size);
			WorldRect bounds;
			double textWidth = 0.0;
			if (gSDK->GetObjectBounds(text, bounds))
				textWidth = bounds.right - bounds.left;
			const core::LevelMarkShape shape = core::levelMarkShape(size, textWidth);
			if (textWidth > 0.0)
				gSDK->MoveObject(text, shape.textLeft - bounds.left,
								 shape.textBottom - std::min(bounds.top, bounds.bottom));
			markWidth = shape.width;

			// ▽（頂点で基準線に触れる正三角形）。
			const double h = shape.triangleHeight;
			const double w = shape.triangleHalfWidth;
			bool drawn = true;
			if (h > 0.0)
			{
				drawn = AddLine(layout, 0.0, h, 2.0 * w, h) && drawn;
				drawn = AddLine(layout, 0.0, h, w, 0.0) && drawn;
				drawn = AddLine(layout, 2.0 * w, h, w, 0.0) && drawn;
			}

			for (const MCObjectHandle object : old)
				gSDK->DeleteObject(object, true);
			return gSDK->SetCustomObjectProfileGroup(mark, layout) != 0 && drawn;
		}

		// **PIO 自身が引く水平引出線を消し、基準線はレイアウトに自分で引く。**
		// 実機で分かったこと（docs/DEV-NOTES.md「レベル基準線の描き方の調整」）:
		//   * 既定のレイアウトに線は無く、PIO が挿入点から左へ 5400（1/150 で用紙 36mm）の
		//     ポリゴンを 1 つ描く。パス（GetCustomObjectPath）は持たない。
		//   * カスタム制御点（既定 (0, 3000)）を動かしても、そのポリゴンは変わらない。
		//   * 全パラメータのどれにも -5400 は無い。線に関わりそうなのは
		//     `UseHorizontalLeader`（水平引出線を使用。既定 True）だけ。
		// そこで引出線を切り（kParamHorizontalLeader）、線は起点から右へレイアウトの中に
		// 引く（最初の版で、レイアウトの線は右へ図の右端まで出た）。
		bool DrawLevelLine(MCObjectHandle mark, double lengthOnPaper)
		{
			bool leaderOff = false;
			try
			{
				VWParametricObj pio(mark);
				pio.SetParamBool(kParamHorizontalLeader, false);
				leaderOff = !pio.GetParamBool(kParamHorizontalLeader);
			}
			catch (...)
			{
				leaderOff = false;
			}
			const MCObjectHandle layout = HeldProfileGroup(mark);
			if (layout == nil)
				return false;
			const bool drawn = AddLine(layout, 0.0, 0.0, lengthOnPaper, 0.0);
			return gSDK->SetCustomObjectProfileGroup(mark, layout) != 0 && drawn && leaderOff;
		}

#if VW_DRAW_VERIFY
		// 検算（dev だけ）: 制御点の持ち方を探る——カスタム制御点の 1 つ目と、全パラメータの
		// 「名前=値」を 1 行にする（どこに -5400 があるかを見る）。
		std::string DescribeControlPoints(MCObjectHandle mark)
		{
			try
			{
				const VWParametricObj pio(mark);
				std::string out = "カスタム制御点 ";
				VWPoint3D point;
				if (pio.CustomControlPointsGet(0, point))
				{
					std::array<char, 64> buffer{};
					std::snprintf(buffer.data(), buffer.size(), "(%g, %g)", point.x, point.y);
					out += buffer.data();
				}
				else
					out += "なし";
				out += " / 欄";
				const size_t count = pio.GetParamsCount();
				for (size_t i = 0; i < count; ++i)
				{
					out += " ";
					out += pio.GetParamName(i).GetStdString();
					out += "=";
					out += pio.GetParamAsString(i).GetStdString();
				}
				return out;
			}
			catch (...)
			{
				return "読めない";
			}
		}
#endif

#if VW_DRAW_VERIFY
		// 検算（dev だけ）: container の中のテキストを**入れ子のグループまで**辿って集める
		// （PIO が吐いた図形は、レイアウトを写したグループの中にテキストを持つことがある）。
		// 深さは有限に留める（グループの入れ子は数段しか無い）。
		void CollectTexts(MCObjectHandle container, std::vector<std::string>& out, int depth = 0)
		{
			constexpr int kMaxDepth = 4;
			for (MCObjectHandle h = gSDK->FirstMemberObj(container); h != nil;
				 h = gSDK->NextObject(h))
			{
				const short type = gSDK->GetObjectTypeN(h);
				if (type == kTextNode)
					out.emplace_back(static_cast<const char*>(gSDK->GetTextChars(h)));
				else if (type == kGroupNode && depth < kMaxDepth)
					CollectTexts(h, out, depth + 1);
			}
		}

		std::string JoinTexts(const std::vector<std::string>& texts)
		{
			std::string joined;
			for (const std::string& text : texts)
				joined += "〈" + text + "〉";
			return joined.empty() ? std::string("（なし）") : joined;
		}

		// 検算（dev だけ）: レベル基準線が描いた文字に name があるか（Findings「Level
		// Objects」の「描いた文字を機械で読む」）。見つからなければ 1 個目について
		// 「描いた文字」と「レイアウトの中身」を probe へ控える（実機で何が起きたかを
		// 持ち帰る。差し替えが絵に届いていないのか、読み方が違うのかを分ける）。
		bool DrawsText(MCObjectHandle mark, const std::string& name, std::string& probe)
		{
			std::vector<std::string> drawn;
			CollectTexts(mark, drawn);
			if (std::ranges::find(drawn, name) != drawn.end())
				return true;
			if (probe.empty())
			{
				std::vector<std::string> layout;
				const MCObjectHandle direct = gSDK->GetCustomObjectProfileGroup(mark);
				const MCObjectHandle aux = gSDK->GetCustomObjectProfileGroupInAux(mark);
				const MCObjectHandle held = direct != nil ? direct : aux;
				if (held != nil)
					CollectTexts(held, layout);
				const char* source = "無し";
				if (direct != nil)
					source = "直接";
				else if (aux != nil)
					source = "aux";
				probe = name + ": 描いた文字 " + JoinTexts(drawn) + " / レイアウト（" + source +
						"）" + JoinTexts(layout);
			}
			return false;
		}
#endif

		// 描いた高さ（"10953.18" のような文字）を数に読む。読めなければ false。
		bool ParseShownHeight(const std::string& shown, double& value)
		{
			std::string digits;
			for (const char c : shown)
			{
				if (c != ',' && c != ' ')
					digits.push_back(c);
			}
			if (digits.empty())
				return false;
			char* end = nullptr;
			value = std::strtod(digits.c_str(), &end);
			return end != digits.c_str();
		}

		// **描いた高さを読み戻して命令の高さと引き比べる**（直さない）。ストーリレベルへ
		// 結んだ個体の高さは書けない出力（Elev）なので、食い違いは数えて診断へ出す——
		// 0 なら結べていないか、断面の向きがビュー行列へ写っていない（CopySectionViewMatrix）。
		void CheckLevelHeight(MCObjectHandle mark, double elevation, DimensionCounts& counts)
		{
			try
			{
				const VWParametricObj pio(mark);
				const std::string shown =
					static_cast<const char*>(pio.GetParamString(kParamShownElevation));
				double value = 0.0;
				if (!ParseShownHeight(shown, value))
				{
					++counts.levelHeightUnread;
					return;
				}
				if (std::abs(value - elevation) <= kLevelHeightTol)
					return;
				++counts.levelHeightMismatch;
				if (counts.levelHeightProbe.empty())
				{
					std::array<char, 128> buffer{};
					std::snprintf(buffer.data(), buffer.size(), "命令 %g に対し描いた高さ %s",
								  elevation, shown.c_str());
					counts.levelHeightProbe = buffer.data();
				}
			}
			catch (...)
			{
				++counts.levelHeightUnread;
			}
		}

		// レベル記号 1 つを注釈へ置き、ストーリレベルへ結んでレイアウトを組み直す。注釈へ
		// 置けたらそのハンドル、置けなければ nil。記号の幅（用紙 mm）を markWidth へ返す——
		// 起点は同じ図の全記号の幅が揃ってから決める（PositionLevel）。
		MCObjectHandle PlaceLevel(MCObjectHandle viewport, const core::LevelMarkCommand& level,
								  const LevelMarkStyle& style, double& markWidth,
								  DimensionCounts& counts)
		{
			markWidth = 0.0;
			const MCObjectHandle mark = gSDK->CreateCustomObject(
				TXString(kLevelMarkPlugin), WorldPt(level.x, level.elevation), 0.0, true);
			if (mark == nil)
				return nil;
			if (!Annotate(viewport, mark))
				return nil;
			SetClassByName(mark, kDimensionClass);

			try
			{
				VWParametricObj pio(mark);
				// 注釈へ移すと VW が決めた位置へ落ちるので、座標を明示し直す（縦の位置は
				// 絵の置き場所だけで、描く数値には効かない）。
				pio.SetPointObjectPos(VWPoint2D(level.x, level.elevation));
				pio.SetParamString(kParamStoryName, TXString(level.story.c_str()));
				pio.SetParamString(kParamLevelTypeName, TXString(level.levelType.c_str()));
				pio.SetParamString(kParamDatum, kDatumStoryLevel);
			}
			catch (...)
			{
				// 結べなければ高さを拘束できない。名前の差し替えは続ける。
				++counts.levelBindFailed;
			}
			gSDK->ResetObject(mark);

			std::string shapeProbe;
			if (!RebuildLevelLayout(mark, level, style, markWidth, shapeProbe))
				++counts.levelLayoutFailed;
#if VW_DRAW_VERIFY
			if (counts.levelShapeProbe.empty())
				counts.levelShapeProbe = shapeProbe;
#endif
			return mark;
		}

		// 置いた記号を起点 startX へ動かし、基準線（パス）を図の右端を少し越えるまで伸ばして
		// 描き直す。基準線の終点（注釈空間の x）を返す。
		double PositionLevel(MCObjectHandle mark, const core::LevelMarkCommand& level,
							 double startX, const LevelMarkStyle& style, DimensionCounts& counts)
		{
			const double lengthOnPaper =
				core::levelLineLength(startX, level.right, style.markScale);
			const double length = lengthOnPaper * style.markScale;
#if VW_DRAW_VERIFY
			const bool probePath = !counts.levelPathProbed;
			std::string pathBefore;
			if (probePath)
				pathBefore = DescribeControlPoints(mark);
#endif
			// 縦の位置は絵の置き場所だけで、描く数値には効かない。
			try
			{
				VWParametricObj(mark).SetPointObjectPos(VWPoint2D(startX, level.elevation));
			}
			catch (...)
			{
				++counts.levelLayoutFailed;
			}
			if (!DrawLevelLine(mark, lengthOnPaper))
				++counts.levelPathFailed;
			gSDK->ResetObject(mark);
#if VW_DRAW_VERIFY
			if (probePath)
			{
				counts.levelPathProbed = true;
				counts.levelShapeProbe +=
					" / 引出線を切る前 " + pathBefore + " → 後 " + DescribeControlPoints(mark);
			}
#endif
			// **レイアウトとパスを差し替えた後も結び付きが残っているか**を読み戻す（書いても
			// 入らない値がある。Findings「Level Objects」の作法）。
			try
			{
				const VWParametricObj pio(mark);
				if (std::string(static_cast<const char*>(pio.GetParamString(kParamDatum))) !=
					kDatumStoryLevel)
					++counts.levelBindFailed;
			}
			catch (...)
			{
				++counts.levelBindFailed;
			}
#if VW_DRAW_VERIFY
			if (!DrawsText(mark, level.name, counts.levelNameProbe))
				++counts.levelNameUnseen;
#endif
			return startX + length;
		}
	} // namespace

	void prepareLevelMarkPlugin()
	{
		PrepareCustomObjectDefinition(kLevelMarkPlugin);
	}

	std::size_t drawViewportDimensions(MCObjectHandle viewport,
									   const core::ViewportCommand& command,
									   const std::vector<core::LevelMarkCommand>& levels,
									   const std::string& standard, double scale,
									   DimensionCounts& counts,
									   std::vector<PlacedLevelMark>* placedLevels)
	{
		if (viewport == nil || (command.dimensions.empty() && levels.empty()))
			return 0;
		const double denominator = scale > 0.0 ? scale : kFallbackScale;
		// 寸法規格の文字スタイルと、紙でその pt に見せる図面上の大きさ（ビューポートごとに
		// 決める——規格は文書に 1 つだが、縮尺はビューポートごとに違う）。縮尺は
		// ビューポートの**実際の**値を読む（注釈はそれで描かれる）。読めなければ寸法線の
		// 距離と同じ縮尺で代える。
		// レベル記号の名前も同じ紙の大きさ（用紙 mm）で書く（レイアウトの中身は容れ物の
		// 縮尺を VW が掛けるので、寸法と違って縮尺を掛けない。Findings「Drawing Labels」）。
		DimensionText text;
		LevelMarkStyle markStyle;
		markStyle.dimensionScale = denominator;
		markStyle.markScale = denominator;
		try
		{
			text.style = DimensionStandardTextStyle(standard);
			double viewportScale = denominator;
			if (const double actual = VWViewportObj(viewport).GetScale(); actual > 0.0)
				viewportScale = actual;
			markStyle.markScale = viewportScale;
			if (const double points = TextStylePoints(text.style); points > 0.0)
			{
				markStyle.textSize = core::pointsToMillimeters(points);
				text.fontSize = markStyle.textSize * viewportScale;
			}
		}
		catch (...)
		{
			text = DimensionText{};
		}

		std::size_t drawn = 0;
		bool anyPlaced = false;
		for (const core::DimensionChainCommand& chain : command.dimensions)
		{
			if (PlaceChain(viewport, chain, standard, denominator, text, counts))
			{
				++drawn;
				anyPlaced = true;
			}
		}
		counts.chains += drawn;

		// レベル記号はまず全部置いて幅を測り、**いちばん広い記号に合わせた 1 つの起点**へ
		// 揃える（▽ の左端が縦に揃う。ご要望）。
		struct PendingLevel
		{
			MCObjectHandle mark = nil;
			const core::LevelMarkCommand* level = nullptr;
		};
		std::vector<PendingLevel> pending;
		double widest = 0.0;
		for (const core::LevelMarkCommand& level : levels)
		{
			double width = 0.0;
			if (const MCObjectHandle mark = PlaceLevel(viewport, level, markStyle, width, counts);
				mark != nil)
			{
				pending.push_back({mark, &level});
				widest = std::max(widest, width);
			}
			else
				++counts.levelsFailed;
		}
		for (const PendingLevel& placed : pending)
		{
			const core::LevelMarkCommand& level = *placed.level;
			const double startX =
				core::levelMarkStartX(level.x, level.dimensionTier, widest,
									  markStyle.dimensionScale, markStyle.markScale);
			const double endX = PositionLevel(placed.mark, level, startX, markStyle, counts);
			++counts.levels;
			anyPlaced = true;
			if (placedLevels != nullptr)
				placedLevels->push_back({placed.mark, level.elevation, startX, endX});
		}

		// 注釈へ後から足した図形のクラスは非表示のままなので、全クラスを表示へ戻して描き直す
		// （draw/Dimension.h。データタグと同じ後処理）。
		if (anyPlaced)
		{
			counts.classesShown += ShowAllViewportClasses(viewport, command.hiddenClasses);
			try
			{
				VWViewportObj(viewport).Update();
			}
			catch (...)
			{
				++counts.updateFailed;
			}
		}
		return drawn;
	}

	void finishLevelMarks(MCObjectHandle viewport, const std::vector<PlacedLevelMark>& marks,
						  DimensionCounts& counts)
	{
		if (marks.empty())
			return;
		if (!CopySectionViewMatrix(viewport))
			++counts.viewMatrixFailed;
		for (const PlacedLevelMark& placed : marks)
		{
			// 写しただけでは描き直されない。作り直したときに初めてストーリレベルの高さが入る。
			gSDK->ResetObject(placed.mark);
			CheckLevelHeight(placed.mark, placed.elevation, counts);
#if VW_DRAW_VERIFY
			// 描いた範囲と狙い（起点〜基準線の終点）を**文書で 1 個目だけ**控え、描いた中身を
			// 1 つずつ並べる。round 1: 描いた範囲が狙いより用紙 36mm 広かった（既定の
			// レイアウトに線は無かったので、PIO 自身が別の線を描いていると見ている）。
			if (!counts.levelShapeDrawn)
			{
				counts.levelShapeDrawn = true;
				std::array<char, 96> aim{};
				std::snprintf(aim.data(), aim.size(), " / 狙い x=[%.1f,%.1f]", placed.startX,
							  placed.endX);
				counts.levelShapeProbe += " / 描いた範囲 " + DescribeBounds(placed.mark) +
										  aim.data() + " / 描いた中身" +
										  DescribeMembers(placed.mark);
			}
#endif
		}
	}

	std::string dimensionDiagnostics(const std::string& label, const DimensionCounts& counts)
	{
		const bool placedAny = counts.dimensions > 0 || counts.levels > 0;
		const bool classesBroken = placedAny && counts.classesShown == 0;
#if VW_DRAW_VERIFY
		const bool verifyIssue = counts.levelNameUnseen > 0;
#else
		constexpr bool verifyIssue = false;
#endif
		if (counts.failed == 0 && counts.standardRejected == 0 && counts.unjoined == 0 &&
			counts.textStyleMissing == 0 && counts.textSizeUnread == 0 &&
			counts.levelsFailed == 0 && counts.levelLayoutFailed == 0 &&
			counts.levelPathFailed == 0 && counts.levelBindFailed == 0 &&
			counts.viewMatrixFailed == 0 && counts.levelHeightUnread == 0 &&
			counts.levelHeightMismatch == 0 && counts.updateFailed == 0 && !classesBroken &&
			!verifyIssue)
			return {};

		std::string text = label + "の寸法の診断: ";
		AppendCount(text, "置けなかった寸法", counts.failed, "本");
		AppendCount(text, "寸法規格を当てられなかった寸法", counts.standardRejected, "本",
					"図面にその寸法規格がありません。文書の既定の規格で描きました");
		AppendCount(text, "連続寸法へ繋げなかった継ぎ目", counts.unjoined, "箇所");
		AppendCount(text, "文字スタイルを当てられなかった寸法", counts.textStyleMissing, "本",
					"寸法規格が文字スタイルを持っていません。注釈では寸法値が描かれません");
		AppendCount(
			text, "文字の大きさを縮尺に合わせられなかった寸法", counts.textSizeUnread, "本",
			"文字スタイルの大きさを読めませんでした。寸法値が小さすぎて見えない可能性があります");
		AppendCount(text, "置けなかったレベル記号", counts.levelsFailed, "個");
		AppendCount(text, "記号を組み直せなかったレベル記号", counts.levelLayoutFailed, "個");
		AppendCount(text, "基準線を伸ばせなかったレベル記号", counts.levelPathFailed, "個",
					"基準線が既定の長さのまま左へ出ます");
		AppendCount(text, "ストーリレベルへ結べなかったレベル記号", counts.levelBindFailed, "個",
					"高さが拘束されず、数値が 0 と出ることがあります");
		AppendCount(text, "断面の向きをビュー行列へ写せなかった軸組図", counts.viewMatrixFailed,
					"枚", "レベル記号の高さが 0 と出ます");
		AppendCount(text, "描いた高さを読めなかったレベル記号", counts.levelHeightUnread, "個");
		AppendCount(text, "描いた高さが命令と合わないレベル記号", counts.levelHeightMismatch, "個",
					counts.levelHeightProbe.c_str());
#if VW_DRAW_VERIFY
		AppendCount(text, "描いた文字に名前が見つからないレベル記号（検算）",
					counts.levelNameUnseen, "個", counts.levelNameProbe.c_str());
#endif
		if (classesBroken)
			text += "寸法を置いた後にクラスを表示へ戻せませんでした（寸法が映りません）。";
		AppendCount(text, "描き直せなかったビューポート", counts.updateFailed, "枚");
		return text;
	}

	std::string dimensionInfo(const std::string& label, const DimensionCounts& counts)
	{
		// 中身は今のところ dev の検算だけ。本番では引数を使わないので、tidy の
		// misc-unused-parameters / misc-const-correctness に掛からない形にしておく
		// （PR の CI は dev の分岐しか tidy しない）。
#if VW_DRAW_VERIFY
		std::string text;
		if (!counts.dimensionProbe.empty())
			text += "寸法 1 本目（検算）: " + counts.dimensionProbe + "。";
		if (!counts.levelShapeProbe.empty())
			text += "レベル記号 1 個目の形（検算）: " + counts.levelShapeProbe + "。";
		if (!text.empty())
			return label + "の寸法の記録: " + text;
#else
		(void)label;
		(void)counts;
#endif
		return {};
	}
} // namespace HomeskzIfcImport::draw
