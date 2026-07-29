//
//	parse/Step.h
//
//	最小 STEP リーダ（ISO 10303-21）。ホームズ君 IFC が出力する既知サブセット向けに、
//	STEP トークナイザ＋エンティティグラフを提供する。Python 版が ifcopenshell を
//	「エンティティグラフの読み取り」だけに使っているのと同じ立ち位置で、幾何エンジン
//	（OpenCASCADE 等）は持たない。配置行列・断面・押し出しの幾何計算は parse/IfcGeometry
//	＋ core/Geometry へ別途移植する（ROADMAP.md M2）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。通常の C++
//	ツールチェインだけでコンパイル・単体実行・テストできる（CLAUDE.md「Phase 1」）。
//
//	提供する読み取り API（Python 版 ifcopenshell の open / by_type / 逆参照に対応）:
//	  * Model::entity(id)     … #id のエンティティ（無ければ nullptr）
//	  * Model::byType(name)   … 型名（大文字）に属する #id 群（id 昇順で決定的）
//	  * Model::referrers(id)  … #id を属性のどこかで参照するエンティティの #id 群
//	  * Value                 … 1 属性の値（参照 / 数値 / 文字列 / 列挙 / リスト / 型付き）
//
//	寛容さ（CLAUDE.md「エラーハンドリング」）: 解釈できないトークンは読み飛ばし、
//	1 エンティティの欠損で全体を止めない。数値変換の失敗も 0 として握りつぶす。
//

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// STEP 属性値の種別。Python 版が ifcopenshell から受け取る値の型に対応する。
	enum class ValueType
	{
		Null,	   // $      … 未設定（省略）
		Derived,   // *      … 派生値（上位型で規定）
		Reference, // #N     … 別エンティティへの参照
		Integer,   // 123    … 整数
		Real,	   // 1.5    … 実数（STEP の実数は必ず '.' を含む）
		String,	   // 'text' … 文字列（'' は ' のエスケープ）
		Enum,	   // .TRUE. … 列挙・論理値
		List,	   // (...)  … 集約（入れ子可）
		Typed,	   // IFCLABEL('x') … 型名付きの単純値（SELECT/明示型）
	};

	// STEP の 1 属性値。プレーンな集約で表す（種別ごとに使うフィールドが決まる）。
	struct Value
	{
		ValueType type = ValueType::Null;

		int reference = 0;		  // Reference: 参照先 #id
		long long integer = 0;	  // Integer: 整数値
		double real = 0.0;		  // Real: 実数値
		std::string text;		  // String: 内容 / Enum: 記号（TRUE 等）/ Typed: 型名
		std::vector<Value> items; // List: 要素 / Typed: 引数（通常 1 個）

		bool isNull() const
		{
			return type == ValueType::Null;
		}
		bool isReference() const
		{
			return type == ValueType::Reference;
		}
		bool isNumber() const
		{
			return type == ValueType::Integer || type == ValueType::Real;
		}
		bool isList() const
		{
			return type == ValueType::List;
		}

		// 数値としての読み出し（Integer/Real を double に正規化。非数値は 0）。
		double asReal() const;
	};

	// STEP のエンティティインスタンス（#id = TYPE(属性...);）。
	struct Entity
	{
		int id = 0;					   // #id
		std::string type;			   // 型名（常に大文字。例: IFCPOLYLINE）
		std::vector<Value> attributes; // 属性リスト（宣言順）

		// index 番目の属性を返す。範囲外なら Null 値を返す（寛容アクセス）。
		const Value& attribute(std::size_t index) const;
	};

	// パース済み STEP モデル。エンティティ本体と、型別・逆参照のインデックスを持つ。
	// 列挙順に依存しない決定的な結果を出すため、#id 昇順で一貫して返す。
	class Model
	{
	public:
		// #id のエンティティ（無ければ nullptr）。
		const Entity* entity(int id) const;

		// 参照値が指すエンティティ（参照でない・未解決なら nullptr）。
		const Entity* resolve(const Value& value) const;

		// 型名（大文字）に属する #id 群を id 昇順で返す。未知の型は空を返す。
		const std::vector<int>& byType(const std::string& type) const;

		// #id を属性のどこか（入れ子リスト・型付き値の内部を含む）で参照している
		// エンティティの #id 群を id 昇順で返す。未参照なら空を返す。
		const std::vector<int>& referrers(int id) const;

		// 総エンティティ数。
		std::size_t size() const
		{
			return fEntities.size();
		}

	private:
		// 構築は parseStep（friend）に限る。読み取り側の公開 API は上のアクセサだけ。
		friend Model parseStep(const std::string& text);

		// エンティティを 1 つ格納する（同一 #id は最初の宣言を採用＝決定的）。
		// 型別・逆参照インデックスはここでは更新せず、buildIndices でまとめて作る。
		void addEntity(Entity entity);

		// 格納済みエンティティから型別・逆参照インデックスを構築する。fEntities を
		// #id 昇順（std::map の反復順）で走るので、byType / referrers は昇順になり、
		// エンティティの出現順に依存しない決定的な結果になる。
		void buildIndices();

		// #id → エンティティ。std::map で id 昇順の決定的な反復を保証する。
		std::map<int, Entity> fEntities;
		// 型名 → その型の #id 群（昇順）。
		std::unordered_map<std::string, std::vector<int>> fByType;
		// 被参照 #id → 参照元 #id 群（昇順・重複なし）。
		std::unordered_map<int, std::vector<int>> fReferrers;
	};

	// STEP テキスト全体を解析して Model を返す。DATA セクションのインスタンス
	// （#id=TYPE(...);）を拾う。ヘッダや解釈できない断片は読み飛ばす（寛容）。
	Model parseStep(const std::string& text);

	// STEP 文字列（'' のエスケープを解いた生の中身）の ISO 10303-21 拡張文字
	// エスケープを UTF-8 へデコードする。パーサが文字列値を読むたびに通すので
	// Value::text は常に UTF-8 になる。ホームズ君 IFC の日本語（"床版" 等の Name）は
	// \X2\5E8A\X0\ 形式（UTF-16 コード単位列）で出力されるため、これを解かないと
	// 名前による要素判別が一切通らない（ifcopenshell と同じ扱いに揃える）。
	//   \X2\<hex…>\X0\ … UTF-16 コード単位列（サロゲートペア対応）
	//   \X\HH           … 1 バイト（ISO 8859-1 のコードポイント）
	//   \S\c            … c のコードポイント + 128
	//   \P?\            … コードページ指示（読み飛ばす）
	// 壊れたエスケープや上記以外のバックスラッシュはそのまま残す（1 文字列の異常で
	// 内容全体を失わない。CLAUDE.md「エラーハンドリング」）。
	std::string decodeStepString(const std::string& raw);
} // namespace HomeskzIfcImport::parse
