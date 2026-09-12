/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_file_click_handler.h"

#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_download_manager.h"
#include "enhanced_forward.h"
#include "storage/file_upload.h"
#include "data/data_photo.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "ui/toast/toast.h"
#include "history/history_item.h"
#include "data/data_forum_topic.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "base/weak_ptr.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "styles/style_layers.h"
#include "rpl/rpl.h"

FileClickHandler::FileClickHandler(FullMsgId context)
: _context(context) {
}

void FileClickHandler::setMessageId(FullMsgId context) {
	_context = context;
}

FullMsgId FileClickHandler::context() const {
	return _context;
}

not_null<DocumentData*> DocumentClickHandler::document() const {
	return _document;
}

DocumentWrappedClickHandler::DocumentWrappedClickHandler(
	ClickHandlerPtr wrapped,
	not_null<DocumentData*> document,
	FullMsgId context)
: DocumentClickHandler(document, context)
, _wrapped(wrapped) {
}

void DocumentWrappedClickHandler::onClickImpl() const {
	_wrapped->onClick({ Qt::LeftButton });
}

DocumentClickHandler::DocumentClickHandler(
	not_null<DocumentData*> document,
	FullMsgId context)
: FileClickHandler(context)
, _document(document) {
	setProperty(
		kDocumentLinkMediaProperty,
		reinterpret_cast<qulonglong>(_document.get()));
}

QString DocumentClickHandler::tooltip() const {
	return property(kDocumentFilenameTooltipProperty).value<QString>();
}

DocumentOpenClickHandler::DocumentOpenClickHandler(
	not_null<DocumentData*> document,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: DocumentClickHandler(document, context)
, _handler(std::move(callback)) {
	Expects(_handler != nullptr);
}

void DocumentOpenClickHandler::onClickImpl() const {
	_handler(context());
}

static void ShowDownloadBatchDone(int downloaded, int duplicates) {
	if (downloaded <= 0 && duplicates <= 0) {
		return;
	}
	if (downloaded > 0 && duplicates > 0) {
		const auto downloadedText = tr::lng_tm_dl_done(tr::now, lt_count, downloaded);
		const auto duplicatesText = tr::lng_tm_dl_duplicates_skipped(tr::now, lt_count, duplicates);
		Ui::Toast::Show(downloadedText + u", "_q + duplicatesText);
	} else if (downloaded > 0) {
		Ui::Toast::Show(tr::lng_tm_dl_done(tr::now, lt_count, downloaded));
	} else {
		Ui::Toast::Show(tr::lng_tm_dl_duplicates_skipped(tr::now, lt_count, duplicates));
	}
}

std::shared_ptr<DownloadBatch> MakeDownloadBatch(int total) {
	auto batch = std::make_shared<DownloadBatch>();
	batch->total = total;
	batch->onDone = [](int downloaded, int duplicates) {
		ShowDownloadBatchDone(downloaded, duplicates);
	};
	return batch;
}

void DocumentSaveClickHandler::Save(
		Data::FileOrigin origin,
		not_null<DocumentData*> data,
		Mode mode,
		Fn<void()> started,
		PeerData *peer,
		const QString &topicName,
		std::shared_ptr<DownloadBatch> batch) {
	if (data->isNull()) {
		if (batch) {
			batch->addFailed();
		}
		return;
	}

	if (mode == Mode::ToCacheOrFile && data->saveToCache()) {
		data->save(origin, QString());
		if (batch) {
			batch->addDownloaded();
		}
		return;
	}
	if (!batch) {
		batch = MakeDownloadBatch(1);
	}
	InvokeQueued(qApp, crl::guard(&data->session(), [=] {
		// If we call file dialog synchronously, it will stop
		// background thread timers from working which would
		// stop audio playback in voice chats / live streams.
		if (mode != Mode::ToNewFile && data->saveFromData()) {
			if (started) {
				started();
			}
			if (batch) {
				batch->addDownloaded();
			}
			return;
		}
		const auto filepath = data->filepath(true);
		const auto fileinfo = QFileInfo(filepath);
		const auto filedir = filepath.isEmpty()
			? QDir()
			: fileinfo.dir();
		const auto filename = filepath.isEmpty()
			? QString()
			: fileinfo.fileName();
		const auto savename = DocumentFileNameForSave(
			data,
			(mode == Mode::ToNewFile),
			filename,
			filedir,
			peer,
			topicName);
		if (savename.isEmpty()) {
			if (batch) {
				batch->addFailed();
			}
			return;
		}
		const auto proceed = [=] {
			// Force the plain file loader: saves to disk must resume reliably
			// after a pause. The streaming-reader downloader is only required
			// for feeding in-progress playback, which uses its own loaders.
			data->save(origin, savename, LoadFromCloudOrLocal, false, true);
			if (started) {
				started();
			}
			auto lifetime = std::make_shared<rpl::lifetime>();
			data->session().data().documentLoadProgress(
			) | rpl::filter([=](not_null<DocumentData*> doc) {
				return (doc == data);
			}) | rpl::on_next([=](not_null<DocumentData*> doc) {
				if (doc->loading()) {
					return;
				}
				if (!doc->filepath(true).isEmpty()) {
					if (batch) {
						batch->addDownloaded();
					}
				} else if (batch) {
					batch->addFailed();
				}
				lifetime->destroy();
			}, *lifetime);
		};
		Core::App().downloadManager().checkDuplicate(
			&data->session(),
			data,
			[=](bool skip) {
				if (skip) {
					if (batch) {
						batch->addDuplicate();
					} else {
						Core::App().downloadManager().reportDuplicateSkipped(
							Data::DedupDb::Table::Downloads);
					}
					return;
				}
				proceed();
			});
	}));
}

void DocumentSaveClickHandler::SaveAndTrack(
		FullMsgId itemId,
		not_null<DocumentData*> document,
		Mode mode,
		Fn<void()> started,
		std::shared_ptr<DownloadBatch> batch) {
	const auto item = document->owner().message(itemId);
	const auto peer = item ? item->history()->peer.get() : nullptr;
	auto topicName = QString();
	if (item && peer && peer->isForum()) {
		const auto rootId = item->topicRootId();
		if (rootId != Data::ForumTopic::kGeneralId) {
			const auto topic = peer->forumTopicFor(rootId);
			if (topic) {
				topicName = topic->title();
			}
		}
	}
	Save(itemId ? itemId : Data::FileOrigin(), document, mode, [=] {
		if (document->loading() && !document->loadingFilePath().isEmpty()) {
			if (const auto item = document->owner().message(itemId)) {
				Core::App().downloadManager().addLoading({
					.item = item,
					.document = document,
				});
			}
		}
		if (started) {
			started();
		}
	}, peer, topicName, batch);
}

void DocumentSaveClickHandler::onClickImpl() const {
	SaveAndTrack(context(), document());
}

DocumentCancelClickHandler::DocumentCancelClickHandler(
	not_null<DocumentData*> document,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: DocumentClickHandler(document, context)
, _handler(std::move(callback)) {
}

void DocumentCancelClickHandler::onClickImpl() const {
	const auto data = document();
	if (data->isNull()) {
		return;
	}
	if (const auto item = data->owner().message(context())) {
		if (EnhancedForward::isEnhancedForwardItem(
				&item->history()->session(),
				item)) {
			const auto weak = base::make_weak(&item->history()->session());
			const auto id = item->fullId();
			auto box = Box([=](not_null<Ui::GenericBox*> box) {
				box->addRow(object_ptr<Ui::FlatLabel>(
					box.get(),
					tr::lng_tm_fw_cancel_confirm(tr::now),
					st::boxLabel));
				box->setStyle(st::defaultBox);
				box->addButton(tr::lng_box_yes(), [=] {
					box->closeBox();
					if (const auto strong = weak.get()) {
						if (const auto live = strong->data().message(id)) {
							EnhancedForward::cancelItemByMessage(strong, live);
						}
					}
				}, st::attentionBoxButton);
				box->addButton(tr::lng_box_no(), [=] {
					box->closeBox();
				});
			});
			if (const auto window = Core::App().windowFor(
					not_null(&item->history()->session().account()))) {
				window->show(std::move(box));
			}
			return;
		}
	}
	if (data->uploading()) {
		if (_handler) {
			_handler(context());
		} else if (const auto item = data->owner().message(context())) {
			// Ask before cancelling an in-progress upload, the same way the
			// download branch below and the transfer manager context menu do,
			// so an accidental tap on the X doesn't silently drop it.
			const auto &session = item->history()->session();
			const auto window = Core::App().windowFor(
				not_null(&session.account()));
			if (!window) {
				session.uploader().cancel(item->fullId());
				return;
			}
			const auto weak = base::make_weak(&session);
			const auto id = item->fullId();
			auto box = Box([=](not_null<Ui::GenericBox*> box) {
				box->addRow(object_ptr<Ui::FlatLabel>(
					box.get(),
					tr::lng_tm_ul_cancel_confirm(tr::now),
					st::boxLabel));
				box->setStyle(st::defaultBox);
				box->addButton(tr::lng_box_yes(), [=] {
					box->closeBox();
					if (const auto strong = weak.get()) {
						strong->uploader().cancel(id);
					}
				}, st::attentionBoxButton);
				box->addButton(tr::lng_box_no(), [=] {
					box->closeBox();
				});
			});
			window->show(std::move(box));
		} else {
			data->owner().session().uploader().cancel(context());
		}
	} else if (const auto item = data->owner().message(context())) {
		// Ask before cancelling an in-progress download, so an accidental
		// tap on the X (the progress circle in the chat bubble) doesn't
		// silently drop it.
		Core::App().downloadManager().cancelWithConfirmation(item);
	} else {
		data->cancel();
	}
}

void DocumentOpenWithClickHandler::Open(
		Data::FileOrigin origin,
		not_null<DocumentData*> data) {
	if (data->isNull()) {
		return;
	}

	data->saveFromDataSilent();
	const auto path = data->filepath(true);
	if (!path.isEmpty()) {
		File::OpenWith(path);
	} else {
		DocumentSaveClickHandler::Save(
			origin,
			data,
			DocumentSaveClickHandler::Mode::ToFile);
	}
}

void DocumentOpenWithClickHandler::onClickImpl() const {
	Open(context(), document());
}

PhotoClickHandler::PhotoClickHandler(
	not_null<PhotoData*> photo,
	FullMsgId context,
	PeerData *peer)
: FileClickHandler(context)
, _photo(photo)
, _peer(peer) {
	setProperty(
		kPhotoLinkMediaProperty,
		reinterpret_cast<qulonglong>(_photo.get()));
}

not_null<PhotoData*> PhotoClickHandler::photo() const {
	return _photo;
}

PeerData *PhotoClickHandler::peer() const {
	return _peer;
}

PhotoOpenClickHandler::PhotoOpenClickHandler(
	not_null<PhotoData*> photo,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: PhotoClickHandler(photo, context)
, _handler(std::move(callback)) {
	Expects(_handler != nullptr);
}

void PhotoOpenClickHandler::onClickImpl() const {
	_handler(context());
}

void PhotoSaveClickHandler::onClickImpl() const {
	const auto data = photo();
	if (data->isNull()) {
		return;
	} else {
		data->clearFailed(Data::PhotoSize::Large);
		data->load(context());
	}
}

PhotoCancelClickHandler::PhotoCancelClickHandler(
	not_null<PhotoData*> photo,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: PhotoClickHandler(photo, context)
, _handler(std::move(callback)) {
}

void PhotoCancelClickHandler::onClickImpl() const {
	const auto data = photo();
	if (data->isNull()) {
		return;
	} else if (data->uploading() && _handler) {
		_handler(context());
	} else {
		data->cancel();
	}
}
