/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include "base/base_file_utilities.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_download_manager.h"
#include "data/data_file_click_handler.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h" // HistoryView::SelectedItem.
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QFile>

namespace Menu {
namespace {

using Documents = std::vector<std::pair<not_null<DocumentData*>, FullMsgId>>;
using Photos = std::vector<std::pair<not_null<PhotoData*>, FullMsgId>>;

[[nodiscard]] bool Added(
		HistoryItem *item,
		Documents &documents,
		Photos &photos) {
	if (item && !item->forbidsForward()) {
		if (const auto media = item->media()) {
			if (const auto photo = media->photo()) {
				photos.emplace_back(photo, item->fullId());
				return true;
			} else if (const auto document = media->document()) {
				documents.emplace_back(document, item->fullId());
				return true;
			}
		}
	}
	return false;
}

void AddAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		Documents &&documents,
		Photos &&photos,
		Fn<void()> callback) {
	const auto text = documents.empty()
		? tr::lng_context_save_images_selected(tr::now)
		: tr::lng_context_save_documents_selected(tr::now);
	const auto icon = documents.empty()
		? &st::menuIconSaveImage
		: &st::menuIconDownload;
	const auto shouldShowToast = documents.empty();

	const auto weak = base::make_weak(controller);
	const auto saveImages = [=](const QString &folderPath) {
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		const auto downloadPath = folderPath.isEmpty()
			? Core::App().settings().downloadPath()
			: folderPath;
		const auto path = downloadPath.isEmpty()
			? File::DefaultDownloadPath(session)
			: (downloadPath == FileDialog::Tmp())
			? session->local().tempDirectory()
			: downloadPath;
		if (path.isEmpty()) {
			return;
		}
		auto basePath = path;
		if (!basePath.endsWith('/')) {
			basePath += '/';
		}
		QDir().mkpath(basePath);

		const auto showToast = !shouldShowToast
			? Fn<void(const QString &)>(nullptr)
			: [=](const QString &lastPath) {
				const auto filter = [lastPath](const auto ...) {
					File::ShowInFolder(lastPath);
					return false;
				};
				controller->showToast({
					.text = (photos.size() > 1
							? tr::lng_mediaview_saved_images_to
							: tr::lng_mediaview_saved_to)(
						tr::now,
						lt_downloads,
						tr::link(
							tr::lng_mediaview_downloads(tr::now),
							"internal:show_saved_message"),
						tr::marked),
					.filter = filter,
					.iconLottie = u"toast/save_to_gallery"_q,
					.iconLottieSize = st::toastLottieIconSize,
					.st = &st::defaultToast,
				});
			};

		auto views = std::vector<std::shared_ptr<Data::PhotoMedia>>();
		auto dates = std::vector<TimeId>();
		for (const auto &[photo, fullId] : photos) {
			if (const auto view = photo->createMediaView()) {
				view->wanted(Data::PhotoSize::Large, fullId);
				views.push_back(view);
				const auto photoDate = photo->date();
				const auto item = session->data().message(fullId);
				dates.push_back(photoDate
					? photoDate
					: (item ? item->date() : TimeId(0)));
			}
		}

		const auto finalCheck = [=] {
			for (const auto &[photo, _] : photos) {
				if (photo->loading()) {
					return false;
				}
			}
			return true;
		};

		const auto photoPeers = [&] {
			auto result = std::vector<PeerData*>();
			result.reserve(photos.size());
			for (const auto &[photo, fullId] : photos) {
				const auto item = session->data().message(fullId);
				result.push_back(item ? item->history()->peer.get() : nullptr);
			}
			return result;
		}();
		const auto photoTopicNames = [&] {
			auto result = std::vector<QString>(photos.size());
			for (auto i = 0; i < photos.size(); i++) {
				const auto &fullId = photos[i].second;
				const auto item = session->data().message(fullId);
				if (!item) continue;
				const auto peer = item->history()->peer.get();
				if (!peer->isForum()) continue;
				const auto rootId = item->topicRootId();
				if (rootId == Data::ForumTopic::kGeneralId) continue;
				const auto topic = peer->forumTopicFor(rootId);
				if (topic) {
					result[i] = topic->title();
				}
			}
			return result;
		}();
		const auto saveToFiles = [=] {
			const auto fullPath = [&](int i) {
				auto dir = basePath;
				if (folderPath.isEmpty()) {
					const auto peer = (i < int(photoPeers.size()))
						? photoPeers[i]
						: nullptr;
					const auto subfolder = DownloadSubfolderForPhoto(
						peer,
						(i < int(photoTopicNames.size()))
							? photoTopicNames[i]
							: QString());
					if (!subfolder.isEmpty()) {
						dir += subfolder + '/';
						QDir().mkpath(dir);
					}
				}
				return filedialogDefaultName(
					u"photo_"_q + QString::number(i + 1),
					u".jpg"_q,
					dir);
			};
			auto lastPath = QString();
			auto skippedCount = 0;
			for (auto i = 0; i < views.size(); i++) {
				lastPath = fullPath(i);
				views[i]->saveToFile(lastPath);
				const auto &photo = photos[i].first;
				const auto fullId = photos[i].second;
				const auto item = session->data().message(fullId);
				if (item) {
					auto &manager = Core::App().downloadManager();
					manager.addLoaded({
						.item = item,
						.photo = photo,
					}, lastPath, manager.computeNextStartDate());
				}
				if (dates[i] > 0) {
					auto f = QFile(lastPath);
					if (f.open(QIODevice::ReadWrite)) {
						const auto when = base::unixtime::parse(dates[i]);
						f.setFileTime(
							when,
							QFileDevice::FileModificationTime);
						f.setFileTime(
							when,
							QFileDevice::FileAccessTime);
					}
				}
				if (!QFile::exists(lastPath)) {
					skippedCount++;
				}
			}
			if (showToast) {
				if (skippedCount > 0) {
					controller->showToast(
						tr::lng_tm_dl_duplicates_skipped(
							tr::now,
							lt_count,
							skippedCount));
				} else {
					showToast(lastPath);
				}
			}
		};

		if (finalCheck()) {
			saveToFiles();
		} else {
			auto lifetime = std::make_shared<rpl::lifetime>();
			session->downloaderTaskFinished(
			) | rpl::on_next([=]() mutable {
				if (finalCheck()) {
					saveToFiles();
					base::take(lifetime)->destroy();
				}
			}, *lifetime);
		}
	};
	const auto saveDocuments = [=](const QString &folderPath) {
		const auto batch = folderPath.isEmpty() && !documents.empty()
			? MakeDownloadBatch(int(documents.size()))
			: nullptr;
		for (const auto &[document, origin] : documents) {
			if (!folderPath.isEmpty()) {
				const auto name =
					base::FileNameFromUserString(document->filename());
				document->save(
					origin,
					folderPath + name,
					LoadFromCloudOrLocal,
					false,
					true);
			} else {
				DocumentSaveClickHandler::SaveAndTrack(origin, document, DocumentSaveClickHandler::Mode::ToCacheOrFile, nullptr, batch);
			}
		}
	};

	menu->addAction(text, [=] {
		const auto save = [=](const QString &folderPath) {
			saveImages(folderPath);
			saveDocuments(folderPath);
			callback();
		};
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		if (Core::App().settings().askDownloadPath()) {
			const auto initialPath = [] {
				const auto path = Core::App().settings().downloadPath();
				if (!path.isEmpty() && path != FileDialog::Tmp()) {
					return path.left(path.size()
						- (path.endsWith('/') ? 1 : 0));
				}
				return QString();
			}();
			const auto handleFolder = [=](const QString &result) {
				if (!result.isEmpty()) {
					const auto folderPath = result.endsWith('/')
						? result
						: (result + '/');
					save(folderPath);
				}
			};
			FileDialog::GetFolder(
				controller->window().widget().get(),
				tr::lng_download_path_choose(tr::now),
				initialPath,
				handleFolder);
		} else {
			save(QString());
		}
	}, icon);
}

} // namespace

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<HistoryView::SelectedItem> &selectedItems,
		not_null<HistoryView::ListWidget*> list) {
	if (selectedItems.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &selectedItem : selectedItems) {
		const auto &id = selectedItem.msgId;
		const auto item = window->session().data().message(id);

		if (!Added(item, docs, photos)) {
			return;
		}
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->cancelSelection();
		}
	};
	AddAction(menu, window, std::move(docs), std::move(photos), done);
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<HistoryInner*> list) {
	if (items.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &item : items) {
		if (!Added(item, docs, photos)) {
			return;
		}
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->clearSelected();
		}
	};
	AddAction(menu, window, std::move(docs), std::move(photos), done);
}

} // namespace Menu
