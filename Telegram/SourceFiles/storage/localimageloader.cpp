/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/localimageloader.h"

#include "api/api_text_entities.h"
#include "api/api_sending.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include <QProcess>
#include "core/mime_type.h"
#include "base/unixtime.h"
#include "base/random.h"
#include "editor/scene/scene_item_sticker.h"
#include "editor/scene/scene.h"
#include "editor/video/video_editor_common.h"
#include "media/audio/media_audio.h"
#include "media/clip/media_clip_reader.h"
#include "media/media_video_encode.h"
#include "mtproto/facade.h"
#include "lottie/lottie_animation.h"
#include "history/history.h"
#include "history/history_item.h"
#include "boxes/abstract_box.h"
#include "boxes/send_files_box.h"
#include "boxes/premium_limits_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image_prepare.h"
#include "ui/image/image.h"
#include "lang/lang_keys.h"
#include "storage/file_download.h"
#include "storage/storage_media_prepare.h"
#include "window/themes/window_theme_preview.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "main/main_session.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStringConverter>
#include <QtCore/QUuid>
#include <QtCore/QXmlStreamReader>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QImageWriter>

#include "base/zlib_help.h"

#include <cstdlib>

namespace {

constexpr auto kThumbnailQuality = 100;
constexpr auto kThumbnailSize = 320;
constexpr auto kPhotoUploadPartSize = 32 * 1024;

using Ui::ValidateThumbDimensions;

struct PreparedFileThumbnail {
	uint64 id = 0;
	QString name;
	QImage image;
	QByteArray bytes;
	MTPPhotoSize mtpSize = MTP_photoSizeEmpty(MTP_string());
};

[[nodiscard]] PreparedFileThumbnail PrepareFileThumbnail(QImage &&original) {
	const auto width = original.width();
	const auto height = original.height();
	if (!ValidateThumbDimensions(width, height)) {
		return {};
	}
	auto result = PreparedFileThumbnail();
	result.id = base::RandomValue<uint64>();
	const auto scaled = (width > kThumbnailSize || height > kThumbnailSize);
	const auto scaledWidth = [&] {
		return (width > height)
			? kThumbnailSize
			: int(base::SafeRound(kThumbnailSize * width / float64(height)));
	};
	const auto scaledHeight = [&] {
		return (width > height)
			? int(base::SafeRound(kThumbnailSize * height / float64(width)))
			: kThumbnailSize;
	};
	result.image = scaled
		? original.scaled(
			scaledWidth(),
			scaledHeight(),
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation)
		: std::move(original);
	result.mtpSize = MTP_photoSize(
		MTP_string(),
		MTP_int(result.image.width()),
		MTP_int(result.image.height()),
		MTP_int(0));
	return result;
}

[[nodiscard]] bool FileThumbnailUploadRequired(
		const QString &filemime,
		int64 filesize) {
	constexpr auto kThumbnailUploadBySize = 5 * int64(1024 * 1024);
	const auto kThumbnailKnownMimes = {
		"image/jpeg",
		"image/gif",
		"image/png",
		"image/webp",
		"video/mp4",
	};
	return (filesize > kThumbnailUploadBySize)
		|| (ranges::find(kThumbnailKnownMimes, filemime.toLower())
			== end(kThumbnailKnownMimes));
}

[[nodiscard]] QString Mp4FileName(const QString &name) {
	const auto dot = name.lastIndexOf('.');
	return ((dot > 0) ? name.mid(0, dot) : name) + u".mp4"_q;
}

[[nodiscard]] PreparedFileThumbnail FinalizeFileThumbnail(
		PreparedFileThumbnail &&prepared,
		const QString &filemime,
		int64 filesize,
		bool isSticker) {
	prepared.name = isSticker ? u"thumb.webp"_q : u"thumb.jpg"_q;
	if (FileThumbnailUploadRequired(filemime, filesize)) {
		const auto format = isSticker ? "WEBP" : "JPG";
		auto buffer = QBuffer(&prepared.bytes);
		prepared.image.save(&buffer, format, kThumbnailQuality);
	}
	return std::move(prepared);
}

[[nodiscard]] auto FindAlbumItem(
		std::vector<SendingAlbum::Item> &items,
		not_null<HistoryItem*> item) {
	const auto result = ranges::find(
		items,
		item->fullId(),
		&SendingAlbum::Item::msgId);

	Ensures(result != end(items));
	return result;
}

[[nodiscard]] MTPInputSingleMedia PrepareAlbumItemMedia(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		uint64 randomId) {
	auto caption = item->originalText();
	TextUtilities::Trim(caption);
	auto sentEntities = Api::EntitiesToMTP(
		&item->history()->session(),
		caption.entities,
		Api::ConvertOption::SkipLocal);
	const auto flags = !sentEntities.v.isEmpty()
		? MTPDinputSingleMedia::Flag::f_entities
		: MTPDinputSingleMedia::Flag(0);

	return MTP_inputSingleMedia(
		MTP_flags(flags),
		media,
		MTP_long(randomId),
		MTP_string(caption.text),
		sentEntities);
}

[[nodiscard]] std::vector<not_null<DocumentData*>> ExtractStickersFromScene(
		not_null<const Ui::PreparedFileInformation::Image*> info) {
	const auto allItems = info->modifications.paint->items();

	return ranges::views::all(
		allItems
	) | ranges::views::filter([](const Editor::Scene::ItemPtr &i) {
		return i->isVisible() && (i->type() == Editor::ItemSticker::Type);
	}) | ranges::views::transform([](const Editor::Scene::ItemPtr &i) {
		return static_cast<Editor::ItemSticker*>(i.get())->sticker();
	}) | ranges::to_vector;
}

constexpr auto kSevenZipListTimeout = 1000;
constexpr auto kSevenZipExtractTimeout = 3000;
constexpr auto kSevenZipPollStep = 250;
constexpr auto kSevenZipListCap = 1024 * 1024;
constexpr auto kSevenZipEntryCap = 10 * 1024 * 1024;
constexpr auto kSevenZipXmlCap = 256 * 1024;
constexpr auto kSevenZipCoverTries = 5;

struct SevenZipEntry {
	QString name;
	uint64 size = 0;
};

[[nodiscard]] QByteArray RunHelperCapped(
		const QString &program,
		const QStringList &args,
		int timeout,
		int cap) {
	auto process = QProcess();
	process.start(program, args);
	auto output = QByteArray();
	for (auto waited = 0; waited < timeout; waited += kSevenZipPollStep) {
		if (process.waitForFinished(kSevenZipPollStep)) {
			break;
		}
		output += process.readAllStandardOutput();
		process.readAllStandardError();
		if (output.size() > cap) {
			break;
		}
	}
	if (process.state() != QProcess::NotRunning) {
		process.kill();
		process.waitForFinished(kSevenZipPollStep);
	}
	output += process.readAllStandardOutput();
	process.readAllStandardError();
	if (process.exitCode() != 0 || output.size() > cap) {
		return {};
	}
	return output;
}

[[nodiscard]] std::vector<SevenZipEntry> SevenZipListEntries(
		const QString &sevenZip,
		const QString &archive) {
	const auto output = RunHelperCapped(
		sevenZip,
		{ u"l"_q, u"-slt"_q, u"-bd"_q, u"-y"_q, u"-sccUTF-8"_q, archive },
		kSevenZipListTimeout,
		kSevenZipListCap);
	auto result = std::vector<SevenZipEntry>();
	if (output.isEmpty()) {
		auto probe = QProcess();
		probe.start(sevenZip, {
			u"l"_q, u"-bd"_q, u"-y"_q, archive });
		probe.waitForFinished(kSevenZipListTimeout);
		const auto details = QString::fromUtf8(
			probe.readAllStandardError()
			+ probe.readAllStandardOutput()).simplified().left(200);
		LOG(("SevenZipListEntries: '%1' lister produced no output%2.").arg(
			QFileInfo(archive).fileName()).arg(details.isEmpty()
				? QString()
				: u" (%1)"_q.arg(details)));
		return result;
	}
	auto pendingName = QString();
	auto pendingIsFile = false;
	auto pendingHasFolder = false;
	auto blocks = 0;
	for (const auto &line : QString::fromUtf8(output).split(u'\n')) {
		if (line.startsWith(u"Path = "_q)) {
			pendingName = line.mid(7).trimmed();
			pendingIsFile = false;
			pendingHasFolder = false;
			blocks++;
		} else if (line.startsWith(u"Folder = "_q)) {
			pendingHasFolder = true;
			pendingIsFile = (line.mid(9).trimmed() == u"-"_q);
		} else if (line.startsWith(u"Size = "_q)
			&& !pendingName.isEmpty()
			&& blocks > 1
			&& (!pendingHasFolder || pendingIsFile)) {
			pendingIsFile = false;
			pendingHasFolder = false;
			if (const auto size = line.mid(7).trimmed().toULongLong();
				size <= uint64(kSevenZipEntryCap)) {
				result.push_back({ pendingName, size });
			}
			pendingName = QString();
		}
	}
	if (result.empty() && !output.isEmpty()) {
		LOG(("SevenZipListEntries: '%1' unparsed output (%2 bytes): %3.").arg(
			QFileInfo(archive).fileName()).arg(output.size()).arg(
			QString::fromUtf8(output).simplified().left(300)));
		return result;
	}
	return result;
}

[[nodiscard]] QByteArray SevenZipExtractEntry(
		const QString &sevenZip,
		const QString &archive,
		const QString &entry,
		int cap) {
	return RunHelperCapped(
		sevenZip,
		{ u"x"_q, u"-so"_q, u"-bd"_q, u"-y"_q, u"-sccUTF-8"_q, archive, entry },
		kSevenZipExtractTimeout,
		cap);
}

[[nodiscard]] bool IsSevenZipImageName(const QString &name) {
	const auto lower = name.toLower();
	for (const auto &suffix : {
		u".jpg"_q,
		u".jpeg"_q,
		u".png"_q,
		u".gif"_q,
		u".bmp"_q,
		u".webp"_q,
		u".tiff"_q,
		u".tif"_q,
	}) {
		if (lower.endsWith(suffix)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool IsCoverStemName(const QString &name) {
	const auto lower = name.toLower();
	const auto base = lower.contains(u'/')
		? lower.mid(lower.lastIndexOf(u'/') + 1)
		: lower;
	const auto dot = base.lastIndexOf(u'.');
	const auto stem = (dot >= 0) ? base.left(dot) : base;
	return (stem == u"cover"
		|| stem == u"folder"
		|| stem == u"thumb"
		|| stem == u"thumbnail"
		|| stem == u"front");
}

[[nodiscard]] bool SevenZipNaturalLess(const QString &a, const QString &b) {
	auto i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (a[i].isDigit() && b[j].isDigit()) {
			auto ai = i;
			while (ai < a.size() && a[ai].isDigit()) ++ai;
			auto bj = j;
			while (bj < b.size() && b[bj].isDigit()) ++bj;
			const auto aNum = a.mid(i, ai - i).toULongLong();
			const auto bNum = b.mid(j, bj - j).toULongLong();
			if (aNum != bNum) return aNum < bNum;
			i = ai;
			j = bj;
		} else {
			if (a[i].toLower() != b[j].toLower())
				return a[i].toLower() < b[j].toLower();
			++i;
			++j;
		}
	}
	return a.size() < b.size();
}

[[nodiscard]] QImage SevenZipDecodeImages(
		const QString &sevenZip,
		const QString &archive,
		std::vector<SevenZipEntry> entries) {
	std::sort(entries.begin(), entries.end(), [](const SevenZipEntry &a, const SevenZipEntry &b) {
		const auto aCover = IsCoverStemName(a.name);
		const auto bCover = IsCoverStemName(b.name);
		if (aCover != bCover) {
			return aCover;
		}
		return SevenZipNaturalLess(a.name, b.name);
	});
	auto tried = 0;
	for (const auto &entry : entries) {
		if (!IsSevenZipImageName(entry.name)) {
			continue;
		}
		if (++tried > kSevenZipCoverTries) {
			break;
		}
		if (auto image = QImage::fromData(SevenZipExtractEntry(
			sevenZip,
			archive,
			entry.name,
			kSevenZipEntryCap));
			!image.isNull() && !Images::IsBlank(image)) {
			return image;
		}
	}
	return {};
}

[[nodiscard]] QImage SevenZipDirectoryImages(
		const QString &sevenZip,
		const QString &archive,
		const std::vector<SevenZipEntry> &entries,
		const QString &prefix) {
	auto filtered = std::vector<SevenZipEntry>();
	for (const auto &entry : entries) {
		if (entry.name.startsWith(prefix, Qt::CaseInsensitive)
			&& IsSevenZipImageName(entry.name)) {
			filtered.push_back(entry);
		}
	}
	return SevenZipDecodeImages(sevenZip, archive, std::move(filtered));
}

[[nodiscard]] QImage SevenZipOfficeCover(
		const QString &sevenZip,
		const QString &archive,
		const QString &mediaPrefix,
		const QString &thumbnailEntry) {
	const auto entries = SevenZipListEntries(sevenZip, archive);
	if (entries.empty()) {
		LOG(("Document cover: no archive entries in '%1'.").arg(
			QFileInfo(archive).fileName()));
		return {};
	}
	if (!thumbnailEntry.isEmpty()) {
		for (const auto &entry : entries) {
			if (entry.name == thumbnailEntry) {
				if (auto image = QImage::fromData(SevenZipExtractEntry(
					sevenZip,
					archive,
					entry.name,
					kSevenZipEntryCap));
					!image.isNull()) {
					return image;
				}
				break;
			}
		}
	}
	return SevenZipDirectoryImages(sevenZip, archive, entries, mediaPrefix);
}

[[nodiscard]] QImage SevenZipEpubCover(
		const QString &sevenZip,
		const QString &archive) {
	const auto container = SevenZipExtractEntry(
		sevenZip,
		archive,
		u"META-INF/container.xml"_q,
		kSevenZipXmlCap);
	if (!container.isEmpty()) {
		auto opfPath = QString();
		auto reader = QXmlStreamReader(container);
		while (!reader.atEnd() && !reader.hasError()) {
			if (reader.readNext() == QXmlStreamReader::StartElement
				&& reader.name().toString().compare(
					u"rootfile"_q,
					Qt::CaseInsensitive) == 0) {
				opfPath = reader.attributes().value(
					u"full-path"_q).toString();
				break;
			}
		}
		if (!opfPath.isEmpty()) {
			const auto opf = SevenZipExtractEntry(
				sevenZip,
				archive,
				opfPath,
				kSevenZipXmlCap);
			if (!opf.isEmpty()) {
				auto epub3Href = QString();
				auto epub2Id = QString();
				auto items = QMap<QString, QString>();
				auto opfReader = QXmlStreamReader(opf);
				while (!opfReader.atEnd() && !opfReader.hasError()) {
					opfReader.readNext();
					if (opfReader.isStartElement()) {
						const auto tag = opfReader.name().toString().toLower();
						if (tag == u"item") {
							const auto id = opfReader.attributes().value(
								u"id"_q).toString();
							const auto href = opfReader.attributes().value(
								u"href"_q).toString();
							const auto props = opfReader.attributes().value(
								u"properties"_q).toString();
							if (!id.isEmpty() && !href.isEmpty()) {
								items[id] = href;
							}
							if (!props.isEmpty()
								&& props.split(u' ').contains(
									u"cover-image"_q)) {
								epub3Href = href;
							}
						} else if (tag == u"meta") {
							const auto metaName = opfReader.attributes().value(
								u"name"_q).toString();
							const auto content = opfReader.attributes().value(
								u"content"_q).toString();
							if (metaName.compare(
								u"cover"_q,
								Qt::CaseInsensitive) == 0) {
								epub2Id = content;
							}
						}
					}
				}
				const auto coverHref = !epub3Href.isEmpty()
					? epub3Href
					: (!epub2Id.isEmpty() && items.contains(epub2Id)
						? items[epub2Id]
						: QString());
				if (!coverHref.isEmpty()) {
					const auto opfDir = opfPath.contains(u'/')
						? opfPath.left(opfPath.lastIndexOf(u'/') + 1)
						: QString();
					auto resolved = opfDir + coverHref;
					if (resolved.startsWith(u'/')) {
						resolved = resolved.mid(1);
					}
					auto parts = QStringList();
					for (const auto &part : resolved.split(u'/')) {
						if (part == u"..") {
							if (!parts.isEmpty()) {
								parts.removeLast();
							}
						} else if (!part.isEmpty() && part != u".") {
							parts.push_back(part);
						}
					}
					const auto coverPath = parts.join(u'/');
					if (auto image = QImage::fromData(SevenZipExtractEntry(
						sevenZip,
						archive,
						coverPath,
						kSevenZipEntryCap));
						!image.isNull()) {
						return image;
					}
				}
			}
		}
	}
	return SevenZipDecodeImages(
		sevenZip,
		archive,
		SevenZipListEntries(sevenZip, archive));
}

[[nodiscard]] QImage Fb2CoverFromBytes(const QByteArray &fb2) {
	if (fb2.isEmpty()) {
		return {};
	}
	const auto hrefOf = [](QXmlStreamReader &reader) {
		for (const auto &key : { u"href"_q, u"l:href"_q, u"xlink:href"_q }) {
			const auto value = reader.attributes().value(key).toString();
			if (!value.isEmpty()) {
				return value;
			}
		}
		return QString();
	};
	auto coverId = QString();
	{
		auto reader = QXmlStreamReader(fb2);
		auto inCover = false;
		while (!reader.atEnd() && !reader.hasError()) {
			const auto token = reader.readNext();
			if (token != QXmlStreamReader::StartElement
				&& token != QXmlStreamReader::EndElement) {
				continue;
			}
			const auto tag = reader.name().toString().toLower();
			if (tag == u"coverpage") {
				inCover = (token == QXmlStreamReader::StartElement);
			} else if (inCover
				&& token == QXmlStreamReader::StartElement
				&& tag == u"image") {
				coverId = hrefOf(reader);
				break;
			}
		}
	}
	if (coverId.isEmpty()) {
		return {};
	}
	if (coverId.startsWith(u'#')) {
		coverId = coverId.mid(1);
	}
	auto reader = QXmlStreamReader(fb2);
	while (!reader.atEnd() && !reader.hasError()) {
		if (reader.readNext() != QXmlStreamReader::StartElement
			|| reader.name().toString().compare(u"binary"_q, Qt::CaseInsensitive) != 0) {
			continue;
		}
		if (reader.attributes().value(u"id"_q).toString() != coverId) {
			continue;
		}
		const auto mime = reader.attributes().value(
			u"content-type"_q).toString().toLower();
		if (!mime.startsWith(u"image/"_q)) {
			return {};
		}
		auto text = reader.readElementText().simplified();
		text.remove(u' ');
		return QImage::fromData(QByteArray::fromBase64(text.toLatin1()));
	}
	return {};
}

[[nodiscard]] QImage SevenZipFbzCover(
		const QString &sevenZip,
		const QString &archive) {
	const auto entries = SevenZipListEntries(sevenZip, archive);
	auto books = std::vector<SevenZipEntry>();
	for (const auto &entry : entries) {
		if (entry.name.endsWith(u".fb2"_q, Qt::CaseInsensitive)) {
			books.push_back(entry);
		}
	}
	if (books.empty()) {
		return {};
	}
	std::sort(books.begin(), books.end(), [](const SevenZipEntry &a, const SevenZipEntry &b) {
		return SevenZipNaturalLess(a.name, b.name);
	});
	auto tried = 0;
	for (const auto &book : books) {
		if (++tried > 2) {
			break;
		}
		const auto fb2 = SevenZipExtractEntry(
			sevenZip,
			archive,
			book.name,
			kSevenZipEntryCap);
		if (auto image = Fb2CoverFromBytes(fb2); !image.isNull()) {
			return image;
		}
	}
	return {};
}

[[nodiscard]] QString HtmlTextSnippet(const QByteArray &html) {
	auto text = QString::fromUtf8(html);
	for (const auto &tag : { u"script"_q, u"style"_q }) {
		auto from = 0;
		while (true) {
			const auto open = text.indexOf(u'<' + tag, from, Qt::CaseInsensitive);
			if (open < 0) {
				break;
			}
			const auto close = text.indexOf(u"</"_q + tag, open, Qt::CaseInsensitive);
			if (close < 0) {
				text.truncate(open);
				break;
			}
			const auto end = text.indexOf(u'>', close);
			text.remove(open, (end < 0 ? text.size() : end + 1) - open);
			from = open;
		}
	}
	auto result = QString();
	auto inTag = false;
	for (const auto &ch : text) {
		if (ch == u'<') {
			inTag = true;
		} else if (ch == u'>') {
			inTag = false;
			result.push_back(u' ');
		} else if (!inTag) {
			result.push_back(ch);
		}
	}
	result.replace(u"&amp;"_q, u"&"_q);
	result.replace(u"&lt;"_q, u"<"_q);
	result.replace(u"&gt;"_q, u">"_q);
	result.replace(u"&quot;"_q, u"\""_q);
	result.replace(u"&apos;"_q, u"'"_q);
	result.replace(u"&nbsp;"_q, u" "_q);
	return result.simplified().left(500).trimmed();
}

[[nodiscard]] QImage DocumentTextTile(
	const QString &title,
	const QString &snippet);

[[nodiscard]] QImage TryChmCover(
		const QString &sevenZip,
		const QString &archive) {
	auto entries = SevenZipListEntries(sevenZip, archive);
	if (entries.empty()) {
		LOG(("Document cover: no archive entries in '%1'.").arg(
			QFileInfo(archive).fileName()));
		return {};
	}
	auto topics = std::vector<QString>();
	auto pictures = std::vector<SevenZipEntry>();
	for (const auto &entry : entries) {
		const auto lower = entry.name.toLower();
		if (lower.endsWith(u".htm"_q) || lower.endsWith(u".html"_q)) {
			topics.push_back(entry.name);
		}
		if (IsSevenZipImageName(entry.name)) {
			pictures.push_back(entry);
		}
	}
	std::sort(topics.begin(), topics.end(), SevenZipNaturalLess);
	std::sort(pictures.begin(), pictures.end(), [](const SevenZipEntry &a, const SevenZipEntry &b) {
		return a.size > b.size;
	});
	auto tried = 0;
	for (const auto &picture : pictures) {
		if (++tried > kSevenZipCoverTries) {
			break;
		}
		if (auto image = QImage::fromData(SevenZipExtractEntry(
			sevenZip,
			archive,
			picture.name,
			kSevenZipEntryCap));
			!image.isNull()
			&& image.width() >= 100
			&& image.height() >= 100
			&& !Images::IsBlank(image)) {
			return image;
		}
	}
	tried = 0;
	for (const auto &topic : topics) {
		if (++tried > 5) {
			break;
		}
		const auto snippet = HtmlTextSnippet(SevenZipExtractEntry(
			sevenZip,
			archive,
			topic,
			kSevenZipXmlCap));
		if (!snippet.isEmpty()) {
			return DocumentTextTile(QFileInfo(archive).fileName(), snippet);
		}
	}
	LOG(("Document cover: no text pages in '%1'.").arg(
		QFileInfo(archive).fileName()));
	return {};
}

[[nodiscard]] QImage TrySevenZipDocumentCover(const QString &filepath) {
	const auto sevenZip = Core::HelperBinaryPath(u"7z"_q);
	if (sevenZip.isEmpty()) {
		return {};
	}
	const auto lower = filepath.toLower();
	if (lower.endsWith(u".docx"_q)) {
		return SevenZipOfficeCover(
			sevenZip,
			filepath,
			u"word/media/"_q,
			u"docProps/thumbnail.jpeg"_q);
	} else if (lower.endsWith(u".xlsx"_q)) {
		return SevenZipOfficeCover(
			sevenZip,
			filepath,
			u"xl/media/"_q,
			QString());
	} else if (lower.endsWith(u".pptx"_q)) {
		return SevenZipOfficeCover(
			sevenZip,
			filepath,
			u"ppt/media/"_q,
			u"ppt/thumbnail.jpeg"_q);
	} else if (lower.endsWith(u".odt"_q)
		|| lower.endsWith(u".ods"_q)
		|| lower.endsWith(u".odp"_q)) {
		auto entries = SevenZipListEntries(sevenZip, filepath);
		for (const auto &entry : entries) {
			if (entry.name == u"Thumbnails/thumbnail.png"_q) {
				if (auto image = QImage::fromData(SevenZipExtractEntry(
					sevenZip,
					filepath,
					entry.name,
					kSevenZipEntryCap));
					!image.isNull()) {
					return image;
				}
				break;
			}
		}
		return SevenZipDecodeImages(sevenZip, filepath, std::move(entries));
	} else if (lower.endsWith(u".epub"_q)) {
		return SevenZipEpubCover(sevenZip, filepath);
	} else if (lower.endsWith(u".fbz"_q)
		|| (lower.endsWith(u".zip"_q) && lower.contains(u".fb2."_q))) {
		return SevenZipFbzCover(sevenZip, filepath);
	} else if (lower.endsWith(u".chm"_q)) {
		return TryChmCover(sevenZip, filepath);
	} else if (lower.endsWith(u".cbz"_q)
		|| lower.endsWith(u".cbr"_q)
		|| lower.endsWith(u".cb7"_q)
		|| lower.endsWith(u".cbt"_q)
		|| lower.endsWith(u".rar"_q)) {
		return SevenZipDecodeImages(
			sevenZip,
			filepath,
			SevenZipListEntries(sevenZip, filepath));
	}
	return {};
}

constexpr auto kPageCoverPages = 5;
constexpr auto kPageRenderWidth = 640;

[[nodiscard]] QByteArray TempFileRenderBytes(
		const QString &program,
		const QString &tool,
		const QString &filepath,
		int page,
		bool quiet) {
	auto argsFor = [&](const QString &output) {
		auto args = QStringList();
		if (!tool.isEmpty()) {
			args.push_back(tool);
		}
		args.push_back(u"-F"_q);
		args.push_back(u"png"_q);
		args.push_back(u"-r"_q);
		args.push_back(u"300"_q);
		args.push_back(u"-w"_q);
		args.push_back(QString::number(kPageRenderWidth));
		if (quiet) {
			args.push_back(u"-q"_q);
		}
		args.push_back(u"-o"_q);
		args.push_back(output);
		args.push_back(filepath);
		args.push_back(QString::number(page));
		return args;
	};
	if (const auto piped = RunHelperCapped(
		program,
		argsFor(u"-"_q),
		kSevenZipExtractTimeout,
		kSevenZipEntryCap);
		!piped.isEmpty()) {
		return piped;
	}
	const auto tempPath = QDir::tempPath()
		+ u"/tdesktop_thumb_"_q
		+ QUuid::createUuid().toString(QUuid::WithoutBraces)
		+ u".png"_q;
	auto process = QProcess();
	process.start(program, argsFor(tempPath));
	auto errors = QByteArray();
	for (auto waited = 0; waited < kSevenZipExtractTimeout; waited += kSevenZipPollStep) {
		if (process.waitForFinished(kSevenZipPollStep)) {
			break;
		}
		process.readAllStandardOutput();
		if (errors.size() < 512) {
			errors += process.readAllStandardError();
		}
	}
	if (process.state() != QProcess::NotRunning) {
		process.kill();
		process.waitForFinished(kSevenZipPollStep);
	}
	process.readAllStandardOutput();
	if (errors.size() < 512) {
		errors += process.readAllStandardError();
	}
	auto file = QFile(tempPath);
	auto result = QByteArray();
	if (file.open(QIODevice::ReadOnly) && file.size() <= kSevenZipEntryCap) {
		result = file.readAll();
	}
	file.close();
	QFile::remove(tempPath);
	if (result.isEmpty()) {
		const auto details = errors.isEmpty()
			? process.errorString()
			: QString::fromUtf8(errors);
		LOG(("Document cover render produced no output, errors: %1.").arg(
			details.simplified().left(300)));
	}
	return result;
}

[[nodiscard]] QImage TryDjvuPageCover(const QString &filepath) {
	const auto ddjvu = Core::HelperBinaryPath(u"ddjvu"_q);
	if (ddjvu.isEmpty()) {
		return {};
	}
	auto probe = QFile(filepath);
	if (!probe.open(QIODevice::ReadOnly)
		|| probe.read(8) != QByteArray("AT&TFORM", 8)) {
		LOG(("Document cover: not a DjVu file '%1'.").arg(
			QFileInfo(filepath).fileName()));
		return {};
	}
	const auto sizeArg = u"-size="_q
		+ QString::number(kPageRenderWidth)
		+ u'x'
		+ QString::number(kPageRenderWidth);
	for (auto page = 1; page <= kPageCoverPages; ++page) {
		const auto tempPath = QDir::tempPath()
			+ u"/tdesktop_djvu_"_q
			+ QUuid::createUuid().toString(QUuid::WithoutBraces)
			+ u".ppm"_q;
		auto process = QProcess();
		process.start(ddjvu, {
			u"-format=ppm"_q,
			u"-page="_q + QString::number(page),
			sizeArg,
			filepath,
			tempPath,
		});
		auto errors = QByteArray();
		for (auto waited = 0; waited < kSevenZipExtractTimeout; waited += kSevenZipPollStep) {
			if (process.waitForFinished(kSevenZipPollStep)) {
				break;
			}
			process.readAllStandardOutput();
			if (errors.size() < 512) {
				errors += process.readAllStandardError();
			}
		}
		if (process.state() != QProcess::NotRunning) {
			process.kill();
			process.waitForFinished(kSevenZipPollStep);
		}
		process.readAllStandardOutput();
		if (errors.size() < 512) {
			errors += process.readAllStandardError();
		}
		auto file = QFile(tempPath);
		const auto bytes = (file.open(QIODevice::ReadOnly) && file.size() <= kSevenZipEntryCap)
			? file.readAll()
			: QByteArray();
		file.close();
		QFile::remove(tempPath);
		if (bytes.isEmpty()) {
			const auto details = errors.isEmpty()
				? (process.exitCode() == -1073741515
					? u"helper failed to start, missing files beside it (exit %1)"_q.arg(
						process.exitCode())
					: u"%1 (exit %2)"_q.arg(
						process.errorString()).arg(process.exitCode()))
				: QString::fromUtf8(errors);
			LOG(("Document cover render produced no output, errors: %1.").arg(
				details.simplified().left(300)));
			break;
		}
		if (const auto image = QImage::fromData(bytes);
			!image.isNull() && !Images::IsBlank(image)) {
			return image;
		}
	}
	return {};
}

[[nodiscard]] QImage TrySumatraPageCover(const QString &filepath) {
	const auto sumatra = Core::HelperBinaryPath(u"sumatrapdf-tool"_q);
	if (sumatra.isEmpty()) {
		return {};
	}
	for (auto page = 1; page <= kPageCoverPages; ++page) {
		const auto output = TempFileRenderBytes(sumatra, u"draw"_q, filepath, page, true);
		if (output.isEmpty()) {
			break;
		}
		auto image = QImage::fromData(output);
		if (!image.isNull() && !Images::IsBlank(image)) {
			return image;
		}
	}
	return {};
}

[[nodiscard]] QImage TryRtfPictCover(const QByteArray &rtf) {
	auto tried = 0;
	auto i = 0;
	const auto n = rtf.size();
	while (i < n && tried < 3) {
		auto open = rtf.indexOf("{\\pict", i);
		const auto openStar = rtf.indexOf("{\\*\\pict", i);
		if (openStar >= 0 && (open < 0 || openStar < open)) {
			open = openStar;
		}
		if (open < 0) {
			break;
		}
		auto hex = QByteArray();
		auto depth = 0;
		auto j = open;
		while (j < n) {
			const auto ch = rtf[j];
			if (ch == '{') {
				++depth;
				++j;
			} else if (ch == '}') {
				if (--depth <= 0) {
					++j;
					break;
				}
				++j;
			} else if (ch == '\\') {
				++j;
				while (j < n
					&& ((rtf[j] >= 'a' && rtf[j] <= 'z')
						|| (rtf[j] >= 'A' && rtf[j] <= 'Z'))) {
					++j;
				}
				if (j < n && (rtf[j] == '-' || (rtf[j] >= '0' && rtf[j] <= '9'))) {
					if (rtf[j] == '-') {
						++j;
					}
					while (j < n && rtf[j] >= '0' && rtf[j] <= '9') {
						++j;
					}
					if (j < n && rtf[j] == ' ') {
						++j;
					}
				}
			} else if ((ch >= '0' && ch <= '9')
				|| (ch >= 'a' && ch <= 'f')
				|| (ch >= 'A' && ch <= 'F')
				|| ch == ' '
				|| ch == '\r'
				|| ch == '\n'
				|| ch == '\t') {
				hex.push_back(ch);
				++j;
			} else {
				++j;
			}
		}
		i = j;
		++tried;
		hex.replace(' ', "");
		hex.replace("\r", "");
		hex.replace("\n", "");
		hex.replace("\t", "");
		if (hex.size() < 100) {
			continue;
		}
		const auto image = QImage::fromData(QByteArray::fromHex(hex));
		if (!image.isNull() && !Images::IsBlank(image)) {
			return image;
		}
	}
	return {};
}

[[nodiscard]] QString RtfPlainText(const QByteArray &rtf) {
	constexpr auto kMax = 500;
	auto decoderName = QByteArray("windows-1252");
	{
		auto pos = 0;
		while ((pos = rtf.indexOf("\\ansicpg", pos)) >= 0) {
			pos += 8;
			auto digits = QByteArray();
			while (pos < rtf.size() && rtf[pos] >= '0' && rtf[pos] <= '9') {
				digits.push_back(rtf[pos]);
				++pos;
			}
			const auto cp = digits.toInt();
			if ((cp >= 1250 && cp <= 1258) || cp == 874) {
				decoderName = "windows-" + digits;
				break;
			}
		}
	}
	auto decoder = QStringDecoder(decoderName.constData());
	auto result = QString();
	result.reserve(4096);
	const auto isSkipWord = [](const QByteArray &word) {
		for (const auto &skip : {
			"fonttbl", "colortbl", "stylesheet", "info", "pict",
			"header", "footer", "footnote", "xmlnstbl", "listtable",
			"listoverridetable", "rsidtbl", "generator",
		}) {
			if (word == skip) {
				return true;
			}
		}
		return false;
	};
	auto i = 0;
	const auto n = rtf.size();
	auto depth = 0;
	auto skipping = false;
	auto skipDepth = -1;
	while (i < n && result.size() < 2000) {
		const auto ch = rtf[i];
		if (ch == '{') {
			++depth;
			auto j = i + 1;
			if (j < n && rtf[j] == '\\') {
				++j;
				if (j < n && rtf[j] == '*') {
					++j;
					if (j < n && rtf[j] == '\\') {
						++j;
					}
				}
				const auto wordStart = j;
				while (j < n
					&& ((rtf[j] >= 'a' && rtf[j] <= 'z')
						|| (rtf[j] >= 'A' && rtf[j] <= 'Z'))) {
					++j;
				}
				if (isSkipWord(QByteArray(rtf.constData() + wordStart, j - wordStart))) {
					skipping = true;
					skipDepth = depth;
				}
			}
			++i;
		} else if (ch == '}') {
			if (skipping && depth == skipDepth) {
				skipping = false;
			}
			--depth;
			++i;
		} else if (skipping) {
			++i;
		} else if (ch == '\\') {
			++i;
			const auto wordStart = i;
			while (i < n
				&& ((rtf[i] >= 'a' && rtf[i] <= 'z')
					|| (rtf[i] >= 'A' && rtf[i] <= 'Z'))) {
				++i;
			}
			const auto word = QByteArray(rtf.constData() + wordStart, i - wordStart);
			if (word == "par" || word == "line") {
				result.push_back(u'\n');
			} else if (word == "tab") {
				result.push_back(u' ');
			} else if (word == "'") {
				if (i + 1 < n) {
					const auto hex = QByteArray(rtf.constData() + i, 2);
					i += 2;
					auto ok = false;
					const auto byte = uint8(hex.toUShort(&ok, 16));
					if (ok) {
						result += decoder(QByteArray(1, char(byte)));
					}
				}
			} else if (word == "u") {
				auto negative = false;
				if (i < n && rtf[i] == '-') {
					negative = true;
					++i;
				}
				auto digits = 0;
				while (i < n && rtf[i] >= '0' && rtf[i] <= '9') {
					digits = digits * 10 + (rtf[i] - '0');
					++i;
				}
				if (negative) {
					digits = -digits;
				}
				result.push_back(QChar(ushort(digits & 0xFFFF)));
				if (i < n && rtf[i] != '{' && rtf[i] != '\\' && rtf[i] != '}') {
					++i;
				}
			} else {
				while (i < n && rtf[i] >= '0' && rtf[i] <= '9') {
					++i;
				}
			}
			if (i < n && rtf[i] == ' ') {
				++i;
			}
		} else if (ch == '\r' || ch == '\n') {
			++i;
		} else {
			result.push_back(QChar::fromLatin1(ch));
			++i;
		}
	}
	auto lines = QStringList();
	for (const auto &raw : result.split(u'\n')) {
		const auto line = raw.simplified();
		if (!line.isEmpty()) {
			lines.push_back(line);
		}
	}
	while (lines.size() > 10) {
		lines.removeLast();
	}
	return lines.join(u'\n').left(kMax).trimmed();
}

[[nodiscard]] QImage DocumentTextTile(
		const QString &title,
		const QString &snippet) {
	constexpr auto kWidth = 640;
	constexpr auto kHeight = 400;
	auto card = QImage(kWidth, kHeight, QImage::Format_RGB32);
	card.fill(Qt::white);
	auto painter = QPainter(&card);
	const auto margin = 32;
	const auto contentWidth = kWidth - margin * 2;
	auto titleFont = QFont();
	titleFont.setBold(true);
	titleFont.setPixelSize(28);
	painter.setFont(titleFont);
	painter.setPen(QColor(34, 34, 34));
	const auto titleMetrics = QFontMetrics(titleFont);
	painter.drawText(
		margin,
		margin + titleMetrics.ascent(),
		titleMetrics.elidedText(title, Qt::ElideRight, contentWidth));
	auto bodyFont = QFont();
	bodyFont.setPixelSize(24);
	painter.setFont(bodyFont);
	painter.setPen(QColor(110, 110, 110));
	painter.drawText(
		QRect(
			margin,
			margin + titleMetrics.height() + 8,
			contentWidth,
			kHeight),
		Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
		snippet);
	painter.end();
	return card;
}

[[nodiscard]] QImage TryRtfCover(const QString &filepath) {
	auto file = QFile(filepath);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto rtf = file.read(kSevenZipEntryCap);
	if (rtf.isEmpty()) {
		return {};
	}
	if (auto pict = TryRtfPictCover(rtf); !pict.isNull()) {
		return pict;
	}
	const auto text = RtfPlainText(rtf);
	if (text.isEmpty()) {
		return {};
	}
	return DocumentTextTile(QFileInfo(filepath).fileName(), text);
}

} // namespace

constexpr auto kRecompressAfterBpp = 16;

[[nodiscard]] QByteArray ComputePhotoJpegBytes(
		QImage &full,
		const QByteArray &bytes,
		const QByteArray &format) {
	if (!bytes.isEmpty()
		&& (bytes.size()
			<= full.width() * full.height() * kRecompressAfterBpp / 8)
		&& (format == u"jpeg"_q)) {
		if (!Images::IsProgressiveJpeg(bytes)) {
			if (const auto result = Images::MakeProgressiveJpeg(bytes)
				; !result.isEmpty()) {
				return result;
			}
		} else {
			return bytes;
		}
	}

	auto result = QByteArray();
	QBuffer buffer(&result);
	QImageWriter writer(&buffer, "JPEG");
	writer.setQuality(100);
	writer.setProgressiveScanWrite(true);
	writer.write(full);
	buffer.close();

	return result;
}

int PhotoSideLimit(bool large) {
	return large ? 2560 : 1280;
}

int PhotoSideLimit() {
	return PhotoSideLimit(
		Core::App().settings().sendFilesWay().sendLargePhotos());
}

// Computes the exact JPEG bytes the photo uploader would submit for this
// source file, so pick-time dedup can be keyed on the encoded content.
[[nodiscard]] QByteArray PreparePhotoUploadBytes(
		const QString &filepath,
		bool sendLargePhotos) {
	auto image = Images::Opaque(Images::Read({ .path = filepath }).image);
	if (image.isNull()
		|| !Ui::ValidateThumbDimensions(image.width(), image.height())) {
		return QByteArray();
	}
	const auto limit = PhotoSideLimit(sendLargePhotos);
	if (image.width() > limit || image.height() > limit) {
		image = image.scaled(
			limit,
			limit,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	return ComputePhotoJpegBytes(image, QByteArray(), QByteArray());
}

TaskQueue::TaskQueue(crl::time stopTimeoutMs) {
	if (stopTimeoutMs > 0) {
		_stopTimer = new QTimer(this);
		connect(_stopTimer, SIGNAL(timeout()), this, SLOT(stop()));
		_stopTimer->setSingleShot(true);
		_stopTimer->setInterval(int(stopTimeoutMs));
	}
}

TaskId TaskQueue::addTask(std::unique_ptr<Task> &&task) {
	const auto result = task->id();
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		_tasksToProcess.push_back(std::move(task));
	}

	wakeThread();

	return result;
}

void TaskQueue::addTasks(std::vector<std::unique_ptr<Task>> &&tasks) {
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		for (auto &task : tasks) {
			_tasksToProcess.push_back(std::move(task));
		}
	}

	wakeThread();
}

void TaskQueue::wakeThread() {
	if (!_thread) {
		_thread = new QThread();

		_worker = new TaskQueueWorker(this);
		_worker->moveToThread(_thread);

		connect(this, SIGNAL(taskAdded()), _worker, SLOT(onTaskAdded()));
		connect(_worker, SIGNAL(taskProcessed()), this, SLOT(onTaskProcessed()));

		_thread->start();
	}
	if (_stopTimer) _stopTimer->stop();
	taskAdded();
}

void TaskQueue::cancelTask(TaskId id) {
	const auto removeFrom = [&](std::deque<std::unique_ptr<Task>> &queue) {
		const auto proj = [](const std::unique_ptr<Task> &task) {
			return task->id();
		};
		auto i = ranges::find(queue, id, proj);
		if (i != queue.end()) {
			queue.erase(i);
		}
	};
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		removeFrom(_tasksToProcess);
		if (_taskInProcessId == id) {
			_taskInProcessId = TaskId();
		}
	}
	QMutexLocker lock(&_tasksToFinishMutex);
	removeFrom(_tasksToFinish);
}

void TaskQueue::onTaskProcessed() {
	do {
		auto task = std::unique_ptr<Task>();
		{
			QMutexLocker lock(&_tasksToFinishMutex);
			if (_tasksToFinish.empty()) break;
			task = std::move(_tasksToFinish.front());
			_tasksToFinish.pop_front();
		}
		task->finish();
	} while (true);

	if (_stopTimer) {
		QMutexLocker lock(&_tasksToProcessMutex);
		if (_tasksToProcess.empty() && !_taskInProcessId) {
			_stopTimer->start();
		}
	}
}

void TaskQueue::stop() {
	if (_thread) {
		_thread->requestInterruption();
		_thread->quit();
		LOG(("Waiting for taskThread to finish"));
		_thread->wait();
		LOG(("TaskQueue::stop: thread finished, cleaning up worker"));
		delete base::take(_worker);
		LOG(("TaskQueue::stop: worker deleted, cleaning up thread"));
		delete base::take(_thread);
		LOG(("TaskQueue::stop: thread deleted"));
	}
	LOG(("TaskQueue::stop: clearing tasks (count=%1)")
		.arg(_tasksToProcess.size() + _tasksToFinish.size()));
	_tasksToProcess.clear();
	_tasksToFinish.clear();
	_taskInProcessId = TaskId();
	LOG(("TaskQueue::stop: done"));
}

TaskQueue::~TaskQueue() {
	stop();
	delete _stopTimer;
}

void TaskQueueWorker::onTaskAdded() {
	if (_inTaskAdded) return;
	_inTaskAdded = true;

	bool someTasksLeft = false;
	do {
		auto task = std::unique_ptr<Task>();
		{
			QMutexLocker lock(&_queue->_tasksToProcessMutex);
			if (!_queue->_tasksToProcess.empty()) {
				task = std::move(_queue->_tasksToProcess.front());
				_queue->_tasksToProcess.pop_front();
				_queue->_taskInProcessId = task->id();
			}
		}

		if (task) {
			task->process();
			bool emitTaskProcessed = false;
			{
				QMutexLocker lockToProcess(&_queue->_tasksToProcessMutex);
				if (_queue->_taskInProcessId == task->id()) {
					_queue->_taskInProcessId = TaskId();
					someTasksLeft = !_queue->_tasksToProcess.empty();

					QMutexLocker lockToFinish(&_queue->_tasksToFinishMutex);
					emitTaskProcessed = _queue->_tasksToFinish.empty();
					_queue->_tasksToFinish.push_back(std::move(task));
				}
			}
			if (emitTaskProcessed) {
				taskProcessed();
			}
		}
		QCoreApplication::processEvents();
	} while (someTasksLeft && !thread()->isInterruptionRequested());

	_inTaskAdded = false;
}

SendingAlbum::SendingAlbum() : groupId(base::RandomValue<uint64>()) {
}

bool SendingAlbum::preparedMusicBatching() const {
	return musicPreparedBatching;
}

bool SendingAlbum::preparedMusicReady() const {
	return preparedMusicBatching()
		&& !items.empty()
		&& ranges::all_of(items, [](const Item &item) {
			return item.prepared != nullptr;
		});
}

std::shared_ptr<FilePrepareResult> SendingAlbum::preparedMusicSample() const {
	const auto it = ranges::find_if(items, [](const Item &item) {
		return item.prepared != nullptr;
	});
	return (it == end(items)) ? nullptr : it->prepared;
}

std::vector<std::shared_ptr<FilePrepareResult>> SendingAlbum::takePreparedMusic() {
	auto result = std::vector<std::shared_ptr<FilePrepareResult>>();
	if (!preparedMusicReady()) {
		return result;
	}
	result.reserve(items.size());
	for (auto &item : items) {
		result.push_back(std::move(item.prepared));
	}
	return result;
}

void SendingAlbum::fillMedia(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		uint64 randomId) {
	const auto i = FindAlbumItem(items, item);
	Assert(!i->media);

	i->randomId = randomId;
	i->media = PrepareAlbumItemMedia(item, media, randomId);
}

void SendingAlbum::refreshMediaCaption(not_null<HistoryItem*> item) {
	const auto i = FindAlbumItem(items, item);
	if (!i->media) {
		return;
	}
	i->media = i->media->match([&](const MTPDinputSingleMedia &data) {
		return PrepareAlbumItemMedia(
			item,
			data.vmedia(),
			data.vrandom_id().v);
	});
}

void SendingAlbum::removeItem(not_null<HistoryItem*> item) {
	const auto localId = item->fullId();
	const auto i = ranges::find(items, localId, &Item::msgId);
	const auto moveCaption = (items.size() > 1) && (i == begin(items));
	Assert(i != end(items));
	items.erase(i);
	if (expectedCount > 0) {
		expectedCount--;
	}
	if (moveCaption) {
		auto caption = item->originalText();
		const auto firstId = items.front().msgId;
		if (const auto first = item->history()->owner().message(firstId)) {
			// We don't need to finishEdition() here, because the whole
			// album will be rebuilt after one item was removed from it.
			auto firstCaption = first->originalText();
			first->setText(firstCaption.text.isEmpty()
				? std::move(caption)
				: firstCaption.append('\n').append(std::move(caption)));
			refreshMediaCaption(first);
		}
	}
}

void SendingAlbum::removeTask(TaskId taskId) {
	const auto i = ranges::find(items, taskId, &Item::taskId);
	Assert(i != end(items));
	items.erase(i);
	if (expectedCount > 0) {
		expectedCount--;
	}
}

SendingAlbum::Item::Item(TaskId taskId)
: taskId(taskId) {
}

FilePrepareResult::~FilePrepareResult() {
	if (!transcodedTempPath.isEmpty()) {
		QFile::remove(transcodedTempPath);
	}
}

FilePrepareResult::FilePrepareResult(FilePrepareDescriptor &&descriptor)
: taskId(descriptor.taskId)
, id(descriptor.id)
, to(std::move(descriptor.to))
, album(std::move(descriptor.album))
, type(descriptor.type)
, caption(std::move(descriptor.caption))
, spoiler(descriptor.spoiler) {
}

void FilePrepareResult::setFileData(const QByteArray &filedata) {
	if (filedata.isEmpty()) {
		partssize = 0;
	} else {
		partssize = filedata.size();
		fileparts.reserve(
			(partssize + kPhotoUploadPartSize - 1) / kPhotoUploadPartSize);
		for (int32 i = 0, part = 0; i < partssize; i += kPhotoUploadPartSize, ++part) {
			fileparts.push_back(filedata.mid(i, kPhotoUploadPartSize));
		}
		filemd5.resize(32);
		hashMd5Hex(filedata.constData(), filedata.size(), filemd5.data());
	}
}

void FilePrepareResult::setThumbData(const QByteArray &thumbdata) {
	if (!thumbdata.isEmpty()) {
		thumbbytes = thumbdata;
		int32 size = thumbdata.size();
		thumbparts.reserve(
			(size + kPhotoUploadPartSize - 1) / kPhotoUploadPartSize);
		for (int32 i = 0, part = 0; i < size; i += kPhotoUploadPartSize, ++part) {
			thumbparts.push_back(thumbdata.mid(i, kPhotoUploadPartSize));
		}
		thumbmd5.resize(32);
		hashMd5Hex(thumbdata.constData(), thumbdata.size(), thumbmd5.data());
	}
}

std::shared_ptr<FilePrepareResult> MakePreparedFile(
		FilePrepareDescriptor &&descriptor) {
	return std::make_shared<FilePrepareResult>(std::move(descriptor));
}

FileLoadTask::FileLoadTask(Args &&args)
: _id(args.idOverride ? args.idOverride : base::RandomValue<uint64>())
, _session(args.session)
, _dcId(args.session->mainDcId())
, _to(std::move(args.to))
, _album(std::move(args.album))
, _filepath(std::move(args.filepath))
, _displayName(std::move(args.displayName))
, _content(std::move(args.content))
, _videoCover(std::move(args.videoCover))
, _information(std::move(args.information))
, _type(args.type)
, _caption(std::move(args.caption))
, _spoiler(args.spoiler)
, _forceFile(args.forceFile)
, _sendLargePhotos(args.sendLargePhotos)
, _animationJob(std::move(args.animationJob)) {
	Expects(_to.options.scheduled
		|| _to.options.shortcutId
		|| !_to.replaceMediaOf
		|| IsServerMsgId(_to.replaceMediaOf));
}

FileLoadTask::FileLoadTask(VoiceArgs &&args)
: _id(base::RandomValue<uint64>())
, _session(args.session)
, _dcId(args.session->mainDcId())
, _to(std::move(args.to))
, _content(std::move(args.voice))
, _duration(args.duration)
, _waveform(std::move(args.waveform))
, _type(args.video ? SendMediaType::Round : SendMediaType::Audio)
, _caption(std::move(args.caption)) {
}

FileLoadTask::~FileLoadTask() = default;

[[nodiscard]] bool LooksLikeAudio(
		const QString &filepath,
		const QByteArray &content) {
	const auto head = content.isEmpty() && !filepath.isEmpty()
		? ([&] {
			auto file = QFile(filepath);
			return file.open(QIODevice::ReadOnly)
				? file.read(12)
				: QByteArray();
		})()
		: content.left(12);
	if (head.size() < 4) {
		return false;
	}
	const auto data = head.constData();
	if (!memcmp(data, "ID3", 3)
		|| !memcmp(data, "MP+", 3)
		|| !memcmp(data, "RIFF", 4)
		|| !memcmp(data, "OggS", 4)
		|| !memcmp(data, "fLaC", 4)
		|| !memcmp(data, "FORM", 4)
		|| !memcmp(data, "wvpk", 4)
		|| !memcmp(data, "MAC ", 4)
		|| !memcmp(data, "MPCK", 4)) {
		return true;
	}
	const auto first = uint8(data[0]);
	const auto second = uint8(data[1]);
	if (first == 0xFF
		&& (second & 0xE0) == 0xE0
		&& (second & 0x18) != 0x08
		&& (second & 0x06) != 0x00) {
		return true;
	}
	if (!memcmp(data, "\x30\x26\xB2\x75", 4)) {
		return true;
	}
	return head.size() >= 12
		&& !memcmp(data + 4, "ftyp", 4);
}

auto FileLoadTask::ReadMediaInformation(
	const QString &filepath,
	const QByteArray &content,
	const QString &filemime)
-> std::unique_ptr<Ui::PreparedFileInformation> {
	auto result = std::make_unique<Ui::PreparedFileInformation>();
	result->filemime = filemime;

	if (CheckForSong(filepath, content, result)) {
		return result;
	} else if (CheckForVideo(filepath, content, result)) {
		return result;
	} else if (CheckForImage(filepath, content, result)) {
		return result;
	} else if (CheckForDocument(filepath, content, result)) {
		return result;
	}
	if (v::is<v::null_t>(result->media)
		&& LooksLikeAudio(filepath, content)) {
		auto check = Media::Player::PrepareForSending(
			filepath,
			content);
		auto &song = v::get<Ui::PreparedFileInformation::Song>(
			check.media);
		if (song.duration >= 0) {
			if (!ValidateThumbDimensions(
				song.cover.width(),
				song.cover.height())) {
				song.cover = QImage();
			}
			result->media = std::move(song);
			result->filemime = u"audio/mp4"_q;
		}
	}
	return result;
}

template <typename Mimes, typename Extensions>
bool FileLoadTask::CheckMimeOrExtensions(
		const QString &filepath,
		const QString &filemime,
		Mimes &mimes,
		Extensions &extensions) {
	if (std::find(std::begin(mimes), std::end(mimes), filemime) != std::end(mimes)) {
		return true;
	}
	if (std::find_if(std::begin(extensions), std::end(extensions), [&filepath](auto &extension) {
		return filepath.endsWith(extension, Qt::CaseInsensitive);
	}) != std::end(extensions)) {
		return true;
	}
	return false;
}

bool FileLoadTask::CheckForSong(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	static const auto mimes = {
		u"audio/aac"_q,
		u"audio/ac3"_q,
		u"audio/ac4"_q,
		u"audio/aiff"_q,
		u"audio/ape"_q,
		u"audio/basic"_q,
		u"audio/dff"_q,
		u"audio/dsd"_q,
		u"audio/eac3"_q,
		u"audio/flac"_q,
		u"audio/m4a"_q,
		u"audio/m4b"_q,
		u"audio/mp3"_q,
		u"audio/mp4"_q,
		u"audio/mpeg"_q,
		u"audio/musepack"_q,
		u"audio/ogg"_q,
		u"audio/opus"_q,
		u"audio/pcm"_q,
		u"audio/vnd.dts"_q,
		u"audio/vnd.dts.hd"_q,
		u"audio/vnd.wave"_q,
		u"audio/vorbis"_q,
		u"audio/wav"_q,
		u"audio/wave"_q,
		u"audio/webm"_q,
		u"audio/x-aiff"_q,
		u"audio/x-caf"_q,
		u"audio/x-dff"_q,
		u"audio/x-dop"_q,
		u"audio/x-dsd"_q,
		u"audio/x-dsf"_q,
		u"audio/x-dsdiff"_q,
		u"audio/x-flac"_q,
		u"audio/x-m4a"_q,
		u"audio/x-matroska"_q,
		u"audio/x-m4b"_q,
		u"audio/x-monkeys-audio"_q,
		u"audio/x-ms-wma"_q,
		u"audio/x-musepack"_q,
		u"audio/x-wav"_q,
		u"audio/x-wavpack"_q,
	};
	static const auto extensions = {
		u".aac"_q,
		u".ac4"_q,
		u".aif"_q,
		u".aifc"_q,
		u".aiff"_q,
		u".aff"_q,
		u".alac"_q,
		u".ape"_q,
		u".au"_q,
		u".caf"_q,
		u".dff"_q,
		u".dsf"_q,
		u".dts"_q,
		u".dtshd"_q,
		u".f4a"_q,
		u".f4b"_q,
		u".flac"_q,
		u".m4a"_q,
		u".m4b"_q,
		u".m4r"_q,
		u".mka"_q,
		u".mp+"_q,
		u".mp2"_q,
		u".mp3"_q,
		u".mpc"_q,
		u".mpp"_q,
		u".oga"_q,
		u".ogg"_q,
		u".ogx"_q,
		u".opus"_q,
		u".pcm"_q,
		u".wav"_q,
		u".webma"_q,
		u".wma"_q,
		u".wsd"_q,
		u".wv"_q,
		u".snd"_q,
	};
	if (!filepath.isEmpty()
		&& !CheckMimeOrExtensions(
			filepath,
			result->filemime,
			mimes,
			extensions)) {
		return false;
	}

	auto media = v::get<Ui::PreparedFileInformation::Song>(
		Media::Player::PrepareForSending(filepath, content).media);
	if (media.duration < 0) {
		return false;
	}
	if (!ValidateThumbDimensions(media.cover.width(), media.cover.height())) {
		media.cover = QImage();
	}
	static const auto extToMime = {
		std::pair(u".aac"_q, u"audio/aac"_q),
		std::pair(u".ac4"_q, u"audio/ac4"_q),
		std::pair(u".aif"_q, u"audio/aiff"_q),
		std::pair(u".aifc"_q, u"audio/aiff"_q),
		std::pair(u".aiff"_q, u"audio/aiff"_q),
		std::pair(u".aff"_q, u"audio/x-dff"_q),
		std::pair(u".alac"_q, u"audio/x-alac"_q),
		std::pair(u".ape"_q, u"audio/x-ape"_q),
		std::pair(u".au"_q, u"audio/basic"_q),
		std::pair(u".caf"_q, u"audio/x-caf"_q),
		std::pair(u".dff"_q, u"audio/dff"_q),
		std::pair(u".dsf"_q, u"audio/x-dsf"_q),
		std::pair(u".dts"_q, u"audio/vnd.dts"_q),
		std::pair(u".dtshd"_q, u"audio/vnd.dts.hd"_q),
		std::pair(u".f4a"_q, u"audio/mp4"_q),
		std::pair(u".f4b"_q, u"audio/mp4"_q),
		std::pair(u".flac"_q, u"audio/flac"_q),
		std::pair(u".m4a"_q, u"audio/mp4"_q),
		std::pair(u".m4b"_q, u"audio/mp4"_q),
		std::pair(u".m4r"_q, u"audio/mp4"_q),
		std::pair(u".mka"_q, u"audio/x-matroska"_q),
		std::pair(u".mp+"_q, u"audio/x-musepack"_q),
		std::pair(u".mp2"_q, u"audio/mpeg"_q),
		std::pair(u".mp3"_q, u"audio/mpeg"_q),
		std::pair(u".mpga"_q, u"audio/mpeg"_q),
		std::pair(u".mpc"_q, u"audio/x-musepack"_q),
		std::pair(u".mpp"_q, u"audio/x-musepack"_q),
		std::pair(u".oga"_q, u"audio/ogg"_q),
		std::pair(u".ogg"_q, u"audio/ogg"_q),
		std::pair(u".ogx"_q, u"audio/ogg"_q),
		std::pair(u".opus"_q, u"audio/opus"_q),
		std::pair(u".pcm"_q, u"audio/pcm"_q),
		std::pair(u".snd"_q, u"audio/basic"_q),
		std::pair(u".wav"_q, u"audio/wav"_q),
		std::pair(u".webma"_q, u"audio/webm"_q),
		std::pair(u".wma"_q, u"audio/x-ms-wma"_q),
		std::pair(u".wsd"_q, u"audio/x-wsd"_q),
		std::pair(u".wv"_q, u"audio/x-wavpack"_q),
	};
	for (const auto &[ext, mime] : extToMime) {
		if (filepath.endsWith(ext, Qt::CaseInsensitive)) {
			result->filemime = mime;
			break;
		}
	}
	result->media = std::move(media);
	return true;
}

bool FileLoadTask::CheckForVideo(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	static const auto mimes = {
		u"application/mxf"_q,
		u"application/vnd.adobe.flash.movie"_q,
		u"application/vnd.rn-realmedia-vbr"_q,
		u"application/x-shockwave-flash"_q,
		u"video/asf"_q,
		u"video/avi"_q,
		u"video/dvd"_q,
		u"video/mp2t"_q,
		u"video/mp4"_q,
		u"video/mpeg"_q,
		u"video/msvideo"_q,
		u"video/ogg"_q,
		u"video/quicktime"_q,
		u"video/vnd.dvb.file"_q,
		u"video/webm"_q,
		u"video/wmv"_q,
		u"video/x-flv"_q,
		u"video/x-m4v"_q,
		u"video/x-matroska"_q,
		u"video/x-ms-asf"_q,
		u"video/x-ms-wm"_q,
		u"video/x-ms-wmv"_q,
		u"video/x-ms-wmv"_q,
		u"video/x-msvideo"_q,
		u"video/x-pn-realvideo"_q,
		u"video/x-quicktime"_q,
	};
	static const auto extensions = {
		u".asf"_q,
		u".asx"_q,
		u".avi"_q,
		u".f4v"_q,
		u".flv"_q,
		u".m2ts"_q,
		u".m2v"_q,
		u".m4v"_q,
		u".mkv"_q,
		u".mov"_q,
		u".mp4"_q,
		u".mts"_q,
		u".mxf"_q,
		u".ogm"_q,
		u".ogv"_q,
		u".swf"_q,
		u".ts"_q,
		u".vob"_q,
		u".webm"_q,
		u".wmv"_q,
		u".wtv"_q,
	};
	if (!CheckMimeOrExtensions(filepath, result->filemime, mimes, extensions)) {
		return false;
	}

	auto media = v::get<Ui::PreparedFileInformation::Video>(
		Media::Clip::PrepareForSending(filepath, content).media);
	auto coverWidth = media.thumbnail.width();
	auto coverHeight = media.thumbnail.height();
	if (media.duration <= 0 && coverWidth <= 0) {
		// No thumbnail and no duration — not a real video.
		return false;
	}
	if (!ValidateThumbDimensions(coverWidth, coverHeight)) {
		return false;
	}

	static const auto extToMime = {
		std::pair(u".mp4"_q, u"video/mp4"_q),
		std::pair(u".mov"_q, u"video/quicktime"_q),
		std::pair(u".qt"_q, u"video/quicktime"_q),
		std::pair(u".mkv"_q, u"video/mp4"_q),
		std::pair(u".webm"_q, u"video/mp4"_q),
		std::pair(u".asf"_q, u"video/mp4"_q),
		std::pair(u".asx"_q, u"video/mp4"_q),
		std::pair(u".avi"_q, u"video/mp4"_q),
		std::pair(u".wmv"_q, u"video/mp4"_q),
		std::pair(u".ts"_q, u"video/mp4"_q),
		std::pair(u".mts"_q, u"video/mp4"_q),
		std::pair(u".m2ts"_q, u"video/mp4"_q),
		std::pair(u".tp"_q, u"video/mp4"_q),
		std::pair(u".trp"_q, u"video/mp4"_q),
		std::pair(u".flv"_q, u"video/mp4"_q),
		std::pair(u".m2v"_q, u"video/mp4"_q),
		std::pair(u".mpeg"_q, u"video/mp4"_q),
		std::pair(u".mpg"_q, u"video/mp4"_q),
		std::pair(u".mpv"_q, u"video/mp4"_q),
		std::pair(u".m2p"_q, u"video/mp4"_q),
		std::pair(u".m2s"_q, u"video/mp4"_q),
		std::pair(u".m2t"_q, u"video/mp4"_q),
		std::pair(u".ps"_q, u"video/mp4"_q),
		std::pair(u".vob"_q, u"video/mp4"_q),
		std::pair(u".wtv"_q, u"video/mp4"_q),
		std::pair(u".3gp"_q, u"video/mp4"_q),
		std::pair(u".3gpp"_q, u"video/mp4"_q),
		std::pair(u".3g2"_q, u"video/mp4"_q),
		std::pair(u".m4v"_q, u"video/mp4"_q),
		std::pair(u".f4v"_q, u"video/mp4"_q),
		std::pair(u".ogv"_q, u"video/mp4"_q),
		std::pair(u".ogm"_q, u"video/mp4"_q),
		std::pair(u".rm"_q, u"video/mp4"_q),
		std::pair(u".rv"_q, u"video/mp4"_q),
		std::pair(u".rmvb"_q, u"video/mp4"_q),
		std::pair(u".divx"_q, u"video/mp4"_q),
		std::pair(u".xvid"_q, u"video/mp4"_q),
		std::pair(u".mxf"_q, u"video/mp4"_q),
		std::pair(u".dav"_q, u"video/mp4"_q),
	};
	for (const auto &[ext, mime] : extToMime) {
		if (filepath.endsWith(ext, Qt::CaseInsensitive)) {
			result->filemime = mime;
			break;
		}
	}
	result->media = std::move(media);
	return true;
}

bool FileLoadTask::CheckForImage(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	auto read = [&] {
		if (filepath.endsWith(u".tgs"_q, Qt::CaseInsensitive)) {
			auto image = Lottie::ReadThumbnail(
				Lottie::ReadContent(content, filepath));
			const auto success = !image.isNull();
			if (success) {
				result->filemime = u"application/x-tgsticker"_q;
			}
			return Images::ReadResult{
				.image = std::move(image),
				.animated = success,
			};
		}
		return Images::Read({
			.path = filepath,
			.content = content,
			.returnContent = true,
		});
	}();
	return FillImageInformation(
		std::move(read.image),
		read.animated,
		result,
		std::move(read.content),
		std::move(read.format));
}

bool FileLoadTask::CheckForDocument(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	static const auto mimes = {
		u"application/pdf"_q,
		u"application/x-mobipocket-ebook"_q,
		u"application/epub+zip"_q,
		u"application/vnd.openxmlformats-officedocument.wordprocessingml.document"_q,
		u"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"_q,
		u"application/vnd.openxmlformats-officedocument.presentationml.presentation"_q,
		u"application/x-cbr"_q,
		u"application/vnd.rar"_q,
	};
	static const auto extensions = {
		u".pdf"_q,
		u".epub"_q,
		u".cbz"_q,
		u".cbr"_q,
		u".cb7"_q,
		u".cbt"_q,
		u".mobi"_q,
		u".prc"_q,
		u".azw"_q,
		u".azw3"_q,
		u".azw4"_q,
		u".pdb"_q,
		u".djvu"_q,
		u".djv"_q,
		u".chm"_q,
		u".docx"_q,
		u".xlsx"_q,
		u".pptx"_q,
		u".odt"_q,
		u".ods"_q,
		u".odp"_q,
		u".fb2"_q,
		u".fbz"_q,
		u".fb2.zip"_q,
		u".txt"_q,
		u".text"_q,
		u".rtf"_q,
		u".md"_q,
		u".markdown"_q,
		u".xps"_q,
		u".oxps"_q,
		u".html"_q,
		u".htm"_q,
		u".xhtml"_q,
	};
	if (!filepath.isEmpty()
		&& !CheckMimeOrExtensions(
			filepath,
			result->filemime,
			mimes,
			extensions)) {
		return false;
	}


	auto tryExtractMobiCover = [&] {
		auto file = QFile(filepath);
		if (!file.open(QIODevice::ReadOnly)) {
			return QImage();
		}
		const auto fileSize = file.size();
		const auto mapped = (fileSize >= 86) ? file.map(0, fileSize) : nullptr;
		if (!mapped) {
			return QImage();
		}
		const auto cover = [&]() -> QImage {
			const auto data = reinterpret_cast<const unsigned char*>(mapped);
			const auto size = fileSize;

			const auto read16 = [&](qint64 pos, uint16 &out) {
				if (pos < 0 || pos + 2 > size) {
					return false;
				}
				out = (uint16(data[pos]) << 8) | data[pos + 1];
				return true;
			};
			const auto read32 = [&](qint64 pos, uint32 &out) {
				if (pos < 0 || pos + 4 > size) {
					return false;
				}
				out = (uint32(data[pos]) << 24)
					| (uint32(data[pos + 1]) << 16)
					| (uint32(data[pos + 2]) << 8)
					| data[pos + 3];
				return true;
			};
			const auto matches = [&](qint64 pos, const char *magic) {
				return pos >= 0
					&& pos + 4 <= size
					&& !memcmp(data + pos, magic, 4);
			};

			if (!matches(60, "BOOK") || !matches(64, "MOBI")) {
				return QImage();
			}

			uint16 numRecords = 0;
			uint32 rec0Offset = 0;
			if (!read16(76, numRecords) || numRecords < 1
				|| !read32(78, rec0Offset)) {
				return QImage();
			}

			auto firstResource = (uint32)-1;
			uint32 mobiLen = 0;
			uint32 fr = 0;
			if (matches(qint64(rec0Offset) + 16, "MOBI")
				&& read32(qint64(rec0Offset) + 20, mobiLen)
				&& mobiLen >= 0x6C - 16 + 4
				&& read32(qint64(rec0Offset) + 0x6C, fr)
				&& fr != 0xFFFFFFFF) {
				firstResource = fr;
			} else {
				uint16 textRecords = 0;
				if (!read16(qint64(rec0Offset) + 8, textRecords)) {
					return QImage();
				}
				firstResource = uint32(textRecords) + 1;
			}

			const auto isMarkerRecord = [&](qint64 off) {
				if (off < 0 || off + 4 > size) {
					return true;
				}
				const auto m = data + off;
				return (m[0] == 'F' && m[1] == 'L' && m[2] == 'I' && m[3] == 'S')
					|| (m[0] == 'F' && m[1] == 'C' && m[2] == 'I' && m[3] == 'S')
					|| (m[0] == 'S' && m[1] == 'R' && m[2] == 'C' && m[3] == 'S')
					|| (m[0] == 'R' && m[1] == 'E' && m[2] == 'S' && m[3] == 'C')
					|| (m[0] == 'B' && m[1] == 'O' && m[2] == 'U' && m[3] == 'N')
					|| (m[0] == 'F' && m[1] == 'D' && m[2] == 'S' && m[3] == 'T')
					|| (m[0] == 'D' && m[1] == 'A' && m[2] == 'T' && m[3] == 'P')
					|| (m[0] == 'A' && m[1] == 'U' && m[2] == 'D' && m[3] == 'I')
					|| (m[0] == 'V' && m[1] == 'I' && m[2] == 'D' && m[3] == 'E')
					|| (m[0] == 0xE9 && m[1] == 0x8E && m[2] == 0x0D && m[3] == 0x0A);
			};
			const auto decodeRecord = [&](qint64 off, qint64 end) -> QImage {
				if (off < 0 || end <= off || end - off > kSevenZipEntryCap) {
					return {};
				}
				if (off + 2 > size || end > size) {
					return {};
				}
				return QImage::fromData(QByteArray::fromRawData(
					reinterpret_cast<const char*>(data + off),
					int(end - off)));
			};
			const auto recordEnd = [&](uint64 index) {
				uint32 end = 0;
				if (index + 1 < numRecords
					&& read32(78 + qint64(index + 1) * 8, end)) {
					return qint64(end);
				}
				return size;
			};

			uint32 rec0Next = 0;
			const auto rec0End = (numRecords > 1
				&& read32(78 + 8, rec0Next)
				&& qint64(rec0Next) <= size)
				? qint64(rec0Next)
				: size;
			constexpr auto kMaxExthCount = 256;
			for (auto i = qint64(rec0Offset); i < rec0End && i < size; ++i) {
				if (!matches(i, "EXTH")) {
					continue;
				}
				uint32 exthLen = 0;
				uint32 exthCount = 0;
				if (!read32(i + 4, exthLen)
					|| exthLen < 12
					|| i + exthLen > rec0End
					|| !read32(i + 8, exthCount)
					|| exthCount > kMaxExthCount) {
					continue;
				}
				auto pos = i + 12;
				for (auto j = 0u; j < exthCount; ++j) {
					uint32 type = 0;
					uint32 length = 0;
					if (!read32(pos, type)
						|| !read32(pos + 4, length)
						|| length < 8
						|| pos + length > size) {
						break;
					}
					const auto contentPos = pos + 8;
					const auto contentLen = qint64(length) - 8;
					pos += length;

					if (type == 202
						&& contentLen > 8
						&& contentLen <= kSevenZipEntryCap) {
						const auto raw = QByteArray::fromRawData(
							reinterpret_cast<const char*>(data + contentPos),
							int(contentLen));
						auto image = QImage::fromData(raw);
						if (image.isNull()) {
							image = QImage::fromData(raw.mid(8));
						}
						if (!image.isNull()) {
							return image;
						}
					}
					if ((type == 201 && contentLen >= 4)
						|| (type == 202 && contentLen == 4)) {
						uint32 coverIdx = 0;
						if (!read32(contentPos, coverIdx)) {
							continue;
						}
						const auto absoluteIdx = uint64(firstResource) + coverIdx;
						uint32 off = 0;
						if (absoluteIdx >= numRecords
							|| !read32(78 + qint64(absoluteIdx) * 8, off)) {
							continue;
						}
						if (isMarkerRecord(off)) {
							continue;
						}
						if (auto image = decodeRecord(off, recordEnd(absoluteIdx));
							!image.isNull()) {
							return image;
						}
					}
				}
				break;
			}

			for (auto idx = 0; idx < numRecords; ++idx) {
				uint32 off = 0;
				if (!read32(78 + qint64(idx) * 8, off)) {
					continue;
				}
				const auto end = recordEnd(idx);
				if (qint64(off) + 2 > size || end <= off) {
					continue;
				}
				const auto b = data + qint64(off);
				if (!((b[0] == 0xFF && b[1] == 0xD8)
					|| (b[0] == 0x89 && b[1] == 0x50)
					|| (b[0] == 'G' && b[1] == 'I'))) {
					continue;
				}
				if (auto image = decodeRecord(off, end);
					!image.isNull()
					&& image.width() >= 100
					&& image.height() >= 100) {
					return image;
				}
			}
			return QImage();
		}();
		file.unmap(mapped);
		return cover;
	};

	auto image = [&]() -> QImage {
		if (filepath.endsWith(u".djvu"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".djv"_q, Qt::CaseInsensitive)) {
			if (auto cover = TryDjvuPageCover(filepath); !cover.isNull()) {
				return cover;
			}
		}
		const auto rtf = filepath.endsWith(u".rtf"_q, Qt::CaseInsensitive);
		if (rtf) {
			if (auto cover = TryRtfCover(filepath); !cover.isNull()) {
				return cover;
			}
		}
		const auto odf = filepath.endsWith(u".odt"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".ods"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".odp"_q, Qt::CaseInsensitive);
		if (odf) {
			if (auto sumatra = TrySumatraPageCover(filepath); !sumatra.isNull()) {
				return sumatra;
			}
		}
		if (auto cover = TrySevenZipDocumentCover(filepath); !cover.isNull()) {
			return cover;
		}
		if (filepath.endsWith(u".mobi"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".prc"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".azw"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".azw3"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".azw4"_q, Qt::CaseInsensitive)) {
			if (auto cover = tryExtractMobiCover(); !cover.isNull()) {
				return cover;
			}
		}
		if (!odf && !rtf) {
			if (auto sumatra = TrySumatraPageCover(filepath); !sumatra.isNull()) {
				return sumatra;
			}
		}
		return {};
	}();
	if (image.isNull()) {
		return false;
	}
	if (!ValidateThumbDimensions(image.width(), image.height())) {
		return false;
	}
	result->fileThumbnail = std::move(image);
	return true;
}

bool FileLoadTask::FillImageInformation(
		QImage &&image,
		bool animated,
		std::unique_ptr<Ui::PreparedFileInformation> &result,
		QByteArray content,
		QByteArray format) {
	Expects(result != nullptr);

	if (image.isNull()) {
		return false;
	}
	auto media = Ui::PreparedFileInformation::Image();
	media.data = std::move(image);
	media.bytes = std::move(content);
	media.format = std::move(format);
	media.animated = animated;
	result->media = media;
	return true;
}

void FileLoadTask::process(ProcessArgs &&args) {
	_result = MakePreparedFile({
		.taskId = id(),
		.id = _id,
		.to = _to,
		.caption = _caption,
		.spoiler = _spoiler,
		.album = _album,
	});
	if (const auto cover = _videoCover.get()) {
		cover->process();
		if (const auto &result = cover->peekResult()) {
			if (result->type == SendMediaType::Photo
				&& !result->fileparts.empty()) {
				_result->videoCover = result;
			}
		}
	}

	auto animationPreparing = false;
	if (_animationJob) {
		const auto still = _forceFile
			? nullptr
			: std::get_if<Media::Encode::StillSource>(&_animationJob->source);
		auto preview = QImage();
		if (_information) {
			const auto media = &_information->media;
			if (const auto image = std::get_if<
					Ui::PreparedFileInformation::Image>(media)) {
				preview = std::move(image->data);
			}
		}
		if (still && !still->base.isNull() && still->duration > 0) {
			if (preview.isNull()) {
				preview = still->base;
			} else if (preview.size() != still->base.size()) {
				preview = preview.scaled(
					still->base.size(),
					Qt::IgnoreAspectRatio,
					Qt::SmoothTransformation);
			}
			auto information = std::make_unique<
				Ui::PreparedFileInformation>();
			information->filemime = "video/mp4";
			information->media = Ui::PreparedFileInformation::Video{
				.isGifv = true,
				.supportsStreaming = true,
				.duration = still->duration,
				.thumbnail = std::move(preview),
			};
			_information = std::move(information);
			_content = QByteArray();
			_filepath = QString();
			_type = SendMediaType::File;
			_displayName = u"animation.mp4"_q;
			_result->animationJob = _animationJob;
			animationPreparing = true;
		}
		_animationJob = nullptr;
	}

	QString filename, filemime;
	qint64 filesize = 0;
	QByteArray filedata;

	auto isAnimation = false;
	auto isSong = false;
	auto isVideo = false;
	auto isVoice = (_type == SendMediaType::Audio);
	auto isRound = (_type == SendMediaType::Round);
	auto isSticker = false;

	auto fullimage = QImage();
	auto fullimagebytes = QByteArray();
	auto fullimageformat = QByteArray();
	auto info = _filepath.isEmpty() ? QFileInfo() : QFileInfo(_filepath);
	if (info.exists()) {
		if (info.isDir()) {
			_result->filesize = -1;
			return;
		}

		// Voice sending is supported only from memory for now.
		// Because for voice we force mime type and don't read MediaInformation.
		// For a real file we always read mime type and read MediaInformation.
		Assert(!isVoice && !isRound);

		filesize = info.size();
		filename = info.fileName();
		if (!_information) {
			_information = readMediaInformation(Core::MimeTypeForFile(info).name());
		}
		filemime = _information->filemime;
		if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
				&_information->media)) {
			fullimage = base::take(image->data);
			fullimagebytes = base::take(image->bytes);
			fullimageformat = base::take(image->format);
			if (!Core::IsMimeSticker(filemime)
				&& fullimageformat != u"jpeg"_q) {
				fullimage = Images::Opaque(std::move(fullimage));
				fullimagebytes = fullimageformat = QByteArray();
			}
			isAnimation = image->animated;
		}
	} else if (animationPreparing) {
		const auto video = std::get_if<Ui::PreparedFileInformation::Video>(
			&_information->media);
		const auto seconds = std::max(
			int64(video->duration / 1000),
			int64(1));
		filesize = seconds * 200'000;
		filename = filedialogDefaultName(
			u"animation"_q,
			u".mp4"_q,
			QString(),
			true);
		filemime = "video/mp4";
	} else if (!_content.isEmpty()) {
		filesize = _content.size();
		if (isVoice) {
			filename = filedialogDefaultName(u"audio"_q, u".ogg"_q, QString(), true);
			filemime = "audio/ogg";
		} else if (isRound) {
			filename = filedialogDefaultName(u"round"_q, u".mp4"_q, QString(), true);
			filemime = "video/mp4";
		} else {
			if (_information) {
				if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
						&_information->media)) {
					fullimage = base::take(image->data);
					fullimagebytes = base::take(image->bytes);
					fullimageformat = base::take(image->format);
				}
			}
			const auto mimeType = Core::MimeTypeForData(_content);
			filemime = mimeType.name();
			if (!Core::IsMimeSticker(filemime)
				&& fullimageformat != u"jpeg"_q) {
				fullimage = Images::Opaque(std::move(fullimage));
				fullimagebytes = fullimageformat = QByteArray();
			}
			if (filemime == "image/jpeg") {
				filename = filedialogDefaultName(u"photo"_q, u".jpg"_q, QString(), true);
			} else if (filemime == "image/png") {
				filename = filedialogDefaultName(u"image"_q, u".png"_q, QString(), true);
			} else {
				QString ext;
				QStringList patterns = mimeType.globPatterns();
				if (!patterns.isEmpty()) {
					ext = patterns.front().replace('*', QString());
				}
				filename = filedialogDefaultName(u"file"_q, ext, QString(), true);
			}
		}
	} else {
		if (_information) {
			if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
					&_information->media)) {
				fullimage = base::take(image->data);
				fullimagebytes = base::take(image->bytes);
				fullimageformat = base::take(image->format);
			}
		}
		if (!fullimage.isNull() && fullimage.width() > 0) {
			if (_type == SendMediaType::Photo) {
				if (ValidateThumbDimensions(fullimage.width(), fullimage.height())) {
					filesize = -1; // Fill later.
					filemime = Core::MimeTypeForName("image/jpeg").name();
					filename = filedialogDefaultName(u"image"_q, u".jpg"_q, QString(), true);
				} else {
					_type = SendMediaType::File;
				}
			}
			if (_type == SendMediaType::File) {
				filemime = Core::MimeTypeForName("image/png").name();
				filename = filedialogDefaultName(u"image"_q, u".png"_q, QString(), true);
				{
					QBuffer buffer(&_content);
					fullimage.save(&buffer, "PNG");
				}
				filesize = _content.size();
			}
			fullimage = Images::Opaque(std::move(fullimage));
			fullimagebytes = fullimageformat = QByteArray();
		}
	}
	_result->filesize = qMin(filesize, qint64(UINT_MAX));

	if (!filesize || filesize > kFileSizePremiumLimit) {
		return;
	}

	PreparedPhotoThumbs photoThumbs;
	QVector<MTPPhotoSize> photoSizes;
	QImage goodThumbnail;
	QByteArray goodThumbnailBytes;

	auto attributes = QVector<MTPDocumentAttribute>(
		1,
		MTP_documentAttributeFilename(MTP_string(_displayName.isEmpty()
			? filename
			: _displayName)));

	if (filename.endsWith(u".htm"_q, Qt::CaseInsensitive)) {
		attributes[0] = MTP_documentAttributeFilename(MTP_string(
			QString(filename).chopped(4) + u"[htm].xhtml"_q));
	} else if (filename.endsWith(u".html"_q, Qt::CaseInsensitive)) {
		attributes[0] = MTP_documentAttributeFilename(MTP_string(
			QString(filename).chopped(5) + u"[html].xhtml"_q));
	}

	auto thumbnail = PreparedFileThumbnail();

	auto photo = MTP_photoEmpty(MTP_long(0));
	auto document = MTP_documentEmpty(MTP_long(0));

	if (isRound) {
		_information = readMediaInformation(u"video/mp4"_q);
		if (auto video = std::get_if<Ui::PreparedFileInformation::Video>(
			&_information->media)) {
			isVideo = true;
			auto coverWidth = video->thumbnail.width();
			auto coverHeight = video->thumbnail.height();
			if (video->isGifv && !_album) {
				attributes.push_back(MTP_documentAttributeAnimated());
			}
			auto flags = MTPDdocumentAttributeVideo::Flags(
				MTPDdocumentAttributeVideo::Flag::f_round_message);
			if (video->supportsStreaming) {
				flags |= MTPDdocumentAttributeVideo::Flag::f_supports_streaming;
			}
			const auto realSeconds = std::max(
				video->duration / 1000.,
				0.);
			attributes.push_back(MTP_documentAttributeVideo(
				MTP_flags(flags),
				MTP_double(realSeconds),
				MTP_int(coverWidth),
				MTP_int(coverHeight),
				MTPint(), // preload_prefix_size
				MTPdouble(), // video_start_ts
				MTPstring())); // video_codec

			if (args.generateGoodThumbnail) {
				goodThumbnail = video->thumbnail;
				{
					QBuffer buffer(&goodThumbnailBytes);
					goodThumbnail.save(&buffer, "JPG", kThumbnailQuality);
				}
			}
			thumbnail = PrepareFileThumbnail(std::move(video->thumbnail));
		}
	} else if (!isVoice) {
		if (!_information) {
			_information = readMediaInformation(filemime);
			filemime = _information->filemime;
		}
		if (auto song = std::get_if<Ui::PreparedFileInformation::Song>(
				&_information->media)) {
			isSong = true;
			const auto seconds = song->duration / 1000;
			auto flags = MTPDdocumentAttributeAudio::Flag::f_title | MTPDdocumentAttributeAudio::Flag::f_performer;
			attributes.push_back(MTP_documentAttributeAudio(MTP_flags(flags), MTP_int(seconds), MTP_string(song->title), MTP_string(song->performer), MTPstring()));
			thumbnail = PrepareFileThumbnail(std::move(song->cover));
			if (thumbnail.image.isNull()
				&& _information
				&& !_information->fileThumbnail.isNull()) {
				thumbnail = PrepareFileThumbnail(QImage(_information->fileThumbnail));
			}
		} else if (auto video = std::get_if<Ui::PreparedFileInformation::Video>(
				&_information->media)) {
			isVideo = true;
			auto coverWidth = video->thumbnail.width();
			auto coverHeight = video->thumbnail.height();
			auto realSeconds = video->duration / 1000.;
			const auto gif = video->modifications.gif && !_forceFile;
			const auto convertForGif = gif
				&& video->isGifv
				&& (filemime != u"video/mp4"_q);
			const auto convert = (!Core::IsMimeSentAsVideo(filemime)
					|| convertForGif)
				&& !video->isWebmSticker
				&& (filesize < Media::Encode::MaxTranscodeSourceSize());
			if (!_forceFile
				&& !video->thumbnail.isNull()
				&& (convert
					|| Editor::VideoEdited(
						video->modifications,
						video->thumbnail.size(),
						video->duration,
						video->hasAudio))) {
				auto source = Editor::ComposeVideoSource(
					_filepath,
					video->modifications,
					{},
					false);
				source.bytes = _filepath.isEmpty() ? _content : QByteArray();
				const auto target = Media::Encode::TranscodedSize(
					source,
					video->thumbnail.size());
				if (!target.isEmpty()) {
					const auto duration = Media::Encode::TranscodedDuration(
						source,
						video->duration);
					coverWidth = target.width();
					coverHeight = target.height();
					realSeconds = duration / 1000.;
					video->thumbnail = Editor::ImageModified(
						base::take(video->thumbnail),
						video->modifications.geometry
					).scaled(
						target,
						Qt::IgnoreAspectRatio,
						Qt::SmoothTransformation);
					_result->videoCoverOffset = std::clamp(
						video->modifications.cover
							- video->modifications.from,
						crl::time(0),
						duration);
					_result->videoSource = std::make_shared<
						Media::Encode::VideoSource>(std::move(source));

					filemime = u"video/mp4"_q;
					filename = Mp4FileName(filename);
					if (!_displayName.isEmpty()) {
						_displayName = Mp4FileName(_displayName);
					}
					attributes[0] = MTP_documentAttributeFilename(
						MTP_string(_displayName.isEmpty()
							? filename
							: _displayName));
					video->supportsStreaming = true;
				}
			}
			if (!_forceFile && !_result->videoSource) {
				_result->videoCoverOffset = std::clamp(
					video->modifications.cover,
					crl::time(0),
					video->duration);
			}
			if (!_forceFile) {
				if (gif && !_album && (filemime == u"video/mp4"_q)) {
					attributes.push_back(MTP_documentAttributeAnimated());
				}
				auto flags = MTPDdocumentAttributeVideo::Flags(0);
				if (video->supportsStreaming) {
					flags |= MTPDdocumentAttributeVideo::Flag::f_supports_streaming;
				}
				if (gif) {
					flags |= MTPDdocumentAttributeVideo::Flag::f_nosound;
				}
				const auto startTs = _result->videoCoverOffset;
				if (startTs > 0) {
					using Flag = MTPDdocumentAttributeVideo::Flag;
					flags |= Flag::f_video_start_ts;
				}
				attributes.push_back(MTP_documentAttributeVideo(
					MTP_flags(flags),
					MTP_double(realSeconds),
					MTP_int(coverWidth),
					MTP_int(coverHeight),
					MTPint(),
					MTP_double(startTs / 1000.),
					MTPstring()));
				const auto lowerName = QString(filename).toLower();
				if (filename.endsWith(u".webm"_q, Qt::CaseInsensitive)) {
					attributes[0] = MTP_documentAttributeFilename(MTP_string(
						QString(filename).chopped(5) + u"[webm].mp4"_q));
				}
			}

			if (args.generateGoodThumbnail) {
				goodThumbnail = video->thumbnail;
				{
					QBuffer buffer(&goodThumbnailBytes);
					goodThumbnail.save(&buffer, "JPG", kThumbnailQuality);
				}
			}
			thumbnail = PrepareFileThumbnail(std::move(video->thumbnail));
		} else if (filemime == u"application/x-tdesktop-theme"_q
			|| filemime == u"application/x-tgtheme-tdesktop"_q) {
			goodThumbnail = Window::Theme::GeneratePreview(_content, _filepath);
			if (!goodThumbnail.isNull()) {
				QBuffer buffer(&goodThumbnailBytes);
				goodThumbnail.save(&buffer, "JPG", kThumbnailQuality);

				thumbnail = PrepareFileThumbnail(base::duplicate(goodThumbnail));
			}
		}
	}

	if (fullimage.isNull()
		&& _information
		&& !_information->fileThumbnail.isNull()) {
		fullimage = _information->fileThumbnail;
	}

	if (!fullimage.isNull() && fullimage.width() > 0 && !isSong && !isVideo && !isVoice && !isRound) {
		auto w = fullimage.width(), h = fullimage.height();
		attributes.push_back(MTP_documentAttributeImageSize(MTP_int(w), MTP_int(h)));

		if (ValidateThumbDimensions(w, h)) {
			isSticker = Core::IsMimeSticker(filemime)
				&& (filesize < Storage::kMaxStickerBytesSize)
				&& (Core::IsMimeStickerAnimated(filemime)
					|| (_type == SendMediaType::File
						&& GoodStickerDimensions(w, h)));
			if (isSticker) {
				attributes.push_back(MTP_documentAttributeSticker(
					MTP_flags(0),
					MTP_string(),
					MTP_inputStickerSetEmpty(),
					MTPMaskCoords()));
				if (isAnimation && args.generateGoodThumbnail) {
					goodThumbnail = fullimage;
					{
						QBuffer buffer(&goodThumbnailBytes);
						goodThumbnail.save(&buffer, "WEBP", kThumbnailQuality);
					}
				}
			} else if (isAnimation) {
				attributes.push_back(MTP_documentAttributeAnimated());
			} else if (filemime.startsWith(u"image/"_q)
				&& _type != SendMediaType::File) {
				if (Core::IsMimeSticker(filemime)) {
					fullimage = Images::Opaque(std::move(fullimage));
				}
				auto medium = (w > 320 || h > 320) ? fullimage.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation) : fullimage;

				const auto limit = PhotoSideLimit(_sendLargePhotos);
				const auto downscaled = (w > limit || h > limit);
				auto full = downscaled ? fullimage.scaled(limit, limit, Qt::KeepAspectRatio, Qt::SmoothTransformation) : fullimage;
				if (downscaled) {
					fullimagebytes = fullimageformat = QByteArray();
				}
				filedata = ComputePhotoJpegBytes(full, fullimagebytes, fullimageformat);

				photoThumbs.emplace('m', PreparedPhotoThumb{ .image = medium });
				photoSizes.push_back(MTP_photoSize(MTP_string("m"), MTP_int(medium.width()), MTP_int(medium.height()), MTP_int(0)));

				photoThumbs.emplace('y', PreparedPhotoThumb{
					.image = full,
					.bytes = filedata
				});
				photoSizes.push_back(MTP_photoSize(MTP_string("y"), MTP_int(full.width()), MTP_int(full.height()), MTP_int(0)));

				photo = MTP_photo(
					MTP_flags(0),
					MTP_long(_id),
					MTP_long(0),
					MTP_bytes(),
					MTP_int(base::unixtime::now()),
					MTP_vector<MTPPhotoSize>(photoSizes),
					MTPVector<MTPVideoSize>(),
					MTP_int(_dcId));

				if (filesize < 0) {
					filesize = _result->filesize = filedata.size();
				}
			}
			thumbnail = PrepareFileThumbnail(std::move(fullimage));
		}
	}
	thumbnail = FinalizeFileThumbnail(
		std::move(thumbnail),
		filemime,
		filesize,
		isSticker);

	if (_type == SendMediaType::Photo && photoThumbs.empty()) {
		_type = SendMediaType::File;
	}

	if (isVoice) {
		const auto seconds = _duration / 1000;
		auto flags = MTPDdocumentAttributeAudio::Flag::f_voice | MTPDdocumentAttributeAudio::Flag::f_waveform;
		attributes[0] = MTP_documentAttributeAudio(MTP_flags(flags), MTP_int(seconds), MTPstring(), MTPstring(), MTP_bytes(documentWaveformEncode5bit(_waveform)));
		attributes.resize(1);
		document = MTP_document(
			MTP_flags(0),
			MTP_long(_id),
			MTP_long(0),
			MTP_bytes(),
			MTP_int(base::unixtime::now()),
			MTP_string(filemime),
			MTP_long(filesize),
			MTP_vector<MTPPhotoSize>(1, thumbnail.mtpSize),
			MTPVector<MTPVideoSize>(),
			MTP_int(_dcId),
			MTP_vector<MTPDocumentAttribute>(attributes));
	} else if (_type != SendMediaType::Photo) {
		document = MTP_document(
			MTP_flags(0),
			MTP_long(_id),
			MTP_long(0),
			MTP_bytes(),
			MTP_int(base::unixtime::now()),
			MTP_string(filemime),
			MTP_long(filesize),
			MTP_vector<MTPPhotoSize>(1, thumbnail.mtpSize),
			MTPVector<MTPVideoSize>(),
			MTP_int(_dcId),
			MTP_vector<MTPDocumentAttribute>(attributes));
		_type = isRound ? SendMediaType::Round : SendMediaType::File;
	}

	if (_information) {
		if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
				&_information->media)) {
			if (image->modifications.paint) {
				const auto documents = ExtractStickersFromScene(image);
				_result->attachedStickers = documents
					| ranges::views::transform(&DocumentData::mtpInput)
					| ranges::to_vector;
			}
		}
	}

	_result->type = _type;
	_result->filepath = _filepath;
	_result->content = _content;

	_result->filename = filename;
	_result->filemime = filemime;
	_result->setFileData(filedata);

	_result->thumbId = thumbnail.id;
	_result->thumbname = thumbnail.name;
	_result->setThumbData(thumbnail.bytes);
	_result->thumb = std::move(thumbnail.image);

	_result->goodThumbnail = std::move(goodThumbnail);
	_result->goodThumbnailBytes = std::move(goodThumbnailBytes);

	_result->photo = photo;
	_result->document = document;
	_result->photoThumbs = photoThumbs;
	_result->forceFile = _forceFile;
}

void FileLoadTask::finish() {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	const auto premium = session->user()->isPremium();
	if (!_result || !_result->filesize || _result->filesize < 0) {
		Ui::show(
			Ui::MakeInformBox(
				tr::lng_send_image_empty(tr::now, lt_name, _filepath)),
			Ui::LayerOption::KeepOther);
		removeFromAlbum();
	} else if (_result->filesize > kFileSizePremiumLimit
		|| (_result->filesize > kFileSizeLimit && !premium)) {
		Ui::show(
			Box(FileSizeLimitBox, session, _result->filesize, nullptr),
			Ui::LayerOption::KeepOther);
		removeFromAlbum();
	} else if (_album && _album->preparedMusicBatching()) {
		const auto it = ranges::find(_album->items, id(), &SendingAlbum::Item::taskId);
		Assert(it != _album->items.end());

		it->prepared = _result;
		if (_album->preparedMusicReady()) {
			Api::SendConfirmedFile(session, _result);
		}
	} else {
		if (const auto &job = _result->animationJob) {
			_result->attachedStickers = job->attachedStickerIds
				| ranges::views::transform([&](uint64 id) {
					return session->data().document(id)->mtpInput();
				})
				| ranges::to_vector;
		}
		Api::SendConfirmedFile(session, _result);
		// Don't delete file here - uploader needs it. Delete after upload completes.
	}
}


const std::shared_ptr<FilePrepareResult> &FileLoadTask::peekResult() const {
	return _result;
}

std::unique_ptr<Ui::PreparedFileInformation> FileLoadTask::readMediaInformation(
		const QString &filemime) const {
	return ReadMediaInformation(_filepath, _content, filemime);
}

void FileLoadTask::removeFromAlbum() {
	if (!_album) {
		return;
	}
	const auto session = _session.get();
	_album->removeTask(id());
	if (session && _album->preparedMusicReady()) {
		if (const auto sample = _album->preparedMusicSample()) {
			Api::SendConfirmedFile(session, sample);
		}
	}
}
