//
//	draw/DocumentFile.cpp
//
//	図面ファイルの小道具（意図と但し書きは draw/DocumentFile.h）。**中身は M27 まで
//	draw/Feedback.cpp の無名名前空間に在ったものをそのまま出しただけ**で、手順は 1 行も
//	変えていない（M28 で分けたのは「誰が使うか」だけ——実機フィードバックの周と回帰テストの
//	両方が同じ作法で図面を戻す）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/DocumentFile.h"

// 図面を開き直す（ISDK::OpenDocumentPath）。パスは IFileIdentifier で渡す。
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 絶対パスから IFileIdentifier を作る（作れなければ空の VCOMPtr）。
		VectorWorks::Filing::IFileIdentifierPtr FileIdFor(const std::string& path)
		{
			using namespace VectorWorks::Filing;
			// const で受ける（VCOMPtr の operator-> は const。draw/ImportRun.cpp と同じ
			// 作法で、clang-tidy の misc-const-correctness もこれを求める）。
			const IFileIdentifierPtr fileID(IID_FileIdentifier);
			if (!fileID)
				return IFileIdentifierPtr{};
			if (fileID->Set(TXString(path.c_str())) != kVCOMError_NoError)
				return IFileIdentifierPtr{};
			return fileID;
		}

		// 一時ディレクトリの中のパスを組む（temp が引けなければ空）。
		std::string TempPath(const std::string& name)
		{
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
			if (ec)
				return "";
			return (dir / name).string();
		}

		// そのパスに何か在るか。**`std::filesystem` で見に行かない**——`file_size` の
		// stat が MSVC の clang-analyzer に偽陽性を出した前例があるので、開けるかどうかで
		// 判じる（在るのに開けないなら、どのみち上書きも当てにできない）。
		bool PathExists(const std::string& path)
		{
			if (path.empty())
				return false;
			const std::ifstream in(path, std::ios::binary);
			return in.good();
		}
	} // namespace

	// -----------------------------------------------------------------------
	std::string ActiveDocumentPath()
	{
		VectorWorks::Filing::IFileIdentifierPtr fileID;
		bool saved = false;
		if (!gSDK->GetActiveDocument(&fileID, saved) || !fileID)
			return "";
		TXString path;
		if (fileID->GetFileFullPath(path) != kVCOMError_NoError)
			return "";
		return static_cast<const char*>(path);
	}

	bool SamePath(const std::string& left, const std::string& right)
	{
		if (left.empty() || right.empty())
			return false;
		if (left == right)
			return true;
		std::error_code ec;
		return std::filesystem::equivalent(std::filesystem::path(left),
										   std::filesystem::path(right), ec) &&
			   !ec;
	}

	std::string FreshTempPath(const std::string& stem)
	{
		for (int i = 1; i <= 100; ++i)
		{
			// **const にしない**——返すときに move されなくなり、clang-tidy の
			// performance-no-automatic-move がエラーになる（tidy-mac で実際に落ちた）。
			std::string path = TempPath(stem + "-" + std::to_string(i) + ".vwx");
			if (path.empty())
				return "";
			if (!PathExists(path))
				return path;
		}
		return "";
	}

	bool SaveActiveDocumentAs(const std::string& path)
	{
		if (path.empty())
			return false;
		const VectorWorks::Filing::IFileIdentifierPtr fileID = FileIdFor(path);
		return fileID && gSDK->SaveActiveDocumentPath(fileID) == 0;
	}

	bool OpenDocumentAt(const std::string& path)
	{
		if (path.empty())
			return false;
		const VectorWorks::Filing::IFileIdentifierPtr fileID = FileIdFor(path);
		// bShowErrorMessages=false: 誰も見ていない周でダイアログを出さない。
		return fileID && gSDK->OpenDocumentPath(fileID, false);
	}
} // namespace HomeskzIfcImport::draw
