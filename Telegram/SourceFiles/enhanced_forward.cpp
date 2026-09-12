/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "enhanced_forward.h"

#include "apiwrap.h"
#include "rpl/rpl.h"
#include "base/debug_log.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include <QDir>
#include <QFile>
#include <deque>
#include <unordered_set>
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

// HIGH-LEVEL SENDING AND ITEM RESOLUTION HELPERS:
#include "api/api_sending.h"
#include "api/api_text_entities.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "main/main_session_settings.h"
#include "main/main_account.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "api/api_media.h"
#include "ui/image/image_prepare.h"
#include "lang/lang_keys.h"
#include "core/file_utilities.h"
#include "ui/text/text_utilities.h"
#include "logs.h"
#include "base/random.h"
#include <crl/crl.h>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

// ADDITIONAL INTERNAL DATA TYPES REQUIRED FOR THE STAGE PIPELINE:
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/business/data_shortcut_messages.h"
#include "ui/chat/attach/attach_prepare.h"
#include "storage/storage_account.h"
#include "core/application.h"
#include "data/data_download_manager.h"
#include "info/downloads/info_downloads_widget.h"
#include "data/data_file_hash.h"
#include "data/data_forum_topic.h"
#include "ui/toast/toast.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_controller.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "settings.h"

#include <algorithm>

namespace EnhancedForward {
namespace {

struct SharedState {
	int total = 0;
	int sent = 0;
	int skipped = 0;
	bool cancelled = false;
	bool paused = false;
	bool finished = false;
	bool resumable = false; // seeded from a persisted unfinished DB row
	PeerId destPeer;
	PeerId srcPeer;
	Fn<void()> cancelCallback;
	Fn<void()> pauseCallback;
	Fn<void()> resumeCallback;
	Fn<void()> saveCallback;
	int currentDownload = -1;
	std::vector<FullMsgId> sourceIds;
	std::vector<TrackedItem> items;
	int currentUpload = -1;
	ItemInfo downloadItem;
	ItemInfo uploadItem;
	float64 downloadProgress = 0;
	float64 uploadProgress = 0;
	qint64 downloadSpeed = 0;
	qint64 uploadSpeed = 0;
};

class EnhancedFileTask final : public Task {
public:
	EnhancedFileTask(
		FileLoadTask::Args &&args,
		Fn<void(std::shared_ptr<FilePrepareResult>)> &&cb)
	: _session(base::make_weak(args.session))
	, _impl(std::make_unique<FileLoadTask>(std::move(args)))
	, _cb(std::move(cb)) {}
	void process() override { _impl->process(); }
	void finish() override {
		if (!_session) {
			return;
		}
		if (_cb) _cb(_impl->peekResult());
	}
private:
	base::weak_ptr<Main::Session> _session;
	std::unique_ptr<FileLoadTask> _impl;
	Fn<void(std::shared_ptr<FilePrepareResult>)> _cb;
};

using StateMap = std::unordered_map<PeerId, SharedState>;
using FinishedStateList = std::vector<SharedState>;

rpl::event_stream<PeerId> StateChanges;
rpl::event_stream<> CounterChanges;

void NotifyCounterChanged() {
	CounterChanges.fire({});
}

void NotifyStateChanged(const PeerId &peer) {
	StateChanges.fire_copy(peer);
}

std::unordered_set<FullMsgId> &EFUploadIds() {
	static auto set = std::unordered_set<FullMsgId>();
	return set;
}

void RemoveEFUploadsForPeer(const PeerId &peer) {
	auto &set = EFUploadIds();
	for (auto it = set.begin(); it != set.end();) {
		if (it->peer == peer) {
			it = set.erase(it);
		} else {
			++it;
		}
	}
}

void EnsureEFUploadIdSync(not_null<Main::Session*> session) {
	static base::flat_set<not_null<Main::Session*>> synced;
	if (!synced.emplace(session).second) {
		return;
	}
	session->lifetime().add([session] {
		synced.remove(session);
	});
	// EF uploads are tracked by their local (negative) message id, but the
	// uploader switches its finished list to the server id once the message
	// is confirmed. Keep EFUploadIds in sync so the transfer manager keeps
	// excluding enhanced-forward uploads from the regular Uploads tab even
	// after the pipeline itself is gone.
	session->data().itemIdChanged() | rpl::on_next([session](
			const Data::Session::IdChange &change) {
		auto &ids = EFUploadIds();
		const auto oldFull = FullMsgId(change.newId.peer, change.oldId);
		if (ids.contains(oldFull)) {
			ids.erase(oldFull);
			ids.emplace(change.newId);
		}
	}, session->lifetime());
}

void SetShadowUpload(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		int64 size) {
	const auto media = item->media();
	const auto doc = media ? media->document() : nullptr;
	if (!doc) {
		return;
	}
	doc->uploadingData = std::make_unique<Data::UploadState>(size);
	session->data().requestItemRepaint(item);
}

void UpdateShadowUpload(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		int64 offset,
		int64 size) {
	const auto media = item->media();
	const auto doc = media ? media->document() : nullptr;
	if (!doc || !doc->uploadingData) {
		return;
	}
	doc->uploadingData->offset = std::clamp(offset, int64(0), size);
	session->data().requestItemRepaint(item);
	session->data().notifyDocumentLayoutChanged(doc);
}

void ClearShadowUpload(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto doc = media ? media->document() : nullptr;
	if (!doc || !doc->uploadingData) {
		return;
	}
	doc->uploadingData = nullptr;
	session->data().requestItemRepaint(item);
}

std::pair<int, int> &LastBatchCountsCache() {
	static auto value = std::pair<int, int>{ 0, 0 };
	return value;
}

void SaveLastBatchCounts(int done, int total) {
	LastBatchCountsCache() = { done, total };
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.saveLastBatchCounts(done, total);
	}
}

} // namespace

std::pair<int, int> LastBatchCounts() {
	return LastBatchCountsCache();
}

void ShowForwardDoneToast(int sent, int, int skipped) {
	if (sent <= 0 && skipped <= 0) {
		return;
	}
	if (sent > 0 && skipped > 0) {
		const auto forwardedText = tr::lng_tm_fw_forwarded(tr::now, lt_count, sent);
		const auto duplicatesText = tr::lng_tm_fw_duplicates_skipped(tr::now, lt_count, skipped);
		Ui::Toast::Show(forwardedText + u", "_q + duplicatesText);
	} else if (sent > 0) {
		Ui::Toast::Show(tr::lng_tm_fw_forwarded(tr::now, lt_count, sent));
	} else {
		Ui::Toast::Show(tr::lng_tm_fw_duplicates_skipped(tr::now, lt_count, skipped));
	}
}

StateMap &ActiveStates() {
	static StateMap map;
	return map;
}

// Finished/resumable batches accumulate: re-forwarding to the same chat adds
// a new batch instead of replacing the previous one, so the Forwards tab keeps
// every sent item until the user clears it.
FinishedStateList &FinishedStates() {
	static FinishedStateList list;
	return list;
}

const SharedState *FindFinishedBatch(const PeerId &peer) {
	for (const auto &state : FinishedStates()) {
		if (state.destPeer == peer) {
			return &state;
		}
	}
	return nullptr;
}

bool HasFinishedForPeer(const PeerId &peer) {
	return FindFinishedBatch(peer) != nullptr;
}

void EraseResumableBatchesForPeer(const PeerId &peer) {
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (i->destPeer == peer && i->resumable) {
			i = finished.erase(i);
		} else {
			++i;
		}
	}
}

void processStartQueue(const PeerId &peerId);

void finishJob(not_null<Main::Session*> session, const PeerId &peerId) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;
	auto &state = it->second;
	state.finished = true;
	// Release the pipeline references so finished jobs don't keep the
	// whole Pipeline (and its per-item tasks) alive in FinishedStates.
	state.cancelCallback = nullptr;
	state.pauseCallback = nullptr;
	state.resumeCallback = nullptr;
	state.saveCallback = nullptr;
	const auto allDead = ranges::all_of(
		state.items,
		[](const TrackedItem &tracked) {
			return tracked.cancelled || tracked.dedupSkipped;
		});
	if (allDead) {
		states.erase(it);		
		CleanupPartialFilesForPeer(session, peerId);
		SaveLastBatchCounts(0, 0);
		NotifyStateChanged(peerId);
		NotifyCounterChanged();
		session->changes().peerUpdated(
			session->data().peer(peerId),
			Data::PeerUpdate::Flag::Slowmode);
		processStartQueue(peerId);
		return;
	}
	auto &finished = FinishedStates();
	auto &batch = it->second;
	finished.push_back(std::move(batch));
	states.erase(it);
	// Keep the counter visible with the last completed batch's counts until
	// the next forward replaces it. A cancelled batch clears the counter.
	SaveLastBatchCounts(
		finished.back().cancelled ? 0 : finished.back().sent,
		finished.back().cancelled ? 0 : finished.back().total);
	NotifyStateChanged(peerId);
	NotifyCounterChanged();
	session->changes().peerUpdated(
		session->data().peer(peerId),
		Data::PeerUpdate::Flag::Slowmode);
	processStartQueue(peerId);
}

struct StartRequest {
	not_null<ApiWrap*> api;
	std::vector<not_null<HistoryItem*>> items;
	Api::SendAction action;
	Data::ForwardOptions forwardOptions;
	Data::GroupingOptions groupOptions;
	std::shared_ptr<SavedJob> resumeJob;
};

using StartQueueMap = std::unordered_map<PeerId, std::deque<StartRequest>>;

StartQueueMap &StartQueue() {
	static StartQueueMap map;
	return map;
}

void processStartQueue(const PeerId &peerId) {
	auto &queue = StartQueue();
	const auto it = queue.find(peerId);
	if (it == queue.end() || it->second.empty()) {
		return;
	}
	if (ActiveStates().find(peerId) != ActiveStates().end()) {
		return;
	}
	auto request = std::move(it->second.front());
	it->second.pop_front();
	if (it->second.empty()) {
		queue.erase(it);
	}
	Pipeline::Start(
		request.api,
		std::move(request.items),
		request.action,
		request.forwardOptions,
		request.groupOptions,
		std::move(request.resumeJob));
}

void fireUpdate(not_null<Main::Session*> session, const PeerId &peer) {
	session->changes().peerUpdated(
		session->data().peer(peer),
		Data::PeerUpdate::Flag::Slowmode);
	NotifyStateChanged(peer);
}

base::flat_set<not_null<PeerData*>> &FlagProtectedPeers() {
	static base::flat_set<not_null<PeerData*>> result;
	return result;
}

base::flat_set<not_null<PeerData*>> &ErrorProtectedPeers() {
	static base::flat_set<not_null<PeerData*>> result;
	return result;
}

void NotePeerNoforwardsFlag(not_null<PeerData*> peer, bool value) {
	if (value) {
		FlagProtectedPeers().insert(peer);
	} else {
		FlagProtectedPeers().erase(peer);
	}
}

void MarkPeerProtectedByError(not_null<PeerData*> peer) {
	ErrorProtectedPeers().insert(peer);
}

bool PeerNeedsEnhancedForward(not_null<PeerData*> peer) {
	return FlagProtectedPeers().contains(peer)
		|| ErrorProtectedPeers().contains(peer);
}

Split classifyItems(const std::vector<not_null<HistoryItem*>> &items) {
	auto result = Split();
	for (const auto &item : items) {
		if (PeerNeedsEnhancedForward(item->history()->peer)) {
			result.restricted.push_back(item);
		} else {
			result.normal.push_back(item);
		}
	}
	return result;
}

void startForwardSession(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		const PeerId &srcPeer,
		const std::vector<FullMsgId> &sourceIds,
		Fn<void()> saveCallback) {
	auto &states = ActiveStates();
	states.erase(peerId);
	// Keep finished batches in the tab; only stale resumable leftovers of
	// this destination are dropped (they are being resumed right now).
	EraseResumableBatchesForPeer(peerId);

	auto &state = states[peerId];
	state.total = int(sourceIds.size());
	state.sent = 0;
	state.skipped = 0;
	state.cancelled = false;
	state.paused = false;
	state.finished = false;
	state.destPeer = peerId;
	state.srcPeer = srcPeer;
	state.sourceIds = sourceIds;
	state.items.resize(state.total);
	state.saveCallback = std::move(saveCallback);

	fireUpdate(session, peerId);
	NotifyCounterChanged();
}

void markItemSent(
		not_null<Main::Session*> session,
		const PeerId &peerId) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (state.cancelled || state.finished) return;

	state.sent++;
	fireUpdate(session, peerId);
	NotifyCounterChanged();
	if (state.saveCallback) {
		state.saveCallback();
	}

	if (state.sent >= state.total) {
		finishJob(session, peerId);
	}
}

void markItemSkipped(
		not_null<Main::Session*> session,
		const PeerId &peerId) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (state.cancelled || state.finished) return;

	if (state.total > 0) state.total--;
	state.skipped++;
	fireUpdate(session, peerId);
	NotifyCounterChanged();
	if (state.saveCallback) {
		state.saveCallback();
	}

	if (state.sent >= state.total) {
		finishJob(session, peerId);
	}
}

void cancelForward(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;
	auto &state = it->second;
	if (state.cancelled || state.finished) return;

	if (state.sent == 0) {
		state.cancelled = true;
		if (state.cancelCallback) {
			state.cancelCallback();
		}
		finishJob(session, id);
		return;
	}
	// Partial cancel: items that were already forwarded stay visible as a
	// finished batch; only the remaining items are cancelled and dropped.
	const auto pipeline = Pipeline::Active().find(id);
	const auto live = (pipeline != Pipeline::Active().end())
		? pipeline->second.lock()
		: nullptr;
	if (live) {
		for (auto i = 0; i < int(state.items.size()); i++) {
			if (!state.items[i].sent && !state.items[i].cancelled) {
				live->cancelItem(i);
			}
		}
	} else if (state.cancelCallback) {
		state.cancelCallback();
	}
	auto sent = 0;
	for (auto &item : state.items) {
		if (item.sent || item.state == ItemState::Done) {
			sent++;
		} else {
			item.cancelled = true;
		}
	}
	state.skipped = 0;
	state.sent = sent;
	state.total = sent;
	state.cancelled = false;
	finishJob(session, id);
}

void setCancelCallback(
		const PeerId &id,
		not_null<Main::Session*> session,
		Fn<void()> callback) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.cancelCallback = std::move(callback);
}

void setPauseCallback(
		const PeerId &id,
		not_null<Main::Session*> session,
		Fn<void()> callback) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.pauseCallback = std::move(callback);
}

void setResumeCallback(
		const PeerId &id,
		not_null<Main::Session*> session,
		Fn<void()> callback) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.resumeCallback = std::move(callback);
}

void pauseForward(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.paused = true;
	if (it->second.pauseCallback) {
		it->second.pauseCallback();
	}
	fireUpdate(session, id);
}

void resumeForward(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.paused = false;
	if (it->second.resumeCallback) {
		it->second.resumeCallback();
	}
	fireUpdate(session, id);
}

void cancelCurrentItem(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	auto &state = it->second;
	state.currentDownload = -1;
	state.currentUpload = -1;
	state.downloadProgress = 0;
	state.uploadProgress = 0;
	fireUpdate(session, id);
}

void updateDownloadProgress(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int itemIndex,
		const ItemInfo &info,
		float64 progress) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (progress >= 1.0) {
		state.currentDownload = -1;
		state.downloadItem = ItemInfo();
		state.downloadProgress = 0;
	} else {
		state.currentDownload = itemIndex;
		state.downloadItem = info;
		state.downloadProgress = progress;
	}
	if (itemIndex >= 0 && itemIndex < int(state.items.size())) {
		state.items[itemIndex].state = (progress >= 1.0)
			? ItemState::Uploading
			: ItemState::Downloading;
		state.items[itemIndex].info = info;
		state.items[itemIndex].downloadProgress = progress;
		state.items[itemIndex].progress = progress;
	}
	fireUpdate(session, peerId);
}

void updateUploadProgress(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int itemIndex,
		const ItemInfo &info,
		float64 progress) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (progress >= 1.0) {
		state.currentUpload = -1;
		state.uploadItem = ItemInfo();
		state.uploadProgress = 0;
	} else {
		state.currentUpload = itemIndex;
		state.uploadItem = info;
		state.uploadProgress = progress;
	}
	if (itemIndex >= 0 && itemIndex < int(state.items.size())) {
		state.items[itemIndex].state = (progress >= 1.0)
			? ItemState::Done
			: ItemState::Uploading;
		state.items[itemIndex].info = info;
		state.items[itemIndex].uploadProgress = progress;
		state.items[itemIndex].progress = progress;
	}
	fireUpdate(session, peerId);
}

void updateItemState(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int itemIndex,
		ItemState newState,
		const ItemInfo &info,
		float64 progress) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (itemIndex >= 0 && itemIndex < int(state.items.size())) {
		state.items[itemIndex].state = newState;
		state.items[itemIndex].info = info;
		state.items[itemIndex].progress = progress;
		if (newState == ItemState::Done || newState == ItemState::Failed) {
			state.items[itemIndex].downloadProgress = 1.0;
			state.items[itemIndex].uploadProgress = 1.0;
		}
	}
	fireUpdate(session, peerId);
}

bool isForwarding(const PeerId &id) {
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return false;
	return !it->second.cancelled && !it->second.finished;
}

std::optional<PeerId> activeJobPeer() {
	const auto &states = ActiveStates();
	for (const auto &[peer, state] : states) {
		if (!state.cancelled && !state.finished) {
			return peer;
		}
	}
	return std::nullopt;
}

void saveProgressForPeer(
		const PeerId &peer,
		not_null<Main::Session*> session) {
	const auto &states = ActiveStates();
	const auto it = states.find(peer);
	if (it == states.end()) return;
	const auto &state = it->second;
	if (state.saveCallback) {
		state.saveCallback();
	}
}

bool isPaused(const PeerId &id) {
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return false;
	return it->second.paused && !it->second.cancelled && !it->second.finished;
}

ForwardProgress currentProgress(const PeerId &id) {
	ForwardProgress result;
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) {
		result.state = State::Idle;
		return result;
	}
	const auto &state = it->second;
	result.sent = state.sent;
	result.total = state.total;
	result.skipped = state.skipped;
	result.destPeer = state.destPeer;
	result.currentDownload = state.currentDownload;
	result.currentUpload = state.currentUpload;
	result.downloadItem = state.downloadItem;
	result.uploadItem = state.uploadItem;
	result.downloadProgress = state.downloadProgress;
	result.uploadProgress = state.uploadProgress;
	result.downloadSpeed = state.downloadSpeed;
	result.uploadSpeed = state.uploadSpeed;
	result.sourceIds = state.sourceIds;
	result.items = state.items;

	if (state.cancelled) {
		result.state = State::Cancelled;
	} else if (state.finished) {
		result.state = State::Finished;
	} else if (state.paused) {
		result.state = State::Paused;
	} else {
		result.state = State::Sending;
	}

	return result;
}

void ClearProgressForPeer(const PeerId &peerId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.clearEfResumeForPeer(peerId);
	}
}

void CleanupPartialFilesForPeer(
		not_null<Main::Session*> session,
		const PeerId &peerId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto tempDir = File::DefaultDownloadPath(session) + "ForwardTemp/";
	const auto records = db.loadEfResumeItemsForPeer(
		session->uniqueId(),
		peerId);
	for (const auto &record : records) {
		if (!record.localPath.isEmpty()
				&& record.localPath.startsWith(tempDir)
				&& QFileInfo(record.localPath).exists()) {
			QFile(record.localPath).remove();
		}
	}
	db.clearEfResumeForPeer(peerId);
}

void CleanupLeftoverForwardFiles(
		not_null<Main::Session*> session,
		const QString &tempDir) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	auto keep = base::flat_set<QString>();
	if (db.isOpen()) {
		for (const auto &record : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
			if (!record.localPath.isEmpty()) {
				keep.emplace(record.localPath);
			}
		}
	}
	QDir dir(tempDir);
	if (!dir.exists()) {
		return;
	}
	const auto entries = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
	for (const auto &entry : entries) {
		const auto path = dir.absoluteFilePath(entry);
		if (keep.contains(path)) {
			continue;
		}
		QFile::remove(path);
	}
	const auto cleaned = QDir::cleanPath(tempDir);
	QDir().rmdir(cleaned);
}

std::vector<SavedJob> GetUnfinishedJobs(
		not_null<Main::Session*> session) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return {};
	}
	const auto records = db.loadUnfinishedEfResumeItems(session->uniqueId());
	base::flat_map<QString, SavedJob> jobs;
	for (const auto &record : records) {
		auto &job = jobs[record.jobId];
		if (job.dstId == PeerId()) {
			job.dstId = record.peerId;
			job.srcId = record.sourceId.peer;
		}
		job.total++;
		if (record.state.toInt() >= 3) {
			job.sent++;
		} else {
			job.unfinishedFiles++;
		}
		job.sourceMsgs.push_back(record.sourceId);
		job.uploadDone.push_back(record.state.toInt() >= 2);
		job.fileId.push_back(record.fileId);
		job.uploadedParts.push_back(record.uploadedParts);
	}
	std::vector<SavedJob> result;
	for (auto &[jobId, job] : jobs) {
		if (job.unfinishedFiles > 0) {
			result.push_back(std::move(job));
		} else {
			db.clearEfResumeJob(jobId);
		}
	}
	return result;
}

qint64 PersistedForwardBytes(
		not_null<Main::Session*> session,
		qint64 *total) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		if (total) {
			*total = 0;
		}
		return 0;
	}
	auto ready = qint64(0);
	auto tot = qint64(0);
	for (const auto &r : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
		if (r.state.toInt() >= 3) {
			continue;
		}
		const auto localSize = QFileInfo::exists(r.localPath)
			? qint64(QFileInfo(r.localPath).size())
			: qint64(0);
		ready += localSize;
		if (r.fileSize > 0) {
			tot += r.fileSize;
		} else if (const auto msg = session->data().message(r.sourceId)) {
			if (const auto media = msg->media()) {
				if (const auto doc = media->document()) {
					tot += doc->size;
					continue;
				} else if (const auto photo = media->photo()) {
					tot += localSize;
					continue;
				}
			}
			tot += localSize;
		} else {
			tot += localSize;
		}
	}
	if (total) {
		*total = tot;
	}
	return ready;
}

std::optional<qint64> persistedItemBytes(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return std::nullopt;
	}
	for (const auto &r : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
		if (r.state.toInt() >= 3 || r.sourceId != sourceId) {
			continue;
		}
		return QFileInfo::exists(r.localPath)
			? std::optional<qint64>(QFileInfo(r.localPath).size())
			: std::nullopt;
	}
	return std::nullopt;
}

std::optional<qint64> persistedItemFileSize(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return std::nullopt;
	}
	for (const auto &r : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
		if (r.state.toInt() >= 3 || r.sourceId != sourceId) {
			continue;
		}
		if (r.fileSize > 0) {
			return r.fileSize;
		}
		if (const auto msg = session->data().message(r.sourceId)) {
			if (const auto media = msg->media()) {
				if (const auto doc = media->document()) {
					return qint64(doc->size);
				}
			}
		}
		return std::nullopt;
	}
	return std::nullopt;
}

std::optional<SavedJob> GetUnfinishedJobByDst(
		const PeerId &dstId,
		not_null<Main::Session*> session) {
	for (const auto &job : GetUnfinishedJobs(session)) {
		if (job.dstId == dstId) {
			return job;
		}
	}
	return std::nullopt;
}

std::vector<SavedJob> GetFinishedJobs(
		not_null<Main::Session*> session) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return {};
	}
	const auto records = db.loadFinishedEfResumeItems(session->uniqueId());
	base::flat_map<QString, SavedJob> jobs;
	for (const auto &record : records) {
		auto &job = jobs[record.jobId];
		if (job.dstId == PeerId()) {
			job.dstId = record.peerId;
			job.srcId = record.sourceId.peer;
		}
		job.total++;
		job.sent++;
		job.sourceMsgs.push_back(record.sourceId);
		job.uploadDone.push_back(true);
		job.fileId.push_back(record.fileId);
		job.uploadedParts.push_back(record.uploadedParts);
	}
	std::vector<SavedJob> result;
	for (auto &[_, job] : jobs) {
		if (job.total > 0 && job.sent >= job.total) {
			result.push_back(std::move(job));
		}
	}
	return result;
}

void EnsureForwardSourceMessages(
		not_null<Main::Session*> session,
		const std::vector<FullMsgId> &sourceIds,
		Fn<void(bool succeeded)> done) {
	struct Pending {
		uint64 peerAccessHash = 0;
		QVector<MTPInputMessage> ids;
	};
	static base::flat_set<PeerId> inFlight;
	auto &owner = session->data();
	auto prepared = base::flat_map<PeerId, Pending>();
	for (const auto &sourceId : sourceIds) {
		if (owner.message(sourceId) || !IsServerMsgId(sourceId.msg)) {
			continue;
		}
		const auto groupPeer = peerIsChannel(sourceId.peer)
			? sourceId.peer
			: session->userPeerId();
		auto &perPeer = prepared[groupPeer];
		if (peerIsChannel(sourceId.peer)) {
			const auto channel = owner.channelLoaded(
				peerToChannel(sourceId.peer));
			if (!channel) {
				continue;
			}
			if (!perPeer.peerAccessHash) {
				perPeer.peerAccessHash = channel->accessHash();
			}
		}
		perPeer.ids.push_back(MTP_inputMessageID(MTP_int(sourceId.msg.bare)));
	}
	if (prepared.empty()) {
		return;
	}
	const auto weakSession = base::make_weak(session);
	const auto remaining = std::make_shared<int>(0);
	const auto anyFailed = std::make_shared<bool>(false);
	const auto finishOne = [=](const PeerId &groupPeer, bool ok) {
		inFlight.remove(groupPeer);
		if (!ok) {
			*anyFailed = true;
		}
		if (--*remaining <= 0 && weakSession && done) {
			done(!*anyFailed);
		}
	};
	for (auto &[groupPeer, perPeer] : prepared) {
		if (inFlight.contains(groupPeer)) {
			continue;
		}
		inFlight.emplace(groupPeer);
		++*remaining;
		if (const auto channelId = peerToChannel(groupPeer)) {
			session->api().request(MTPchannels_GetMessages(
				MTP_inputChannel(
					MTP_long(channelId.bare),
					MTP_long(perPeer.peerAccessHash)),
				MTP_vector<MTPInputMessage>(perPeer.ids)
			)).done([=](const MTPmessages_Messages &result) {
				if (weakSession) {
					session->data().processExistingMessages(
						session->data().channelLoaded(channelId),
						result);
				}
				finishOne(groupPeer, true);
			}).fail([=](const MTP::Error &) {
				finishOne(groupPeer, false);
			}).send();
		} else {
			session->api().request(MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(perPeer.ids)
			)).done([=](const MTPmessages_Messages &result) {
				if (weakSession) {
					session->data().processExistingMessages(nullptr, result);
				}
				finishOne(groupPeer, true);
			}).fail([=](const MTP::Error &) {
				finishOne(groupPeer, false);
			}).send();
		}
	}
	if (*remaining <= 0) {
		return;
	}
}

bool isEnhancedUpload(const FullMsgId &uploadId) {
	if (EFUploadIds().contains(uploadId)) {
		return true;
	}
	auto &active = Pipeline::Active();
	for (const auto &[peer, weak] : active) {
		if (const auto pipeline = weak.lock()) {
			if (pipeline->containsUpload(uploadId)) {
				return true;
			}
		}
	}
	return false;
}

bool isEnhancedTempUpload(
		not_null<Main::Session*> session,
		const QString &filename) {
	return filename.startsWith(
		File::DefaultDownloadPath(session) + "ForwardTemp/");
}

rpl::producer<PeerId> stateChanges() {
	return StateChanges.events();
}

rpl::producer<> counterChanges() {
	return CounterChanges.events();
}

std::vector<JobSnapshot> MemoryJobs(not_null<Main::Session*> session) {
	auto result = std::vector<JobSnapshot>();

	const auto belongsToSession = [&](const PeerId &peer) {
		return session->data().peerLoaded(peer)
			&& &session->data().peer(peer)->session() == session;
	};

	for (const auto &[peer, state] : ActiveStates()) {
		if (!belongsToSession(peer)) continue;
		result.push_back({
			.peer = peer,
			.srcPeer = state.srcPeer,
			.progress = currentProgress(peer),
			.active = true,
		});
	}
	for (const auto &state : FinishedStates()) {
		const auto peer = state.destPeer;
		if (!belongsToSession(peer)) continue;
		auto progress = ForwardProgress();
		progress.state = state.cancelled
			? State::Cancelled
			: state.resumable
				? State::Paused
				: State::Finished;
		progress.sent = state.sent;
		progress.total = state.total;
		progress.skipped = state.skipped;
		progress.destPeer = peer;
		progress.sourceIds = state.sourceIds;
		progress.items = state.items;
		result.push_back({
			.peer = peer,
			.srcPeer = state.srcPeer,
			.progress = std::move(progress),
			.finished = !state.resumable,
			.resumable = state.resumable,
		});
	}
	// Requests queued behind an active forward to the same peer are pending
	// batches: expose them so the Forwards tab and the counters reflect every
	// file the user queued, not only the currently running job.
	for (const auto &[peer, requests] : StartQueue()) {
		if (!belongsToSession(peer)) {
			continue;
		}
		for (const auto &request : requests) {
			auto progress = ForwardProgress();
			progress.state = State::Idle;
			progress.destPeer = peer;
			auto srcPeer = peer;
			for (const auto &item : request.items) {
				progress.total++;
				progress.sourceIds.push_back(item->fullId());
				srcPeer = item->history()->peer->id;
				auto tracked = TrackedItem();
				if (const auto media = item->media()) {
					if (const auto doc = media->document()) {
						tracked.info.name = doc->filename();
						tracked.info.size = doc->size;
					} else if (const auto photo = media->photo()) {
						tracked.info.name = QString::number(photo->id)
							+ u".jpg"_q;
					}
				}
				progress.items.push_back(tracked);
			}
			if (progress.total == 0 && request.resumeJob) {
				srcPeer = request.resumeJob->srcId;
				for (const auto &full : request.resumeJob->sourceMsgs) {
					progress.total++;
					progress.sourceIds.push_back(full);
					progress.items.push_back(TrackedItem{});
				}
			}
			result.push_back({
				.peer = peer,
				.srcPeer = srcPeer,
				.progress = std::move(progress),
			});
		}
	}
	return result;
}

std::vector<JobSnapshot> AllJobs(not_null<Main::Session*> session) {
	auto result = MemoryJobs(session);

	const auto belongsToSession = [&](const PeerId &peer) {
		return session->data().peerLoaded(peer)
			&& &session->data().peer(peer)->session() == session;
	};

	for (const auto &job : GetUnfinishedJobs(session)) {
		if (!belongsToSession(job.dstId)) continue;
		if (ActiveStates().find(job.dstId) != ActiveStates().end()
			|| HasFinishedForPeer(job.dstId)) {
			continue;
		}
		auto progress = ForwardProgress();
		progress.state = State::Paused;
		progress.sent = job.sent;
		progress.total = job.total;
		progress.destPeer = job.dstId;
		progress.sourceIds = job.sourceMsgs;
		progress.items.resize(job.total);
		for (auto i = 0; i < int(progress.items.size()); i++) {
			auto &tracked = progress.items[i];
			const auto done = (i < int(job.uploadDone.size())
				&& job.uploadDone[i]);
			tracked.state = done ? ItemState::Done : ItemState::Pending;
			tracked.sent = done;
			tracked.progress = done ? 1.0 : 0.0;
		}
		result.push_back({
			.peer = job.dstId,
			.srcPeer = job.srcId,
			.progress = std::move(progress),
			.resumable = true,
		});
	}
	return result;
}

rpl::producer<std::vector<JobSnapshot>> jobsValue(
		not_null<Main::Session*> session) {
	return rpl::single(MemoryJobs(session)) | rpl::then(
		StateChanges.events()
		| rpl::map([session](const PeerId &) {
			return MemoryJobs(session);
		})
	);
}

void MarkForwardedDone(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId,
		const QByteArray &hash) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.insertForwardedDone(sourceId, hash);
	}
}

std::vector<FullMsgId> GetForwardedDone() {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	return db.isOpen()
		? db.loadForwardedDone()
		: std::vector<FullMsgId>();
}

void EnsureResumeStatesSeeded(not_null<Main::Session*> session) {
	static base::flat_set<not_null<Main::Session*>> seeded;
	if (!seeded.emplace(session).second) {
		return;
	}
	session->lifetime().add([session] {
		seeded.remove(session);
	});
	auto &active = ActiveStates();
	auto &finished = FinishedStates();
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		// One-time migration: finished batches saved by older versions as
		// ef_resume 'done' rows become the flat per-item done table. Old
		// rows keep the per-index resume layout, so only import the leftover
		// items that never got an ef_done row.
		for (const auto &job : GetFinishedJobs(session)) {
			for (const auto &sourceId : job.sourceMsgs) {
				db.insertForwardedDone(sourceId, QByteArray());
			}
		}
		LastBatchCountsCache() = db.loadLastBatchCounts();
	}
	// The Forwards tab shows every forwarded item, persisted per-item so the
	// list accumulates across batches and restarts until the user clears it.
	auto doneItems = GetForwardedDone();
	for (const auto &sourceId : doneItems) {
		const auto already = ranges::any_of(
			finished,
			[&](const SharedState &state) {
				return ranges::find(state.sourceIds, sourceId)
					!= end(state.sourceIds);
			});
		if (already) {
			continue;
		}
		auto &state = finished.emplace_back();
		state.total = 1;
		state.sent = 1;
		state.finished = true;
		state.destPeer = sourceId.peer;
		state.srcPeer = sourceId.peer;
		state.sourceIds.push_back(sourceId);
		state.items.push_back(TrackedItem{
			.state = ItemState::Done,
			.progress = 1.0,
			.uploadProgress = 1.0,
			.sent = true,
		});
		NotifyStateChanged(sourceId.peer);
		NotifyCounterChanged();
	}
	// Interrupted batches are resumed on demand; their resume data lives in
	// the ef_resume rows grouped by (src, dst).
	const auto seedResumable = [&](const SavedJob &job) {
		if (active.find(job.dstId) != active.end()) {
			return;
		}
		for (const auto &state : finished) {
			if (state.destPeer == job.dstId && state.srcPeer == job.srcId) {
				return;
			}
		}
		auto &state = finished.emplace_back();
		state.total = job.total;
		state.sent = job.sent;
		state.skipped = 0;
		state.cancelled = false;
		state.paused = false;
		state.finished = true;
		state.resumable = true;
		state.destPeer = job.dstId;
		state.srcPeer = job.srcId;
		state.sourceIds = job.sourceMsgs;
		state.items.resize(job.sourceMsgs.size());
		for (auto i = 0; i < int(state.items.size()); i++) {
			const auto done = (i < int(job.uploadDone.size())
				&& job.uploadDone[i]);
			auto &item = state.items[i];
			item.state = done ? ItemState::Done : ItemState::Pending;
			item.sent = done;
			item.progress = done ? 1.0 : 0.0;
			item.uploadProgress = done ? 1.0 : -1.0;
		}
		NotifyStateChanged(job.dstId);
		NotifyCounterChanged();
	};
	for (const auto &job : GetUnfinishedJobs(session)) {
		seedResumable(job);
	}
}

void cancelItem(
		not_null<Main::Session*> session,
		const PeerId &peer,
		int itemIndex) {
	auto &states = ActiveStates();
	const auto it = states.find(peer);
	if (it == states.end()) return;
	auto &state = it->second;
	if (state.cancelled || state.finished) return;
	if (itemIndex < 0 || itemIndex >= int(state.items.size())) return;

	const auto pipeline = Pipeline::Active().find(peer);
	if (pipeline != Pipeline::Active().end()) {
		pipeline->second.lock()->cancelItem(itemIndex);
	} else {
		// No live pipeline: just drop the item from the visible state.
		if (state.total > 0) state.total--;
		state.items[itemIndex].cancelled = true;
		fireUpdate(session, peer);
		NotifyCounterChanged();
	}
}

bool isEnhancedForwardItem(
		not_null<Main::Session*> session,
		not_null<const HistoryItem*> item) {
	const auto id = item->fullId();
	for (const auto &job : AllJobs(session)) {
		if (!job.active && !job.resumable) {
			continue;
		}
		if (ranges::find(job.progress.sourceIds, id) != end(job.progress.sourceIds)) {
			return true;
		}
	}
	return false;
}

void cancelQueuedItem(
		not_null<Main::Session*> session,
		const PeerId &peer,
		const FullMsgId &sourceId) {
	// Remove one item from a batch that is still waiting in the start queue:
	// the item must not reappear when the active forward is resumed or done.
	auto &queue = StartQueue();
	const auto it = queue.find(peer);
	if (it == end(queue)) {
		return;
	}
	auto changed = false;
	for (auto request = it->second.begin(); request != it->second.end();) {
		const auto before = int(request->items.size());
		request->items.erase(
			ranges::remove_if(request->items, [&](not_null<HistoryItem*> item) {
				return item->fullId() == sourceId;
			}),
			end(request->items));
		if (request->items.size() != before) {
			changed = true;
		}
		if (request->items.empty()) {
			request = it->second.erase(request);
		} else {
			++request;
		}
	}
	if (it->second.empty()) {
		queue.erase(it);
	}
	if (changed) {
		NotifyStateChanged(peer);
		NotifyCounterChanged();
	}
}

void RemoveEfTempFile(const QString &path) {
	if (path.isEmpty() || !QFile::exists(path)) {
		return;
	}
	QFile::remove(path);
	const auto dir = QFileInfo(path).absolutePath();
	if (path.contains(u"ForwardTemp"_q)) {
		QDir(dir).rmdir(dir);
	}
}

void dropPersistedEfItem(
		not_null<Main::Session*> session,
		const PeerId &destPeer,
		const FullMsgId &sourceId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	auto localPath = QString();
	if (db.isOpen()) {
		for (const auto &r : db.loadUnfinishedEfResumeItems(
				session->uniqueId())) {
			if (r.sourceId == sourceId && !r.localPath.isEmpty()) {
				localPath = r.localPath;
				break;
			}
		}
	}
	db.removeEfResumeBySource(
		session->uniqueId(),
		destPeer,
		sourceId.msg);
	RemoveEfTempFile(localPath);
	if (const auto srcItem = session->data().message(sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (!i->resumable || i->destPeer != destPeer) {
			++i;
			continue;
		}
		const auto it = ranges::find(i->sourceIds, sourceId);
		if (it == end(i->sourceIds)) {
			++i;
			continue;
		}
		const auto idx = int(it - begin(i->sourceIds));
		i->sourceIds.erase(begin(i->sourceIds) + idx);
		if (idx < int(i->items.size())) {
			i->items.erase(begin(i->items) + idx);
		}
		i->total = int(i->sourceIds.size());
		i->sent = std::min(i->sent, i->total);
		if (i->total == 0) {
			NotifyStateChanged(destPeer);
			i = finished.erase(i);
			continue;
		}
		++i;
	}
	NotifyStateChanged(destPeer);
	NotifyCounterChanged();
}

void cancelItemByMessage(
		not_null<Main::Session*> session,
		not_null<const HistoryItem*> item) {
	const auto id = item->fullId();
	auto jobs = AllJobs(session);
	for (auto i = 0; i < int(jobs.size()); i++) {
		const auto &job = jobs[i];
		if (job.active) {
			for (auto j = 0; j < int(job.progress.sourceIds.size()); j++) {
				if (job.progress.sourceIds[j] == id) {
					auto &states = ActiveStates();
					if (states.find(job.peer) == states.end()
						&& Pipeline::Active().find(job.peer)
							== Pipeline::Active().end()) {
						auto &db = Core::App().downloadManager().ensureDedupDb();
						if (db.isOpen()) {
							db.removeEfResumeBySource(
								session->uniqueId(),
								job.peer,
								id.msg);
						}
						fireUpdate(session, job.peer);
						NotifyCounterChanged();
						return;
					}
					cancelItem(session, job.peer, j);
					return;
				}
			}
		} else if (job.resumable) {
			for (auto j = 0; j < int(job.progress.sourceIds.size()); j++) {
				if (job.progress.sourceIds[j] != id) {
					continue;
				}
				const auto srcItem = session->data().message(id);
				const auto pipelineIt = Pipeline::Active().find(job.peer);
				if (srcItem && pipelineIt != Pipeline::Active().end()) {
					pipelineIt->second.lock()->cancelItem(j);
				} else {
					dropPersistedEfItem(session, job.peer, id);
				}
				return;
			}
		} else if (!job.finished) {
			cancelQueuedItem(session, job.peer, id);
		}
	}
}

void CancelAll(not_null<Main::Session*> session) {
	const auto peers = [&] {
		auto result = std::vector<PeerId>();
		for (const auto &[peer, state] : ActiveStates()) {
			if (session->data().peerLoaded(peer)
				&& &session->data().peer(peer)->session() == session) {
				result.push_back(peer);
			}
		}
		return result;
	}();
	for (const auto &peer : peers) {
		cancelForward(peer, session);
	}
	auto purgedPersisted = false;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		for (const auto &job : GetUnfinishedJobs(session)) {
			if (session->data().peerLoaded(job.dstId)
				&& &session->data().peer(job.dstId)->session() == session) {
				CleanupPartialFilesForPeer(session, job.dstId);
				purgedPersisted = true;
				auto &finished = FinishedStates();
				for (auto i = finished.begin(); i != finished.end();) {
					if (i->resumable && i->destPeer == job.dstId) {
						i = finished.erase(i);
					} else {
						++i;
					}
				}
				NotifyStateChanged(job.dstId);
			}
		}
	}
	CleanupLeftoverForwardFiles(session, File::DefaultDownloadPath(session) + "ForwardTemp/");
	// Queued batches are not running yet, but the user asked to cancel all
	// forwards: drop them too so they don't silently start afterwards.
	auto &queue = StartQueue();
	for (auto it = queue.begin(); it != queue.end();) {
		const auto before = int(it->second.size());
		it->second.erase(ranges::remove_if(
			it->second,
			[&](const StartRequest &request) {
				return &request.api->session() == session;
			}), end(it->second));
		const auto peer = it->first;
		if (it->second.empty()) {
			it = queue.erase(it);
		} else {
			++it;
		}
		if (before) {
			NotifyStateChanged(peer);
		}
	}
	if (!peers.empty() || !queue.empty() || purgedPersisted) {
		NotifyCounterChanged();
	}
}

void notifyTransfersUpdated() {
	NotifyCounterChanged();
}

void ClearFinished(
		not_null<Main::Session*> session,
		const PeerId &peer) {
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (!i->resumable) {
			i = finished.erase(i);
		} else {
			++i;
		}
	}
	RemoveEFUploadsForPeer(peer);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.clearForwardedDone();
	}
	NotifyStateChanged(peer);
	NotifyCounterChanged();
}

void ClearFinishedItems(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId) {
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (i->resumable) {
			++i;
			continue;
		}
		const auto itemIt = ranges::find(i->sourceIds, sourceId);
		if (itemIt == end(i->sourceIds)) {
			++i;
			continue;
		}
		const auto index = int(itemIt - i->sourceIds.begin());
		if (index < int(i->items.size())) {
			i->items.erase(i->items.begin() + index);
		}
		i->sourceIds.erase(itemIt);
		i->total = std::max(0, i->total - 1);
		if (i->total <= 0 || i->items.empty()) {
			i = finished.erase(i);
		} else {
			++i;
		}
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.removeForwardedDone(sourceId);
	}
	NotifyStateChanged(sourceId.peer);
	NotifyCounterChanged();
}

auto Pipeline::Active()
-> std::unordered_map<PeerId, std::weak_ptr<Pipeline>> & {
	static auto map = std::unordered_map<PeerId, std::weak_ptr<Pipeline>>();
	return map;
}

void Pipeline::Start(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob) {
	const auto dstId = action.history->peer->id;
	if (ActiveStates().find(dstId) != ActiveStates().end()) {
		// A forward already runs to this peer: merge the new files into the
		// same pipeline instead of hiding them in a queue. They are processed
		// after the current items finish (one file at a time) and share the
		// same progress rendering, pause/resume and cancel logic.
		const auto live = Active().find(dstId);
		if (!resumeJob && live != Active().end()) {
			if (const auto pipeline = live->second.lock()) {
				pipeline->appendItems(std::move(items));
				NotifyStateChanged(dstId);
				NotifyCounterChanged();
				return;
			}
		}
		StartQueue()[dstId].push_back(StartRequest{
			api,
			std::move(items),
			action,
			forwardOptions,
			groupOptions,
			std::move(resumeJob) });
		// Let the Forwards tab and the counters pick up the queued batch.
		NotifyStateChanged(dstId);
		NotifyCounterChanged();
		return;
	}
	auto pipeline = std::make_shared<Pipeline>(
		api,
		std::move(items),
		action,
		forwardOptions,
		groupOptions,
		resumeJob);
	Active()[dstId] = pipeline;
	pipeline->run();
}

Pipeline::Pipeline(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob)
: _api(api)
, _session(api->session())
, _action(action)
, _forwardOptions(forwardOptions)
, _groupOptions(groupOptions)
, _runId(base::RandomValue<uint64>()) {
	_downloadPath = File::DefaultDownloadPath(&_session) + "ForwardTemp/";
	_peerId = action.history->peer->id;
	_srcPeer = !items.empty()
		? items.front()->history()->peer->id
		: _peerId;

	if (resumeJob) {
		_n = int(resumeJob->sourceMsgs.size());
		_items.resize(_n);
		for (auto i = 0; i < _n; i++) {
			_items[i].sourceId = resumeJob->sourceMsgs[i];
		}
		_sent = resumeJob->sent;
		loadProgress();

		for (auto i = 0; i < _n; i++) {
			const auto jobUploadDone = (i < int(resumeJob->uploadDone.size()))
				? resumeJob->uploadDone[i]
				: false;
			if (jobUploadDone) {
				_items[i].downloadDone = true;
				_items[i].uploadDone = true;
				_items[i].sent = true;
			} else {
				const auto local = QFileInfo(_items[i].path);
				const auto fileExists = local.exists() && local.size() > 0;
				if (_items[i].downloadDone && fileExists) {
					_items[i].uploadDone = false;
				} else {
					_items[i].downloadDone = false;
					_items[i].needsDownload = true;
					_items[i].uploadDone = false;
				}
			}
		}
	} else {
		_n = int(items.size());
		_items.resize(_n);
		for (auto i = 0; i < _n; i++) {
			_items[i].sourceId = items[i]->fullId();
		}
	}

	EnsureEFUploadIdSync(&_session);
}

Pipeline::~Pipeline() {
	if (GetEnhancedBool("prevent_forward_duplicates")) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			for (const auto &item : _items) {
				// Release only the in-flight dedup registrations. Items that
				// were actually sent keep their finished ('f') content record
				// so a later forward of the same item is detected as a
				// duplicate; wiping them here broke dedup for the next run.
				if (item.mediaId && !item.sent) {
					db.removePending(
						Data::DedupDb::Table::Uploads,
						item.mediaId);
				}
			}
		}
	}
	auto &active = Active();
	const auto it = active.find(_peerId);
	if (it != active.end() && it->second.lock().get() == this) {
		active.erase(it);
	}
	for (const auto &item : _items) {
		if (const auto srcItem = _session.data().message(item.sourceId)) {
			ClearShadowUpload(&_session, srcItem);
		}
	}
}

void Pipeline::run() {
	const auto self = shared_from_this();
	Info::Downloads::SetLastActivityTab(Info::Downloads::Tab::Forwards);
	_uploadLifetime = std::make_shared<rpl::lifetime>();
	_dlLifetime = std::make_shared<rpl::lifetime>();
	_uploadIndex = std::make_shared<base::flat_map<FullMsgId, int>>();

	// Classify items
	base::flat_map<MessageGroupId, int> albumItemCounts;
	for (auto i = 0; i < _n; i++) {
		const auto srcItem = _session.data().message(_items[i].sourceId);
		if (!srcItem) { _items[i].textOnly = true; continue; }
		const auto media = srcItem->media();
		if (!media) {
			_items[i].textOnly = true;
		} else if (media->photo()) {
			_items[i].isPhoto = true;
			if (const auto sg = srcItem->groupId()) {
				albumItemCounts[sg]++;
			}
		} else if (media->document()) {
			if (const auto sg = srcItem->groupId()) {
				albumItemCounts[sg]++;
			}
		} else {
			_items[i].textOnly = true;
		}
	}

	const auto regroupAll = (_groupOptions == Data::GroupingOptions::RegroupAll);
	const auto separate = (_groupOptions == Data::GroupingOptions::Separate);
	// "Group as is" only makes sense when the source messages are an album.
	// Forwarding several loose files should still produce a grouped album,
	// so treat the default as "regroup all" unless the user explicitly chose
	// to send them separately.
	const auto forceRegroup = (_groupOptions == Data::GroupingOptions::GroupAsIs)
		&& (albumItemCounts.empty() || albumItemCounts.size() == 1)
		&& (_n > 1);
	MessageGroupId regroupAllId;
	if (regroupAll || forceRegroup) {
		regroupAllId = MessageGroupId::FromRaw(
			_action.history->peer->id,
			base::RandomValue<uint64>(),
			false);
	}
	const auto doRegroup = (regroupAll || forceRegroup);

	for (auto i = 0; i < _n; i++) {
		if (_items[i].textOnly) continue;
		const auto srcItem = _session.data().message(_items[i].sourceId);
		if (!srcItem) { _items[i].textOnly = true; continue; }

		MessageGroupId sg;
		if (separate) {
			sg = MessageGroupId();
		} else if (doRegroup) {
			sg = regroupAllId;
		} else {
			sg = srcItem->groupId();
		}

		if (sg) {
			_items[i].sourceGroup = sg;
			if (_albums.find(sg) == _albums.end()) {
				auto album = std::make_shared<SendingAlbum>();
				album->options = _action.options;
				album->expectedCount = [&] {
					if (doRegroup) {
						int count = 0;
						for (auto j = 0; j < _n; j++) {
							if (!_items[j].textOnly) count++;
						}
						return count;
					} else {
						auto it = albumItemCounts.find(sg);
						return (it != albumItemCounts.end()) ? it->second : 1;
					}
				}();
				_api->_sendingAlbums.emplace(album->groupId, album);
				_albums.emplace(sg, std::move(album));
			}
		}
	}

	for (auto i = 0; i < _n; i++) {
		if (_items[i].textOnly || _items[i].path.isEmpty()) {
			const auto item = _session.data().message(_items[i].sourceId);
			if (!item) {
				adjustAlbumCount(i);
				_items[i].textOnly = true;
				_items[i].downloadDone = true;
				_items[i].uploadDone = true;
				continue;
			}
			const auto media = item->media();
			if (const auto doc = media ? media->document() : nullptr) {
				const auto filepath = doc->filepath(true);
				if (!filepath.isEmpty()) {
					_items[i].path = filepath;
					_items[i].downloadDone = true;
					EnhancedForward::updateDownloadProgress(
						&_session, _peerId, i,
						{ doc->filename(), doc->size },
						1.0);
				} else {
					auto name = doc->filename();
					if (name.isEmpty()) name = u"file"_q;
					name.replace(QRegularExpression("[:<>\"\\\\|?*]"), "_");
				_items[i].path = QDir(_downloadPath).absoluteFilePath(
					QString::number(_runId) + u"_"_q
					+ QString::number(i) + u"_"_q + name);
				QDir().mkpath(QFileInfo(_items[i].path).absolutePath());
				_items[i].needsDownload = true;
				}
			} else if (const auto photo = media ? media->photo() : nullptr) {
				const auto v = photo->activeMediaView();
				const auto destPath = QDir(_downloadPath).absoluteFilePath(
					QString::number(_runId) + u"_"_q
					+ QString::number(i) + u"_"_q + QString::number(photo->id) + u".jpg"_q);
				_items[i].path = destPath;
				if (v && v->loaded() && v->saveToFile(destPath)) {
					_items[i].downloadDone = true;
					EnhancedForward::updateDownloadProgress(
						&_session, _peerId, i,
						{ QString::number(photo->id) + u".jpg"_q, 0 },
						1.0);
				} else {
					_items[i].needsDownload = true;
				}
			} else {
				adjustAlbumCount(i);
				_items[i].textOnly = true;
				_items[i].downloadDone = true;
				_items[i].uploadDone = true;
			}
		}
		for (auto i = 0; i < _n; i++) {
			if (_items[i].textOnly || _items[i].fileSize > 0) {
				continue;
			}
			if (const auto srcItem = _session.data().message(_items[i].sourceId)) {
				if (const auto media = srcItem->media()) {
					if (const auto doc = media->document()) {
						_items[i].fileSize = doc->size;
					}
				}
			}
		}
	}

	_session.uploader().photoReady(
	) | rpl::on_next([self](const Storage::UploadedMedia &data) {
		self->onUploadDone(data);
	}, *_uploadLifetime);

	_session.uploader().documentReady(
	) | rpl::on_next([self](const Storage::UploadedMedia &data) {
		self->onUploadDone(data);
	}, *_uploadLifetime);

	_session.uploader().photoFailed(
	) | rpl::on_next([self](const FullMsgId &fullId) {
		self->onUploadFail(fullId);
	}, *_uploadLifetime);

	_session.uploader().documentFailed(
	) | rpl::on_next([self](const FullMsgId &fullId) {
		self->onUploadFail(fullId);
	}, *_uploadLifetime);

	_session.uploader().photoProgressInfo(
	) | rpl::on_next([self](const Storage::UploadFileProgress &data) {
		self->onUploadProgress(data);
	}, *_uploadLifetime);

	_session.uploader().documentProgressInfo(
	) | rpl::on_next([self](const Storage::UploadFileProgress &data) {
		self->onUploadProgress(data);
	}, *_uploadLifetime);

	_session.data().documentLoadProgress(
	) | rpl::on_next([self](not_null<DocumentData*> doc) {
		const auto now = crl::now();
		if (now - self->_lastDlProgressMs < 150) {
			return;
		}
		self->_lastDlProgressMs = now;
		for (auto i = 0; i < self->_n; i++) {
			self->checkItem(i);
			auto &item = self->_items[i];
			if (item.cancelled
				|| item.dedupSkipped
				|| item.downloadDone
				|| !item.downloadStarted) {
				continue;
			}
			const auto src = self->_session.data().message(item.sourceId);
			const auto media = src ? src->media() : nullptr;
			const auto srcDoc = media ? media->document() : nullptr;
			if (!srcDoc || srcDoc != doc || !srcDoc->loading()) {
				continue;
			}
			EnhancedForward::updateDownloadProgress(
				&self->_session,
				self->_peerId,
				i,
				{ srcDoc->filename(), srcDoc->size },
				srcDoc->progress());
			self->_session.data().notifyDocumentLayoutChanged(srcDoc);
		}
		for (auto i = 0; i < self->_n; i++) {
			const auto &item = self->_items[i];
			if (item.sentItem
				&& item.sentItem->media()
				&& item.sentItem->media()->document() == doc) {
				self->_session.data().requestItemRepaint(item.sentItem);
			}
		}
	}, *_dlLifetime);

	_session.downloaderTaskFinished(
	) | rpl::on_next([self] {
		for (auto i = 0; i < self->_n; i++) {
			self->checkItem(i);
			if (self->_items[i].sentItem) {
				self->_session.data().requestItemRepaint(
					self->_items[i].sentItem);
			}
		}
	}, *_dlLifetime);

	if (GetEnhancedBool("prevent_forward_duplicates")) {
		for (auto i = 0; i < _n; i++) {
			auto &item = _items[i];
			if (item.textOnly || item.uploadDone || item.dedupSkipped) continue;
			if (!item.mediaId) {
				const auto srcItem = _session.data().message(item.sourceId);
				const auto media = srcItem ? srcItem->media() : nullptr;
				const auto doc = media ? media->document() : nullptr;
				const auto photo = media ? media->photo() : nullptr;
				if (!doc && !photo) continue;
				item.mediaId = doc ? uint64(doc->id) : uint64(photo->id);
			}
			if (item.downloadDone && !item.path.isEmpty()) {
				item.dedupNeedsHash = true;
			}
		}
		// Pre-mark the repeated mediaIds inside this very batch, so the
		// Forwards tab shows only the unique files from the start and the
		// duplicates never download at all.
		base::flat_set<uint64> seenMediaIds;
		for (auto i = 0; i < _n; i++) {
			const auto &item = _items[i];
			if (item.dedupSkipped || item.textOnly || !item.mediaId) {
				continue;
			}
			if (!seenMediaIds.emplace(item.mediaId).second) {
				premarkDuplicate(i);
			}
		}
		auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
		if (dedupDb.isOpen()) {
			for (auto i = 0; i < _n; i++) {
				if (_items[i].mediaId && !_items[i].dedupSkipped) {
					dedupDb.removeByDocumentId(
						Data::DedupDb::Table::Uploads,
						_items[i].mediaId,
						u"u"_q);
				}
			}
		}
		for (auto i = 0; i < _n; i++) {
			if (_items[i].dedupNeedsHash && !_items[i].dedupSkipped) {
				dedupCheckItem(i);
			}
		}
	}

	if (GetEnhancedBool("prevent_forward_duplicates")) {
		// Pre-check all documents in order: the duplicate files in this batch
		// share content hashes but NOT docIds, so the same-docId premarking
		// above can't catch them. Querying the remote fingerprints up-front
		// (before the download queue starts) lets us mark them all before the
		// Forwards tab renders, so only the unique files are shown up-front.
		for (auto i = 0; i < _n; i++) {
			auto &item = _items[i];
			if (item.dedupSkipped || item.textOnly || item.downloadDone) {
				continue;
			}
			if (!item.needsDownload) {
				continue;
			}
			const auto srcItem = _session.data().message(item.sourceId);
			const auto media = srcItem ? srcItem->media() : nullptr;
			const auto doc = media ? media->document() : nullptr;
			if (!doc) {
				continue;
			}
			if (!item.mediaId) {
				item.mediaId = uint64(doc->id);
			}
			if (qint64(doc->size) < Data::kDedupMinPartialHashSize) {
				continue;
			}
			_dedupPrecheckQueue.push_back(i);
		}
		if (!_dedupPrecheckQueue.empty()) {
			runNextPrecheck();
			return;
		}
	}
	startSession();
}

void Pipeline::saveProgress() {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto jobId = u"ef_%1_%2"_q.arg(_srcPeer.value).arg(_peerId.value);
	for (auto i = 0; i < int(_items.size()); i++) {
		const auto &it = _items[i];
		// Dedup-skipped items are never forwarded and never resumed, so they
		// must not leave resume rows behind (they would re-appear as finished
		// history after a restart).
		if (it.dedupSkipped || it.cancelled) {
			continue;
		}
		auto record = Data::EfResumeItem();
		record.jobId = jobId;
		record.itemIndex = i;
		record.peerId = _peerId;
		record.sourceId = it.sourceId;
		record.state = QString::number(it.uploadDone
			? (it.sent ? 3 : 2)
			: (it.downloadDone ? 1 : 0));
		record.localPath = it.path;
		record.fileId = it.fileId;
		record.uploadedParts = it.uploadedParts;
		record.fileHash = it.fileHash;
		record.mediaId = it.mediaId;
		record.fileSize = it.fileSize;
		record.sessionId = _session.uniqueId();
		db.insertEfResumeItem(record);
	}
}

bool Pipeline::loadProgress() {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return false;
	}
	const auto records = db.loadEfResumeItemsForPeer(
		_session.uniqueId(),
		_peerId);
	if (records.empty()) {
		return false;
	}
	for (const auto &record : records) {
		for (auto i = 0; i < int(_items.size()); i++) {
			if (_items[i].sourceId != record.sourceId) {
				continue;
			}
			const auto state = record.state.toInt();
			_items[i].path = record.localPath;
			_items[i].downloadDone = (state >= 1);
			_items[i].uploadDone = (state >= 2);
			_items[i].sent = (state >= 3);
			_items[i].fileId = record.fileId;
			_items[i].uploadedParts = record.uploadedParts;
			_items[i].fileHash = record.fileHash;
			_items[i].mediaId = record.mediaId
				? record.mediaId
				: _items[i].mediaId;
			_items[i].fileSize = record.fileSize;
			break;
		}
	}
	return true;
}

void Pipeline::setupCallbacks() {
	const auto self = shared_from_this();
	EnhancedForward::setCancelCallback(
		_peerId,
		&_session,
		[self] {
			auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
			for (auto i = 0; i < self->_n; i++) {
				const auto msg = self->_session.data().message(self->_items[i].sourceId);
				if (!msg) continue;
				const auto media = msg->media();
				if (const auto doc = media ? media->document() : nullptr) {
					doc->cancel();
				} else if (const auto photo = media ? media->photo() : nullptr) {
					photo->cancel();
				}
				if (self->_items[i].uploadId != FullMsgId()) {
					self->_session.uploader().cancel(self->_items[i].uploadId);
					self->_uploadIndex->erase(self->_items[i].uploadId);
				}
				if (!self->_items[i].uploadDone
					&& self->_items[i].mediaId
					&& dedupDb.isOpen()) {
					dedupDb.removeByDocumentId(
						Data::DedupDb::Table::Uploads,
						self->_items[i].mediaId,
						u"u"_q);
					if (!self->_items[i].fileHash.isEmpty()) {
						dedupDb.removeUnfinishedByHash(
							Data::DedupDb::Table::Uploads,
							self->_items[i].fileHash);
					}
				}
			}
			EnhancedForward::CleanupPartialFilesForPeer(&self->_session, self->_peerId);
		});

	EnhancedForward::setPauseCallback(
		_peerId,
		&_session,
		[self] {
			for (auto i = 0; i < self->_n; i++) {
				if (self->_items[i].textOnly || self->_items[i].uploadDone) continue;
				if (!self->_items[i].downloadDone) {
					const auto item = self->_session.data().message(self->_items[i].sourceId);
					if (item) {
						const auto media = item->media();
						if (const auto doc = media ? media->document() : nullptr) {
							doc->pause();
						} else if (const auto photo = media ? media->photo() : nullptr) {
							photo->cancel();
						}
					}
				}
				if (self->_items[i].uploadId != FullMsgId()) {
					self->_session.uploader().pause(self->_items[i].uploadId);
				}
			}
			self->_downloadInFlight = false;
			self->_uploadInFlight = false;
			self->saveProgress();
		});

	EnhancedForward::setResumeCallback(
		_peerId,
		&_session,
		[self] {
			self->loadProgress();
			self->_session.uploader().unpause();
			for (auto i = 0; i < self->_n; i++) {
				if (self->_items[i].textOnly || self->_items[i].uploadDone) continue;
				const auto srcItem = self->_session.data().message(self->_items[i].sourceId);
				if (!srcItem) {
					self->adjustAlbumCount(i);
					self->_items[i].textOnly = true;
					self->_items[i].downloadDone = true;
					self->_items[i].uploadDone = true;
					continue;
				}
				const auto media = srcItem->media();
				if (!media || (!media->document() && !media->photo())) {
					continue;
				}
				// Restart an in-flight download that was paused: the loader
				// stays paused after FileLoader::pause() and a later save()
				// call alone does not resume it.
				if (const auto doc = media->document()) {
					if (doc->loading() && doc->downloadPaused()) {
						doc->resume();
					}
				}
				self->_items[i].downloadStarted = false;
				// Items with a live uploader entry resume on their own after
				// the unpause above; recreating the upload would send the
				// same file twice, so keep their state untouched.
				const auto liveUpload = self->_items[i].uploadStarted
					&& self->_items[i].uploadId != FullMsgId();
				if (!liveUpload) {
					self->_items[i].uploadStarted = false;
				}
				if (!self->_items[i].downloadDone) {
					self->_items[i].needsDownload = true;
				}
				self->_items[i].uploadDone = false;
			}
			self->_downloadCursor = 0;
			self->_uploadCursor = 0;
			self->_downloadInFlight = false;
			self->_uploadInFlight = false;
			self->pumpDownloads();
			self->pumpUploads();
			self->sendNext();
		});
}

void Pipeline::sendNext() {
	const auto self = shared_from_this();
	if (EnhancedForward::currentProgress(_peerId).state == EnhancedForward::State::Cancelled) {
		if (_uploadLifetime) _uploadLifetime->destroy();
		if (_dlLifetime) _dlLifetime->destroy();
		_session.data().sendHistoryChangeNotifications();
		return;
	}
	if (EnhancedForward::isPaused(_peerId)) {
		return;
	}

	// Send text-only items; media items are handled by upload callbacks
	while (_current < _n) {
		const auto i = _current;

		if (_items[i].cancelled) {
			_current++;
			_items[i].sent = true;
			continue;
		}

		if (_items[i].textOnly) {
			_current++;
			const auto srcItem = _session.data().message(_items[i].sourceId);
			if (srcItem) {
				const auto randomId = base::RandomValue<uint64>();
				const auto history = _action.history;
				const auto peer = history->peer;
				auto caption = srcItem->originalText();
				TextUtilities::Trim(caption);
				auto sentEntities = Api::EntitiesToMTP(
					&_session,
					caption.entities,
					Api::ConvertOption::SkipLocal);
				using SendFlag = MTPmessages_SendMessage::Flag;
				auto sendFlags = SendFlag(0)
					| (ShouldSendSilent(peer, _action.options) ? SendFlag::f_silent : SendFlag(0))
					| (!sentEntities.v.isEmpty() ? SendFlag::f_entities : SendFlag(0))
					| (_action.options.scheduled ? SendFlag::f_schedule_date : SendFlag(0));
				if (_action.replyTo) {
					sendFlags |= SendFlag::f_reply_to;
				}
				if (_action.options.sendAs) {
					sendFlags |= SendFlag::f_send_as;
				}
				if (_action.options.effectId) {
					sendFlags |= SendFlag::f_effect;
				}
				const auto done = [this, i](const MTPUpdates &, const MTP::Response &) {
					EnhancedForward::updateItemState(
						&_session, _peerId, i,
						ItemState::Done,
						{ _items[i].path, 0 },
						1.0);
					EnhancedForward::markItemSent(&_session, _peerId);
				};
				const auto fail = [this, i, randomId](const MTP::Error &error, const MTP::Response &) {
					_api->sendMessageFail(error, _action.history->peer, randomId, FullMsgId());
					EnhancedForward::updateItemState(
						&_session, _peerId, i,
						ItemState::Done,
						{ _items[i].path, 0 },
						1.0);
					EnhancedForward::markItemSent(&_session, _peerId);
				};
				_session.data().histories().sendPreparedMessage(
					history,
					_action.replyTo,
					randomId,
					Data::Histories::PrepareMessage<MTPmessages_SendMessage>(
						MTP_flags(sendFlags),
						peer->input(),
						Data::Histories::ReplyToPlaceholder(),
						MTP_string(caption.text),
						MTP_long(randomId),
						MTPReplyMarkup(),
						sentEntities,
						MTP_int(_action.options.scheduled),
						MTP_int(_action.options.scheduleRepeatPeriod),
						(_action.options.sendAs ? _action.options.sendAs->input() : MTP_inputPeerEmpty()),
						Data::ShortcutIdToMTP(&_session, _action.options.shortcutId),
						MTP_long(_action.options.effectId),
						MTP_long(0),
						Api::SuggestToMTP(_action.options.suggest),
						MTPInputRichMessage()),
					done,
					fail);
			} else {
				EnhancedForward::markItemSent(&_session, _peerId);
			}
			_items[i].sent = true;
			continue;
		}

		// Media item: skip if already sent, wait if not yet sent
		if (!_items[i].sent) return;
		_current++;
	}

	// Check if all items are done
	for (auto i = 0; i < _n; i++) {
		if (!_items[i].sent) return;
	}

	auto done = 0;
	for (auto i = 0; i < _n; i++) {
		if (!_items[i].cancelled && !_items[i].dedupSkipped) {
			done++;
		}
	}
	if (done > 0 || _skippedCount > 0) {
		EnhancedForward::ShowForwardDoneToast(
			done,
			done + _skippedCount,
			_skippedCount);
	}

	// Cleanup
	_session.data().sendHistoryChangeNotifications();
	_session.changes().historyUpdated(
		_action.history,
		(_action.options.scheduled
			? Data::HistoryUpdate::Flag::ScheduledSent
			: Data::HistoryUpdate::Flag::MessageSent));
	for (auto i = 0; i < _n; i++) {
		if (!_items[i].path.isEmpty() && _items[i].path.startsWith(_downloadPath)) {
			QFile::remove(_items[i].path);
		}
	}
	const auto session = &_session;
	const auto path = _downloadPath;
	crl::on_main([=] {
		CleanupLeftoverForwardFiles(session, path);
	});
	for (auto i = 0; i < _n; i++) {
		refreshSourceItemState(i);
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		const auto jobId = u"ef_%1_%2"_q.arg(_srcPeer.value).arg(_peerId.value);
		for (auto i = 0; i < _n; i++) {
			if (_items[i].cancelled || _items[i].dedupSkipped) {
				continue;
			}
			auto record = Data::EfResumeItem();
			record.jobId = jobId;
			record.itemIndex = i;
			record.peerId = _peerId;
			record.sourceId = _items[i].sourceId;
			record.state = u"done"_q;
			record.localPath = _items[i].path;
			record.fileId = _items[i].fileId;
			record.uploadedParts = _items[i].uploadedParts;
			record.fileHash = _items[i].fileHash;
			record.mediaId = _items[i].mediaId;
			record.fileSize = _items[i].fileSize;
			record.sessionId = _session.uniqueId();
			db.insertEfResumeItem(record);
		}
	}
	if (_uploadLifetime) _uploadLifetime->destroy();
	if (_dlLifetime) _dlLifetime->destroy();
}

void Pipeline::pumpUploads() {
	const auto self = shared_from_this();
	if (EnhancedForward::isPaused(_peerId)) return;
	while (true) {
		if (_uploadInFlight) return;
		while (_uploadCursor < _n
				&& (_items[_uploadCursor].dedupSkipped
					|| _items[_uploadCursor].cancelled
					|| _items[_uploadCursor].uploadDone)) {
			_uploadCursor++;
		}
		if (_uploadCursor >= _n) return;
		auto &candidate = _items[_uploadCursor];
		if (!candidate.downloadDone) return;
		if (candidate.dedupHashPending) return;
		// After a resume the cursor is reset to zero: skip items whose upload
		// is already queued (they were paused and unpaused on resume) so the
		// file is not uploaded and sent twice.
		if (candidate.uploadStarted
			&& candidate.uploadId != FullMsgId()
			&& ranges::any_of(
				_session.uploader().activeUploads(),
				[&](const auto &info) {
					return info.itemId == candidate.uploadId;
				})) {
			_uploadCursor++;
			continue;
		}
		const auto i = _uploadCursor;
		_items[i].uploadStarted = true;
		_uploadInFlight = true;
		_uploadCursor++;
		startUploadForItem(i);
		return;
	}
}

void Pipeline::startUploadForItem(int i) {
	auto &item = _items[i];
	const auto srcItem = _session.data().message(item.sourceId);
	if (!srcItem || item.textOnly) {
		item.uploadDone = true;
		_uploadInFlight = false;
		pumpUploads();
		sendNext();
		return;
	}
	const auto to = FileLoadTo(
		_action.history->peer->id,
		_action.options,
		_action.replyTo,
		_action.replaceMediaOf);
	const auto media = srcItem->media();
	const auto doc = media ? media->document() : nullptr;
	auto args = FileLoadTask::Args{
		.session = &_session,
		.filepath = item.path,
		.content = QByteArray(),
		.information = nullptr,
		.videoCover = nullptr,
		.type = item.isPhoto ? SendMediaType::Photo : SendMediaType::File,
		.to = to,
		.caption = (_forwardOptions
			== Data::ForwardOptions::NoNamesAndCaptions)
			? TextWithTags()
			: TextWithTags{
				srcItem->originalText().text,
				TextUtilities::ConvertEntitiesToTextTags(srcItem->originalText().entities) },
		.spoiler = false,
		.album = nullptr,
		.forceFile = false,
		.sendLargePhotos = item.isPhoto,
		.idOverride = 0,
		.displayName = {},
	};
	if (doc) {
		args.displayName = doc->filename();
	}
	if (!item.isPhoto && !item.path.isEmpty()) {
		auto info = FileLoadTask::ReadMediaInformation(
			item.path,
			QByteArray(),
			doc ? doc->mimeString() : QString());
		if (info) {
			if (doc) {
				if (const auto sd = doc->song()) {
					if (auto song = std::get_if<Ui::PreparedFileInformation::Song>(
							&info->media)) {
						if (song->title.isEmpty()) song->title = sd->title;
						if (song->performer.isEmpty()) {
							song->performer = sd->performer;
						}
					}
				}
				if (!doc->inlineThumbnailBytes().isEmpty()) {
					const auto inlineThumb = Images::FromInlineBytes(
						doc->inlineThumbnailBytes());
					if (!inlineThumb.isNull()) {
						if (auto song = std::get_if<Ui::PreparedFileInformation::Song>(
								&info->media)) {
							if (song->cover.isNull()) song->cover = inlineThumb;
						} else if (info->fileThumbnail.isNull()) {
							info->fileThumbnail = inlineThumb;
						}
					}
				}
			}
			args.information = std::move(info);
		}
	}
	const auto self = shared_from_this();
	auto tasks = std::vector<std::unique_ptr<Task>>();
	tasks.push_back(std::make_unique<EnhancedFileTask>(
		std::move(args),
		[this, i, self](std::shared_ptr<FilePrepareResult> result) {
			auto &item = _items[i];
			if (item.cancelled) {
				_uploadInFlight = false;
				pumpUploads();
				return;
			}
			const auto prepareState = EnhancedForward::currentProgress(_peerId).state;
			if (result && result->filesize > 0) {
				item.prepared = std::move(result);
				if (item.fileId == 0) {
					item.fileId = base::RandomValue<uint64>();
				}
				item.prepared->fileId = item.fileId;
				const auto path = item.path;
				if (!path.isEmpty()) {
					const auto realSize = QFile(path).size();
					if (realSize > item.prepared->filesize) {
						item.prepared->filesize = realSize;
						item.prepared->partssize = realSize;
					}
				}
				if (item.prepared->partssize > 0 && !item.prepared->fileparts.empty()) {
					item.partSize = item.prepared->partssize / int(item.prepared->fileparts.size());
				}
			} else if (prepareState == EnhancedForward::State::Sending) {
				adjustAlbumCount(i);
				item.textOnly = true;
				item.uploadDone = true;
				_uploadInFlight = false;
				pumpUploads();
				sendNext();
				return;
			} else {
				// The batch was paused while preparing: keep the item pending
				// for resume instead of marking it done.
				item.uploadDone = (prepareState
					!= EnhancedForward::State::Paused);
				_uploadInFlight = false;
				pumpUploads();
				return;
			}

			if (EnhancedForward::isPaused(_peerId)) {
				// The batch was paused while this file was being prepared.
				// Do not start the upload - the resume callback restarts it.
				item.uploadDone = false;
				_uploadInFlight = false;
				saveProgress();
				return;
			}

			// Create local message with media (like SendConfirmedFile)
			const auto localMsgId = FullMsgId(
				_peerId,
				_session.data().nextLocalMessageId());
			item.localMsgId = localMsgId;

			auto flags = NewMessageFlags(_action.history->peer);
			if (_action.replyTo) {
				flags |= MessageFlag::HasReplyInfo;
			}
			FillMessagePostFlags(_action, _action.history->peer, flags);
			if (_action.options.scheduled) {
				flags |= MessageFlag::IsOrWasScheduled;
			}

			const auto &prepared = item.prepared;
			const auto groupId = item.sourceGroup
				? [&] {
					const auto albumIt = _albums.find(item.sourceGroup);
					return albumIt != _albums.end()
						? albumIt->second->groupId
						: uint64(0);
				}()
				: uint64(0);

			const auto msgMedia = MTPMessageMedia([&] {
				if (prepared->type == SendMediaType::Photo) {
					using Flag = MTPDmessageMediaPhoto::Flag;
					return MTP_messageMediaPhoto(
						MTP_flags(Flag::f_photo),
						prepared->photo,
						MTPint(),
						MTPDocument());
				} else {
					using Flag = MTPDmessageMediaDocument::Flag;
					return MTP_messageMediaDocument(
						MTP_flags(Flag::f_document),
						prepared->document,
						MTPVector<MTPDocument>(),
						MTPPhoto(),
						MTPint(),
						MTPint());
				}
			}());

			auto caption = TextWithEntities{
				prepared->caption.text,
				TextUtilities::ConvertTextTagsToEntities(prepared->caption.tags)
			};
			TextUtilities::Trim(caption);

			// Start the upload BEFORE creating the local message, exactly
			// like Api::SendConfirmedFile does. This way the DocumentData is
			// created with the real (InMemoryLocation) thumbnail already
			// attached, so isSongWithCover() is true when the view is built
			// and the cover gets requested/loaded during the session.
			item.uploadId = localMsgId;
			_uploadIndex->emplace(localMsgId, i);
			EFUploadIds().emplace(localMsgId);
			_session.uploader().upload(localMsgId, item.prepared, item.uploadedParts);
			saveProgress();

			if (const auto srcItem = _session.data().message(item.sourceId)) {
				SetShadowUpload(
					&_session,
					srcItem,
					item.prepared ? int64(item.prepared->filesize) : item.fileSize);
				if (const auto media = srcItem->media()) {
					if (const auto doc = media->document()) {
						// The destination chat already renders the prepared
						// thumbnail. Mirror it onto the source document's
						// shared media view so the transfer manager rows for
						// this source item show the same cover instead of a
						// black box. Media-view only: never persist an
						// in-memory thumb into the document location, because
						// serialized in-memory thumbs are rejected on reload.
						if (!doc->hasThumbnail() && !item.prepared->thumb.isNull()) {
							doc->createMediaView()->setThumbnail(item.prepared->thumb);
						}
					}
				}
			}

			const auto localMsg = _action.history->addNewLocalMessage({
				.id = localMsgId.msg,
				.flags = flags,
				.from = NewMessageFromId(_action),
				.replyTo = _action.replyTo,
				.date = NewMessageDate(_action.options),
				.shortcutId = _action.options.shortcutId,
				.starsPaid = _action.options.starsApproved,
				.postAuthor = NewMessagePostAuthor(_action),
				.groupedId = groupId,
				.effectId = _action.options.effectId,
				.suggest = HistoryMessageSuggestInfo(_action.options),
			}, caption, msgMedia);

			// Add to album items if part of an album
			if (groupId) {
				const auto albumIt = _albums.find(item.sourceGroup);
				if (albumIt != _albums.end()) {
					const auto &album = albumIt->second;
					album->expectedCount = 0;
					for (auto j = 0; j < _n; j++) {
						const auto &other = _items[j];
						if (other.sourceGroup == item.sourceGroup
							&& !other.textOnly
							&& !other.cancelled
							&& !other.dedupSkipped) {
							album->expectedCount++;
						}
					}
					album->items.emplace_back(kEmptyTaskId);
					album->items.back().msgId = localMsg->fullId();
				}
			}

			// Set up upload tracking (use localMsgId as upload ID)
			item.sentItem = localMsg.get();

			const auto fileSize = prepared ? qint64(prepared->filesize) : qint64(0);
			const auto partSize = item.partSize;
			const auto totalParts = (partSize > 0 && fileSize > 0)
				? int((fileSize + partSize - 1) / partSize)
				: 0;
			const auto initialProgress = (totalParts > 0)
				? float64(item.uploadedParts) / float64(totalParts)
				: 0.0;
			EnhancedForward::updateUploadProgress(
				&_session, _peerId, i,
				{ prepared ? prepared->filename : QString(), fileSize },
				initialProgress);
		}));
	_api->_fileLoader->addTasks(std::move(tasks));
}

void Pipeline::onUploadDone(const Storage::UploadedMedia &data) {
	if (data.fullId.peer != _peerId) return;
	const auto it = _uploadIndex->find(data.fullId);
	if (it == _uploadIndex->end()) return;
	const auto idx = it->second;
	auto &item = _items[idx];
	item.uploadInfo = std::move(data.info);
	item.uploadDone = true;
	item.retries = 0;
	if (GetEnhancedBool("prevent_forward_duplicates")
			&& !item.fileHash.isEmpty()
			&& item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.updateDedupStatus(
				Data::DedupDb::Table::Uploads,
				item.fileHash,
				u"f"_q);
		}
	}
	EnhancedForward::updateUploadProgress(
		&_session, _peerId, idx,
		{ item.prepared ? item.prepared->filename : QString(),
		  item.prepared ? qint64(item.prepared->filesize) : qint64(0) },
		1.0);
	saveProgress();

	if (const auto srcItem = _session.data().message(item.sourceId)) {
		ClearShadowUpload(&_session, srcItem);
	}

	// The Uploader itself already calls sendUploadedPhoto/Document
	// via its internal subscription (file_upload.cpp lines 183/199).
	// We just track completion here.
	if (item.sentItem) {
		_session.data().requestItemRepaint(item.sentItem);
	}
	item.sent = true;
	EnhancedForward::markItemSent(&_session, _peerId);
	MarkForwardedDone(&_session, item.sourceId, item.fileHash);

	if (const auto srcItem = _session.data().message(item.sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}

	// The uploader deletes the ForwardTemp file synchronously after this
	// callback, so defer the document state refresh to run once the local
	// file no longer exists. Otherwise the document would still report a
	// stale downloaded state and hide the download arrow.
	const auto self = shared_from_this();
	crl::on_main([self, idx] {
		self->refreshSourceItemState(idx);
	});

	_uploadInFlight = false;
	pumpUploads();
	sendNext();
}

void Pipeline::refreshSourceItemState(int idx) {
	if (idx < 0 || idx >= _n) {
		return;
	}
	const auto &item = _items[idx];
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		if (const auto media = srcItem->media()) {
			if (const auto doc = media->document()) {
				if (doc->filepath(true).startsWith(_downloadPath)) {
					_session.local().removeFileLocation(doc->mediaKey());
					[[maybe_unused]] const auto refreshedPath = doc->filepath(true);
				}
				_session.data().requestDocumentViewRepaint(doc);
			}
		}
		_session.data().requestItemRepaint(srcItem);
	}
	if (item.sentItem) {
		if (const auto media = item.sentItem->media()) {
			if (const auto doc = media->document()) {
				[[maybe_unused]] const auto refreshedPath = doc->filepath(true);
				_session.data().requestDocumentViewRepaint(doc);
			}
		}
		_session.data().requestItemRepaint(item.sentItem);
	}
}

void Pipeline::onUploadFail(const FullMsgId &fullId) {
	if (fullId.peer != _peerId) return;
	const auto it = _uploadIndex->find(fullId);
	if (it == _uploadIndex->end()) return;
	const auto idx = it->second;
	auto &item = _items[idx];
	_uploadInFlight = false;
	const auto state = EnhancedForward::currentProgress(_peerId).state;
	if (state == EnhancedForward::State::Paused) {
		item.uploadDone = false;
		return;
	}
	if (state != EnhancedForward::State::Sending) {
		item.uploadDone = true;
		if (const auto srcItem = _session.data().message(item.sourceId)) {
			ClearShadowUpload(&_session, srcItem);
		}
		return;
	}
	constexpr auto kMaxUploadRetries = 3;
	if (item.retries < kMaxUploadRetries) {
		item.retries++;
		item.uploadedParts = 0;
		item.fileId = base::RandomValue<uint64>();
		item.uploadDone = false;
		_uploadInFlight = true;
		startUploadForItem(idx);
		return;
	}
	_uploadInFlight = false;
	item.uploadDone = false;
	item.retries = 0;
	item.uploadedParts = 0;
	item.fileId = base::RandomValue<uint64>();
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		ClearShadowUpload(&_session, srcItem);
	}
	if (!item.fileHash.isEmpty() && item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.removeByDocumentId(
				Data::DedupDb::Table::Uploads,
				item.mediaId,
				u"u"_q);
			db.removePending(
				Data::DedupDb::Table::Uploads,
				item.mediaId);
			db.removeUnfinishedByHash(
				Data::DedupDb::Table::Uploads,
				item.fileHash);
		}
	}
	const auto history = _session.data().history(_peerId);
	const auto fileName = item.prepared ? item.prepared->filename : u"file"_q;
	const auto text = tr::lng_tm_fw_upload_failed(
		tr::now,
		lt_file_name,
		fileName);
	const auto randomId = base::RandomValue<uint64>();
	const auto newId = FullMsgId(_peerId, _session.data().nextLocalMessageId());
	_session.data().registerMessageRandomId(randomId, newId);
	history->addNewLocalMessage({
		.id = newId.msg,
		.flags = MessageFlags(),
		.from = NewMessageFromId(_action),
		.date = NewMessageDate(_action.options),
	}, TextWithEntities::Simple(text), MTP_messageMediaEmpty());
	saveProgress();
	EnhancedForward::pauseForward(_peerId, &_session);
}

void Pipeline::downloadFailed(int idx) {
	auto &item = _items[idx];
	constexpr auto kMaxDownloadRetries = 3;
	if (item.retries < kMaxDownloadRetries) {
		item.retries++;
		item.downloadedBytes = 0;
		item.downloadDone = false;
		_downloadInFlight = false;
		_downloadCursor = idx;
		pumpDownloads();
		return;
	}
	_downloadInFlight = false;
	item.retries = 0;
	if (item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.removeByDocumentId(
				Data::DedupDb::Table::Uploads,
				item.mediaId,
				u"u"_q);
			db.removePending(
				Data::DedupDb::Table::Uploads,
				item.mediaId);
		}
	}
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	const auto photo = media ? media->photo() : nullptr;
	auto name = doc
		? doc->filename()
		: (photo ? QString::number(photo->id) + u".jpg"_q : QString());
	if (name.isEmpty()) {
		name = QFileInfo(item.path).fileName();
	}
	if (name.isEmpty()) {
		name = u"file"_q;
	}
	const auto history = _session.data().history(_peerId);
	const auto text = tr::lng_tm_fw_download_failed(
		tr::now,
		lt_file_name,
		name);
	const auto randomId = base::RandomValue<uint64>();
	const auto newId = FullMsgId(_peerId, _session.data().nextLocalMessageId());
	_session.data().registerMessageRandomId(randomId, newId);
	history->addNewLocalMessage({
		.id = newId.msg,
		.flags = MessageFlags(),
		.from = NewMessageFromId(_action),
		.date = NewMessageDate(_action.options),
	}, TextWithEntities::Simple(text), MTP_messageMediaEmpty());
	saveProgress();
	EnhancedForward::pauseForward(_peerId, &_session);
}

void Pipeline::onUploadProgress(const Storage::UploadFileProgress &data) {
	if (data.fullId.peer != _peerId) return;
	const auto it = _uploadIndex->find(data.fullId);
	if (it == _uploadIndex->end()) return;
	const auto idx = it->second;
	auto &item = _items[idx];
	if (item.uploadDone) return;
	const auto prepared = item.prepared;
	const auto filename = prepared ? prepared->filename : QString();
	const auto filesize = prepared ? qint64(prepared->filesize) : qint64(0);
	const auto size = (filesize > 0) ? filesize : data.size;
	const auto p = (size > 0) ? std::clamp(float64(data.offset) / float64(size), 0., 1.) : 0.;
	if (data.partSize > 0) {
		item.partSize = data.partSize;
	}
		if (data.size > 0 && item.partSize > 0) {
			item.uploadedParts = int(data.offset / item.partSize);
			const auto newPct = (data.size > 0) ? int(float64(data.offset) / float64(data.size) * 100) : 0;
			const auto lastSaved = item.lastSavedPct;
			if (newPct >= lastSaved + 10
				|| newPct >= 99
				|| (newPct > 0 && lastSaved == 0)) {
				item.lastSavedPct = newPct;
				saveProgress();
			}
		}
		const auto now = crl::now();
		if (now - _lastShadowRepaintMs >= 150) {
			_lastShadowRepaintMs = now;
			if (const auto srcItem = _session.data().message(item.sourceId)) {
				UpdateShadowUpload(&_session, srcItem, data.offset, size);
			}
		}
	EnhancedForward::updateUploadProgress(&_session, _peerId, idx, { filename, filesize }, p);
}

void Pipeline::checkItem(int i) {
	const auto self = shared_from_this();
	auto &item = _items[i];
	if (item.downloadDone) return;
	if (!item.downloadStarted) return;
	if (EnhancedForward::currentProgress(_peerId).state == EnhancedForward::State::Cancelled) {
		return;
	}
	if (EnhancedForward::isPaused(_peerId)) return;

	const auto fi = QFileInfo(item.path);
	if (fi.exists()) {
		const auto srcItem = _session.data().message(item.sourceId);
		const auto media = srcItem ? srcItem->media() : nullptr;
		if (const auto doc = media ? media->document() : nullptr) {
			if (doc->size > 0 && fi.size() < doc->size) {
				item.downloadedBytes = fi.size();
				EnhancedForward::updateDownloadProgress(
					&_session, _peerId, i,
					{ doc->filename(), doc->size },
					float64(fi.size()) / float64(doc->size));
				_session.data().notifyDocumentLayoutChanged(doc);
				if (srcItem) {
					_session.data().requestItemRepaint(srcItem);
				}
				return;
			}
		}
		item.downloadDone = true;
		item.downloadedBytes = fi.size();
		item.retries = 0;
		_downloadInFlight = false;
		EnhancedForward::updateDownloadProgress(
			&_session, _peerId, i,
			{ fi.fileName(), fi.size() },
			1.0);
		dedupCheckItem(i);
		pumpUploads();
		pumpDownloads();
		return;
	}
	const auto srcItem = _session.data().message(item.sourceId);
	if (!srcItem) {
		return;
	}
	const auto media = srcItem->media();
	if (const auto doc = media ? media->document() : nullptr) {
		if (doc->loading()) {
			item.downloadedBytes = qint64(doc->loadOffset());
			EnhancedForward::updateDownloadProgress(
				&_session, _peerId, i,
				{ doc->filename(), doc->size },
				doc->progress());
			_session.data().notifyDocumentLayoutChanged(doc);
			_session.data().requestItemRepaint(srcItem);
		} else if (doc->status == FileDownloadFailed) {
			downloadFailed(i);
			return;
		}
	} else if (const auto photo = media ? media->photo() : nullptr) {
		const auto v = item.photoView ? item.photoView : photo->activeMediaView();
		const auto loaded = v && v->loaded();
		const auto failed = photo->failed(Data::PhotoSize::Large);
		const auto loading = photo->loading(Data::PhotoSize::Large);
		if (loaded) {
			if (v->saveToFile(item.path)) {
				item.downloadDone = true;
				item.downloadedBytes = QFile(item.path).size();
				item.retries = 0;
				EnhancedForward::updateDownloadProgress(
					&_session, _peerId, i,
					{ QString::number(photo->id) + u".jpg"_q, 0 },
					1.0);
				_downloadInFlight = false;
				dedupCheckItem(i);
				pumpUploads();
				pumpDownloads();
			} else {
				adjustAlbumCount(i);
				item.textOnly = true;
				item.downloadDone = true;
				item.uploadDone = true;
				sendNext();
			}
		} else if (failed) {
			downloadFailed(i);
			return;
		} else if (!loading) {
			adjustAlbumCount(i);
			item.textOnly = true;
			item.downloadDone = true;
			item.uploadDone = true;
			_downloadInFlight = false;
			sendNext();
			pumpDownloads();
		}
	}
}

void Pipeline::pumpDownloads() {
	const auto self = shared_from_this();
	while (_downloadCursor < _n
			&& (!_items[_downloadCursor].needsDownload
				|| _items[_downloadCursor].downloadDone
				|| _items[_downloadCursor].cancelled)) {
		_downloadCursor++;
	}
	if (EnhancedForward::isPaused(_peerId)) return;
	if (_downloadInFlight) return;
	if (_downloadCursor >= _n) return;

	const auto i = _downloadCursor;
	auto &item = _items[i];
	item.downloadStarted = true;
	_downloadInFlight = true;
	_downloadCursor++;
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	const auto photo = media ? media->photo() : nullptr;

	if (GetEnhancedBool("prevent_forward_duplicates")
			&& !item.dedupPrechecked
			&& (doc || photo)) {
		const auto mediaId = doc ? uint64(doc->id) : uint64(photo->id);
		item.mediaId = mediaId;
		auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
		if (dedupDb.isOpen()
				&& (dedupDb.containsDocId(
					Data::DedupDb::Table::Uploads,
					mediaId)
					|| dedupDb.containsDocIdInDb(
						Data::DedupDb::Table::Uploads,
						mediaId))) {
			skipAsDuplicate(i);
			return;
		}
		const auto size = doc ? qint64(doc->size) : qint64(0);
		const auto remotePrecheck = doc
			&& size >= Data::kDedupMinPartialHashSize
			&& dedupDb.isOpen();
		if (dedupDb.isOpen()) {
			// In-flight upload registration; the hash fills in once the
			// remote fingerprint (or the local file hash) is computed.
			dedupDb.addPending(
				Data::DedupDb::Table::Uploads,
				mediaId,
				QByteArray());
		}
		if (remotePrecheck && !item.dedupPrechecked) {
			const auto origin = Data::FileOrigin(
				FullMsgId(srcItem->history()->peer->id, srcItem->id));
			Data::RemoteFileFingerprint(&_session, doc, [=](QByteArray hash) {
				auto &it = _items[i];
				if (hash.isEmpty()) {
					it.dedupNeedsHash = true;
					doc->save(origin, it.path, LoadFromCloudOrLocal, false, true);
					Core::App().downloadManager().addLoading(
						{ .item = srcItem, .document = doc },
						true);
					checkItem(i);
					if (it.downloadDone) {
						pumpDownloads();
					}
					return;
				}
				it.fileHash = hash;
				it.fileSize = size;
				auto &db = Core::App().downloadManager().ensureDedupDb();
				// This doc passed the id check above, so any content-hash match
				// means the same content was forwarded before: skip it and
				// attach this id to the existing hash for future id-only dedup.
				const auto duplicateId = db.seekDocumentId(
					Data::DedupDb::Table::Uploads,
					hash,
					0);
				if (duplicateId) {
					skipAsDuplicate(i);
					return;
				}
				db.addPending(
					Data::DedupDb::Table::Uploads,
					mediaId,
					hash);
				doc->save(origin, it.path, LoadFromCloudOrLocal, false, true);
				Core::App().downloadManager().addLoading(
					{ .item = srcItem, .document = doc },
					true);
				checkItem(i);
				if (it.downloadDone) {
					pumpDownloads();
				}
			});
			return;
		}
		item.dedupNeedsHash = true;
	}

	if (doc) {
		doc->save(
			Data::FileOrigin(FullMsgId(srcItem->history()->peer->id, srcItem->id)),
			item.path,
			LoadFromCloudOrLocal,
			false,
			true);
		Core::App().downloadManager().addLoading(
			{ .item = srcItem, .document = doc },
			true);
		saveProgress();
	} else if (photo) {
		item.photoView = photo->createMediaView();
		photo->load(
			Data::PhotoSize::Large,
			Data::FileOrigin(FullMsgId(srcItem->history()->peer->id, srcItem->id)));
	} else {
		item.textOnly = true;
		item.downloadDone = true;
		item.uploadDone = true;
		_downloadInFlight = false;
		sendNext();
		pumpDownloads();
		return;
	}
	checkItem(i);
	if (item.downloadDone) {
		pumpDownloads();
	}
}

void Pipeline::dedupCheckItem(int i) {
	const auto self = shared_from_this();
	auto &item = _items[i];
	if (!GetEnhancedBool("prevent_forward_duplicates")) return;
	if (item.dedupSkipped || item.uploadDone || item.dedupHashPending) return;
	// The precheck queue already hashed this item and registered it (or
	// marked it as a duplicate) before the download started.
	if (item.dedupPrechecked) {
		pumpUploads();
		return;
	}
	if (!item.downloadDone || item.path.isEmpty()) {
		return;
	}
	item.dedupNeedsHash = false;
	item.dedupHashPending = true;

	const auto path = item.path;
	const auto mediaId = item.mediaId;
	const auto needsDelete = item.path.startsWith(_downloadPath);
	auto docSize = int64(0);
	auto asPhoto = false;
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	if (const auto photo = media ? media->photo() : nullptr) {
		asPhoto = true;
	} else if (const auto doc = media ? media->document() : nullptr) {
		docSize = int64(doc->size);
	} else {
		item.dedupHashPending = false;
		return;
	}

	const auto weak = std::weak_ptr<Pipeline>(self);
	crl::async([=]() mutable {
		QByteArray hash;
		qint64 size = 0;
		if (asPhoto) {
			QFile f(path);
			if (f.open(QIODevice::ReadOnly)) {
				const auto content = f.readAll();
				if (!content.isEmpty()) {
					hash = Data::ContentFingerprint(content);
					size = content.size();
				}
			}
		} else {
			hash = Data::FileFingerprint(path, docSize);
			size = docSize;
		}
		crl::on_main([=, hash = std::move(hash)]() mutable {
			if (const auto alive = weak.lock()) {
				alive->dedupHashed(
					i,
					std::move(hash),
					size,
					mediaId,
					needsDelete);
			}
		});
	});
}

void Pipeline::dedupHashed(
		int i,
		QByteArray &&hash,
		qint64 size,
		uint64 mediaId,
		bool needsDelete) {
	if (i < 0 || i >= _n) return;
	auto &item = _items[i];
	item.dedupHashPending = false;
	item.fileHash = hash;
	item.fileSize = size;
	if (hash.isEmpty() || !mediaId || item.uploadDone || item.dedupSkipped) {
		pumpUploads();
		return;
	}
	auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
	if (!dedupDb.isOpen()) {
		pumpUploads();
		return;
	}
	// The seek runs before this id is registered, so it can never match
	// itself: any hit is a genuinely different source doc with the same
	// content. A previous 'f' row of this exact id is caught by selfDone.
	const auto selfDone = (dedupDb.hashForDocId(
		Data::DedupDb::Table::Uploads,
		mediaId) == hash);
	const auto duplicateId = dedupDb.seekDocumentId(
		Data::DedupDb::Table::Uploads,
		hash,
		0);
	if (duplicateId || selfDone) {
		if (needsDelete) {
			QFile::remove(item.path);
		}
		skipAsDuplicate(i);
		return;
	}
	dedupDb.addPending(
		Data::DedupDb::Table::Uploads,
		mediaId,
		hash);
	pumpUploads();
}

void Pipeline::premarkDuplicate(int i) {
	auto &item = _items[i];
	if (item.dedupSkipped) {
		return;
	}
	item.dedupSkipped = true;
	item.downloadDone = true;
	item.uploadDone = true;
	item.sent = true;
	_skippedCount++;
	adjustAlbumCount(i);
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	auto &states = ActiveStates();
	const auto sit = states.find(_peerId);
	if (sit != states.end() && i >= 0 && i < int(sit->second.items.size())) {
		sit->second.items[i].dedupSkipped = true;
		sit->second.items[i].sent = true;
	}
	EnhancedForward::markItemSkipped(&_session, _peerId);
	// Attach this doc id to the existing content hash so a future forward of
	// the exact same item is caught by the O(1) id lookup without hashing.
	// Persisted only when the matched content is finished: an alias based on
	// an in-flight row would outlive that row's cancel and block this
	// content forever with nothing actually uploaded.
	if (item.mediaId && !item.fileHash.isEmpty()) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()
			&& db.containsFinishedHash(
				Data::DedupDb::Table::Uploads,
				item.fileHash)) {
			db.insert(Data::DedupDb::Table::Uploads, {
				.hash = item.fileHash,
				.documentId = item.mediaId,
				.status = u"f"_q,
			});
		}
	}
	saveProgress();
}

void Pipeline::startSession() {
	const auto self = shared_from_this();
	std::vector<FullMsgId> sourceIds;
	sourceIds.reserve(_n);
	for (auto i = 0; i < _n; i++) {
		sourceIds.push_back(_items[i].sourceId);
	}
	EnhancedForward::startForwardSession(
		&_session,
		_peerId,
		_srcPeer,
		sourceIds,
		[self] { self->saveProgress(); });
	auto &states = ActiveStates();
	const auto sit = states.find(_peerId);
	if (sit != states.end()) {
		auto &state = sit->second;
		for (auto i = 0; i < _n; i++) {
			if (_items[i].dedupSkipped && i < int(state.items.size())) {
				state.items[i].dedupSkipped = true;
				state.items[i].sent = true;
			}
		}
		state.total = 0;
		state.skipped = 0;
		state.sent = 0;
		for (auto i = 0; i < _n; i++) {
			if (_items[i].dedupSkipped || _items[i].cancelled) {
				state.skipped++;
			} else {
				state.total++;
			}
		}
		EnhancedForward::NotifyStateChanged(_peerId);
		EnhancedForward::NotifyCounterChanged();
		saveProgress();
		// If everything was dedup-skipped or cancelled up front there is
		// nothing to send: finish right away so the job doesn't linger as
		// "active", which would block new forwards (they'd be queued) and
		// trigger the quit "unfinished forward" prompt.
		if (state.total == 0) {
			EnhancedForward::finishJob(&_session, _peerId);
			// Nothing was downloaded, but the temp folder was created during
			// setup - remove it so an all-duplicates forward leaves no trace.
			const auto session = &_session;
			const auto path = _downloadPath;
			crl::on_main([=] {
				CleanupLeftoverForwardFiles(session, path);
			});
			return;
		}
	}
	// The pause/resume/cancel callbacks must be installed only after the
	// ActiveStates entry exists (startForwardSession above), otherwise the
	// setters bail out early and Pause never stops the in-flight transfers.
	setupCallbacks();
	pumpDownloads();
	pumpUploads();
	sendNext();
}

void Pipeline::appendItems(
		std::vector<not_null<HistoryItem*>> &&items) {
	if (items.empty()) {
		return;
	}
	for (const auto &srcItem : items) {
		if (_session.data().message(srcItem->fullId()) != srcItem) {
			continue;
		}
		ItemTask it;
		it.sourceId = srcItem->fullId();
		const auto media = srcItem->media();
		if (!media) {
			it.textOnly = true;
		} else if (auto doc = media->document()) {
			it.mediaId = uint64(doc->id);
			const auto localPath = doc->filepath(true);
			if (!localPath.isEmpty()) {
				it.path = localPath;
				it.downloadDone = true;
			} else {
				auto name = doc->filename();
				if (name.isEmpty()) {
					name = u"file"_q;
				}
				name.replace(QRegularExpression("[:<>\"\\\\|?*]"), "_");
				it.path = QDir(_downloadPath).absoluteFilePath(
					QString::number(_runId) + u"_"_q
					+ QString::number(_items.size()) + u"_"_q + name);
				QDir().mkpath(QFileInfo(it.path).absolutePath());
				it.needsDownload = true;
			}
		} else if (auto photo = media->photo()) {
			it.isPhoto = true;
			it.mediaId = uint64(photo->id);
			it.path = QDir(_downloadPath).absoluteFilePath(
				QString::number(_runId) + u"_"_q
				+ QString::number(_items.size()) + u"_"_q
				+ QString::number(photo->id) + u".jpg"_q);
			QDir().mkpath(QFileInfo(it.path).absolutePath());
			it.needsDownload = true;
		} else {
			it.textOnly = true;
		}
		_items.push_back(std::move(it));
	}
	const auto oldN = _n;
	_n = int(_items.size());
	// Extend the visible job state so the Forwards tab and the counters see
	// the newly added items right away and keep them after the current item.
	if (const auto j = ActiveStates().find(_peerId); j != end(ActiveStates())) {
		auto &state = j->second;
		for (auto i = oldN; i < _n; i++) {
			state.sourceIds.push_back(_items[i].sourceId);
			auto tracked = TrackedItem();
			if (const auto srcItem = _session.data().message(_items[i].sourceId)) {
				if (const auto media = srcItem->media()) {
					if (const auto doc = media->document()) {
						tracked.info.name = doc->filename();
						tracked.info.size = doc->size;
					} else if (const auto photo = media->photo()) {
						tracked.info.name = QString::number(photo->id)
							+ u".jpg"_q;
					}
				}
			}
			state.items.push_back(tracked);
			state.total++;
		}
	}
	NotifyStateChanged(_peerId);
	NotifyCounterChanged();
	if (!EnhancedForward::isPaused(_peerId)) {
		pumpDownloads();
		pumpUploads();
		sendNext();
	}
}

void Pipeline::runNextPrecheck() {
	if (_dedupPrecheckQueue.empty()) {
		startSession();
		return;
	}
	const auto i = _dedupPrecheckQueue.front();
	_dedupPrecheckQueue.pop_front();
	auto &item = _items[i];
	if (item.dedupSkipped || item.downloadDone || item.mediaId == 0) {
		runNextPrecheck();
		return;
	}
	// ID fast path: the exact same source document was already forwarded (or
	// is currently being forwarded), so skip it without computing a hash.
	auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
	if (dedupDb.isOpen()
		&& (dedupDb.containsDocId(Data::DedupDb::Table::Uploads, item.mediaId)
			|| dedupDb.containsDocIdInDb(
				Data::DedupDb::Table::Uploads,
				item.mediaId))) {
		premarkDuplicate(i);
		runNextPrecheck();
		return;
	}
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	if (!doc) {
		runNextPrecheck();
		return;
	}
	const auto size = qint64(doc->size);
	const auto self = shared_from_this();
	Data::RemoteFileFingerprint(&_session, doc, [self, i, size](QByteArray hash) {
		if (const auto alive = self.get()) {
			alive->remotePrechecked(i, std::move(hash), size);
		}
	});
}

void Pipeline::remotePrechecked(int i, QByteArray &&hash, qint64 size) {
	if (i < 0 || i >= _n) {
		runNextPrecheck();
		return;
	}
	auto &item = _items[i];
	if (hash.isEmpty() || item.dedupSkipped || item.downloadDone) {
		runNextPrecheck();
		return;
	}
	item.fileHash = hash;
	item.fileSize = size;
	item.dedupPrechecked = true;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		runNextPrecheck();
		return;
	}
	// This doc reached here only if its own id was NOT registered (the id fast
	// path above already skips it otherwise), so a content-hash match means a
	// different source doc with the same content was forwarded before: skip it
	// and attach this id to the existing hash for future id-only dedup.
	const auto duplicateId = db.seekDocumentId(
		Data::DedupDb::Table::Uploads,
		hash,
		0);
	if (duplicateId) {
		premarkDuplicate(i);
	} else {
		db.addPending(Data::DedupDb::Table::Uploads, item.mediaId, hash);
		item.dedupNeedsHash = false;
	}
	runNextPrecheck();
}

void Pipeline::skipAsDuplicate(int i) {
	const auto self = shared_from_this();
	auto &item = _items[i];
	item.dedupSkipped = true;
	item.downloadDone = true;
	item.uploadDone = true;
	item.sent = true;
	_downloadInFlight = false;
	_uploadInFlight = false;
	_skippedCount++;
	adjustAlbumCount(i);
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	refreshSourceItemState(i);
	if (item.mediaId && !item.fileHash.isEmpty()) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()
			&& db.containsFinishedHash(
				Data::DedupDb::Table::Uploads,
				item.fileHash)) {
			// Same as downloads: keep the new id -> same content mapping so
			// a future forward of this exact media is skipped O(1). Finished
			// match only - see premarkDuplicate().
			db.insert(Data::DedupDb::Table::Uploads, {
				.hash = item.fileHash,
				.documentId = item.mediaId,
				.status = u"f"_q,
			});
		}
	}
	auto &states = ActiveStates();
	const auto sit = states.find(_peerId);
	if (sit != states.end() && i >= 0 && i < int(sit->second.items.size())) {
		sit->second.items[i].dedupSkipped = true;
		sit->second.items[i].sent = true;
	}
	EnhancedForward::markItemSkipped(&_session, _peerId);
	saveProgress();
	sendNext();
	pumpUploads();
	pumpDownloads();
}

void Pipeline::adjustAlbumCount(int idx) {
	if (idx < 0 || idx >= _n) return;
	const auto &item = _items[idx];
	if (!item.sourceGroup) return;
	const auto albumIt = _albums.find(item.sourceGroup);
	if (albumIt != _albums.end() && albumIt->second->expectedCount > 0) {
		albumIt->second->expectedCount--;
	}
}

void Pipeline::cancelItem(int idx) {
	const auto self = shared_from_this();
	if (idx < 0 || idx >= _n) return;
	auto &item = _items[idx];
	if (item.cancelled || item.sent) return;
	item.cancelled = true;
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	if (!item.downloadDone && srcItem) {
		if (const auto doc = media ? media->document() : nullptr) {
			doc->cancel();
		} else if (const auto photo = media ? media->photo() : nullptr) {
			photo->cancel();
		}
	}
	if (item.uploadId != FullMsgId()) {
		_session.uploader().cancel(item.uploadId);
		_uploadIndex->erase(item.uploadId);
	}
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		ClearShadowUpload(&_session, srcItem);
	}
	if (GetEnhancedBool("prevent_forward_duplicates")
		&& !item.uploadDone
		&& item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.removePending(Data::DedupDb::Table::Uploads, item.mediaId);
		}
	}
	{
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			const auto jobId = u"ef_%1_%2"_q.arg(_srcPeer.value).arg(
				_peerId.value);
			db.removeEfResumeItem(jobId, idx);
		}
	}
	if (!item.path.isEmpty() && item.path.startsWith(_downloadPath)) {
		RemoveEfTempFile(item.path);
	}
	if (srcItem) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	adjustAlbumCount(idx);
	refreshSourceItemState(idx);

	item.downloadDone = true;
	item.uploadDone = true;
	item.sent = true;
	if (_downloadCursor > 0 && _downloadCursor - 1 == idx) {
		_downloadInFlight = false;
	}
	if (_uploadCursor > 0 && _uploadCursor - 1 == idx) {
		_uploadInFlight = false;
	}

	auto &states = ActiveStates();
	const auto stateIt = states.find(_peerId);
	if (stateIt != states.end()
		&& idx >= 0 && idx < int(stateIt->second.items.size())) {
		auto &tracked = stateIt->second.items[idx];
		tracked.cancelled = true;
		tracked.state = ItemState::Done;
		tracked.downloadProgress = 1.0;
		tracked.uploadProgress = 1.0;
		tracked.progress = 1.0;
	}

	EnhancedForward::markItemSkipped(&_session, _peerId);
	saveProgress();
	sendNext();
	pumpUploads();
	pumpDownloads();
}

} // namespace EnhancedForward

namespace NormalForward {
namespace {

constexpr auto kMaxBatchIds = 100;
constexpr auto kMaxAlbumIds = 10;
constexpr auto kFingerprintConcurrencyBase = 50;
constexpr auto kFingerprintConcurrencyMax = 100;
constexpr auto kMaxFingerprintRetries = 3;
constexpr auto kGeneralId = Data::ForumTopic::kGeneralId;

enum class AlbumKind : uint8 {
	NotGroupable,
	AlbumsPhoto,
	AlbumsVideo,
	AlbumsMusic,
	AlbumsDocuments,
};

[[nodiscard]] AlbumKind KindOf(const Data::Media *m) {
	if (!m) return AlbumKind::NotGroupable;
	if (m->photo()) return AlbumKind::AlbumsPhoto;
	if (const auto d = m->document()) {
		if (d->isVideoFile()) return AlbumKind::AlbumsVideo;
		if (d->isSong()) return AlbumKind::AlbumsMusic;
		if (m->canBeGrouped()) return AlbumKind::AlbumsDocuments;
	}
	return AlbumKind::NotGroupable;
}

rpl::event_stream<> CountersChanged;

void Notify() {
	CountersChanged.fire({});
}

struct Registration {
	uint64 mediaId = 0;
	QByteArray hash;
};

class Job final : public std::enable_shared_from_this<Job> {
public:
	enum class State {
		Running,
		FloodWait,
		Paused,
		Done,
		Cancelled,
	};

	Job(
		not_null<Main::Session*> session,
		ApiWrap *api,
		PeerId dst,
		PeerId src,
		Api::SendAction action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions)
	: session(session)
	, api(api)
	, dst(dst)
	, src(src)
	, action(std::move(action))
	, forwardOptions(forwardOptions)
	, groupOptions(groupOptions) {
	}

	Main::Session *session = nullptr;
	ApiWrap *api = nullptr;

	PeerId dst = PeerId();
	PeerId src = PeerId();
	Api::SendAction action;
	Data::ForwardOptions forwardOptions = Data::ForwardOptions();
	Data::GroupingOptions groupOptions = Data::GroupingOptions();

	State state = State::Running;

	int total = 0;
	int done = 0;
	int skipped = 0;
	MsgId lastMsgId = 0;
	QString lastFileName;
	int floodSeconds = 0;

	std::vector<MsgId> remaining;   // source ids not yet acked
	std::vector<MsgId> queue;       // natural singles, batchable
	std::vector<MessageGroupId> queueGroups; // source groupId per queue entry
	std::vector<MsgId> soloQueue;   // album members, 1 per request
	base::flat_map<MsgId, Registration> regs;
	base::flat_set<MsgId> awaitingHash;
	base::flat_map<MsgId, QByteArray> hashes;
	base::flat_map<MsgId, uint64> mediaIds;
	size_t scanned = 0;             // prepare cursor over remaining
	int inflightFetches = 0;
	int soloInFlight = 0;
	constexpr static auto kMaxSoloConcurrent = 10;
	bool batchInFlight = false;
	bool pumping = false;
	bool completionFired = false;
	base::flat_map<MsgId, int> fingerprintRetries;
	std::vector<MsgId> hashRetries;

	std::vector<mtpRequestId> requests;
	base::Timer floodTimer;
	base::Timer tickTimer;
	std::optional<TimeId> videoTimestamp;

	Fn<void()> completion;

	[[nodiscard]] int maxBatch() const {
		if (groupOptions == Data::GroupingOptions::Separate) {
			return 1;
		}
		if (groupOptions == Data::GroupingOptions::RegroupAll) {
			return kMaxAlbumIds;
		}
		return kMaxBatchIds;
	}

	[[nodiscard]] bool alive() const {
		return state == State::Running || state == State::FloodWait;
	}
};

using JobsMap = base::flat_map<PeerId, std::shared_ptr<Job>>;

JobsMap &Jobs() {
	static JobsMap result;
	return result;
}

[[nodiscard]] bool BelongsTo(
		const Job &job,
		not_null<Main::Session*> session) {
	return job.session == session.get();
}

void FireCompletion(not_null<Job*> j) {
	if (!j->completionFired) {
		j->completionFired = true;
		if (j->completion) {
			j->completion();
		}
	}
}

void SaveSnapshot(not_null<Job*> j) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto record = Data::NfResumeRecord();
	record.sessionId = j->session->uniqueId();
	record.destPeerId = j->dst;
	record.srcPeerId = j->src;
	record.total = j->total;
	record.done = j->done;
	record.skipped = j->skipped;
	record.lastMsgId = j->lastMsgId;
	switch (j->state) {
	case Job::State::FloodWait: record.state = u"flood"_q; break;
	case Job::State::Paused: record.state = u"paused"_q; break;
	default: record.state = u"running"_q; break;
	}
	record.remaining = j->remaining;
	db.insertNfResume(record);
}

void Pump(not_null<Job*> j);

void StopTimers(not_null<Job*> j) {
	j->floodTimer.cancel();
	j->tickTimer.cancel();
}

void Finish(not_null<Job*> j) {
	j->state = Job::State::Done;
	for (const auto requestId : base::take(j->requests)) {
		j->api->request(requestId).cancel();
	}
	StopTimers(j);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &[msgId, registration] : j->regs) {
		db.removePending(
			Data::DedupDb::Table::Uploads,
			registration.mediaId);
	}
	j->regs.clear();
	db.removeNfResume(j->session->uniqueId(), j->dst);
	if (j->done > 0 || j->skipped > 0) {
		EnhancedForward::ShowForwardDoneToast(j->done, j->total, j->skipped);
	}
	FireCompletion(j);
	Jobs().remove(j->dst);
	Notify();
}

void PauseJob(not_null<Job*> j) {
	if (j->state != Job::State::Running) {
		return;
	}
	j->state = Job::State::Paused;
	SaveSnapshot(j);
	FireCompletion(j);
	Notify();
}

void CancelJob(not_null<Job*> j) {
	j->state = Job::State::Cancelled;
	for (const auto requestId : base::take(j->requests)) {
		j->api->request(requestId).cancel();
	}
	StopTimers(j);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &[msgId, registration] : j->regs) {
		db.removePending(
			Data::DedupDb::Table::Uploads,
			registration.mediaId);
	}
	j->regs.clear();
	db.removeNfResume(j->session->uniqueId(), j->dst);
	Jobs().remove(j->dst);
	FireCompletion(j);
	Notify();
}

void PauseJob(not_null<Job*> j);

void EnqueueNatural(not_null<Job*> j, MsgId msgId) {
	const auto item = j->session->data().message(FullMsgId(j->src, msgId));
	j->queue.push_back(msgId);
	j->queueGroups.push_back(
		item ? item->groupId() : MessageGroupId());
}

void EnqueueResolved(
		not_null<Job*> j,
		MsgId msgId,
		const QByteArray &hash) {
	j->awaitingHash.remove(msgId);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	const auto duplicate = !hash.isEmpty()
		&& db.seekDocumentId(Data::DedupDb::Table::Uploads, hash) != 0;
	if (duplicate) {
		j->skipped++;
		return;
	}
	const auto mediaIt = j->mediaIds.find(msgId);
	const auto mediaId = (mediaIt != end(j->mediaIds))
		? mediaIt->second
		: uint64(0);
	if (mediaId && !hash.isEmpty()) {
		db.addPending(Data::DedupDb::Table::Uploads, mediaId, hash);
		j->regs.emplace_or_assign(msgId, Registration{ mediaId, hash });
	}
	const auto item = j->session->data().message(
		FullMsgId(j->src, msgId));
	const auto isSolo = j->groupOptions == Data::GroupingOptions::Separate
		&& item
		&& item->groupId() != MessageGroupId();
	if (isSolo) {
		j->soloQueue.push_back(msgId);
	} else {
		EnqueueNatural(j, msgId);
	}
}

void StartFingerprint(
		not_null<Job*> j,
		MsgId msgId,
		not_null<DocumentData*> document) {
	auto &retries = j->fingerprintRetries[msgId];
	if (retries >= kMaxFingerprintRetries) {
		PauseJob(j);
		return;
	}
	retries++;
	j->inflightFetches++;
	j->awaitingHash.insert(msgId);
	const auto weak = j->weak_from_this();
	Data::RemoteFileFingerprint(j->session, document, [=](QByteArray hash) {
		if (const auto strong = weak.lock()) {
			strong->inflightFetches--;
			if (hash.isEmpty()) {
				strong->awaitingHash.remove(msgId);
				strong->hashRetries.push_back(msgId);
				PauseJob(strong.get());
				return;
			}
			strong->fingerprintRetries.remove(msgId);
			strong->hashes.emplace_or_assign(msgId, std::move(hash));
			EnqueueResolved(strong.get(), msgId, strong->hashes[msgId]);
			Pump(strong.get());
		}
	});
}

void StartPhotoFingerprint(
		not_null<Job*> j,
		MsgId msgId,
		not_null<PhotoData*> photo) {
	auto &retries = j->fingerprintRetries[msgId];
	if (retries >= kMaxFingerprintRetries) {
		PauseJob(j);
		return;
	}
	retries++;
	j->inflightFetches++;
	j->awaitingHash.insert(msgId);
	const auto weak = j->weak_from_this();
	Data::RemotePhotoFingerprint(j->session, photo, [=](QByteArray hash) {
		if (const auto strong = weak.lock()) {
			strong->inflightFetches--;
			if (hash.isEmpty()) {
				strong->awaitingHash.remove(msgId);
				strong->hashRetries.push_back(msgId);
				PauseJob(strong.get());
				return;
			}
			strong->fingerprintRetries.remove(msgId);
			strong->hashes.emplace_or_assign(msgId, std::move(hash));
			EnqueueResolved(strong.get(), msgId, strong->hashes[msgId]);
			Pump(strong.get());
		}
	});
}

[[nodiscard]] MTPInputMedia InputMediaForMedia(
		not_null<Data::Session*> data,
		const Data::Media *media) {
	if (const auto photo = media->photo()) {
		return MTP_inputMediaPhoto(
			MTP_flags(0),
			photo->mtpInput(),
			MTPint(),
			MTPInputDocument());
	} else if (const auto document = media->document()) {
		return MTP_inputMediaDocument(
			MTP_flags(0),
			document->mtpInput(),
			MTPInputPhoto(),
			MTPint(),
			MTPint(),
			MTPstring());
	}
	return MTP_inputMediaEmpty();
}

[[nodiscard]] QString DescribeAcks(const MTPUpdates &result) {
	auto line = QString();
	const auto addMessage = [&](const MTPMessage &message) {
		message.match([&](const MTPDmessage &data) {
			line += u" %1:g%2"_q.arg(data.vid().v).arg(
				data.vgrouped_id().value_or_empty());
		}, [&](const MTPDmessageEmpty &) {
			line += u" ?:e"_q;
		}, [&](const MTPDmessageService &data) {
			line += u" %1:s"_q.arg(data.vid().v);
		});
	};
	result.match([&](const MTPDupdates &data) {
		for (const auto &update : data.vupdates().v) {
			update.match([&](const MTPDupdateNewMessage &d) {
				addMessage(d.vmessage());
			}, [&](const MTPDupdateNewChannelMessage &d) {
				addMessage(d.vmessage());
			}, [&](const auto &) {});
		}
	}, [&](const MTPDupdatesCombined &data) {
		for (const auto &update : data.vupdates().v) {
			update.match([&](const MTPDupdateNewMessage &d) {
				addMessage(d.vmessage());
			}, [&](const MTPDupdateNewChannelMessage &d) {
				addMessage(d.vmessage());
			}, [&](const auto &) {});
		}
	}, [&](const auto &) {});
	return line;
}

void FlushForwardBatch(
		not_null<Job*> j,
		std::vector<MsgId> chunk) {
	Expects(!chunk.empty());

	const auto history = j->session->data().history(j->dst);
	const auto srcPeer = j->session->data().peer(j->src);

	auto idsVector = QVector<MTPint>();
	auto randoms = QVector<MTPlong>();
	idsVector.reserve(int(chunk.size()));
	randoms.reserve(int(chunk.size()));
	for (const auto msgId : chunk) {
		idsVector.push_back(MTP_int(msgId.bare));
		randoms.push_back(MTP_long(base::RandomValue<uint64>()));
	}

	const auto topicRootId = j->action.replyTo.topicRootId;
	const auto realTopMsgId = (topicRootId == kGeneralId)
		? MsgId(0)
		: topicRootId;
	using Flag = MTPmessages_ForwardMessages::Flag;
	auto approved = j->action.options.starsApproved;
	const auto starsPaid = std::min(
		history->peer->starsPerMessageChecked(),
		approved);
	if (starsPaid) {
		approved -= starsPaid;
		j->action.options.starsApproved = approved;
	}
	auto flags = Flag(0)
		| Flag::f_with_my_score
		| (ShouldSendSilent(history->peer, j->action.options)
			? Flag::f_silent
			: Flag(0))
		| (realTopMsgId ? Flag::f_top_msg_id : Flag(0))
		| ((j->action.options.sendAs)
			? Flag::f_send_as
			: Flag(0))
		| (j->forwardOptions != Data::ForwardOptions::PreserveInfo
			? Flag::f_drop_author
			: Flag(0))
		| (j->forwardOptions == Data::ForwardOptions::NoNamesAndCaptions
			? Flag::f_drop_media_captions
			: Flag(0))
		| (j->videoTimestamp.has_value()
			? Flag::f_video_timestamp
			: Flag(0))
		| (j->action.options.shortcutId
			? Flag::f_quick_reply_shortcut
			: Flag(0))
		| (j->action.options.scheduled
			? Flag::f_schedule_date
			: Flag(0))
		| ((j->action.options.scheduled
				&& j->action.options.scheduleRepeatPeriod)
			? Flag::f_schedule_repeat_period
			: Flag(0))
		| (starsPaid ? Flag::f_allow_paid_stars : Flag(0));

	const auto maxSourceId = chunk.back();

	const auto weak = j->weak_from_this();
	const auto requestId = j->api->request(
		MTPmessages_ForwardMessages(
			MTP_flags(flags),
			srcPeer->input(),
			MTP_vector<MTPint>(idsVector),
			MTP_vector<MTPlong>(randoms),
			history->peer->input(),
			MTP_int(realTopMsgId.bare),
			ReplyToForMTP(history, j->action.replyTo),
			MTP_int(j->action.options.scheduled),
			MTP_int(j->action.options.scheduleRepeatPeriod),
			(j->action.options.sendAs
				? j->action.options.sendAs->input()
				: MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(
				j->session,
				j->action.options.shortcutId),
			MTP_long(j->action.options.effectId),
			MTP_int(j->videoTimestamp.value_or(0)),
			MTP_long(starsPaid),
			Api::SuggestToMTP(j->action.options.suggest))
	).done([=](
			const MTPUpdates &result) mutable {
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		auto job = not_null{ strong.get() };
		job->api->applyUpdates(result);
		job->done += int(chunk.size());
		job->lastMsgId = std::max(job->lastMsgId, maxSourceId);
		for (const auto msgId : chunk) {
			const auto item = job->session->data().message(
				FullMsgId(job->src, msgId));
			const auto media = item ? item->media() : nullptr;
			if (!media) {
				continue;
			} else if (const auto doc = media->document()) {
				job->lastFileName = doc->filename();
			} else if (media->photo()) {
				job->lastFileName = u"photo_%1.jpg"_q.arg(msgId.bare);
			}
		}
		auto &db = Core::App().downloadManager().ensureDedupDb();
		for (const auto msgId : chunk) {
			if (const auto it = job->regs.find(msgId); it != end(job->regs)) {
				db.insert(Data::DedupDb::Table::Uploads, {
					.hash = it->second.hash,
					.documentId = it->second.mediaId,
					.status = u"f"_q,
				});
				db.removePending(
					Data::DedupDb::Table::Uploads,
					it->second.mediaId);
				job->regs.erase(it);
			}
		}
		job->batchInFlight = false;
		if (job->soloInFlight > 0) {
			job->soloInFlight--;
		}
		job->session->data().sendHistoryChangeNotifications();
		SaveSnapshot(job);
		Notify();
		Pump(job);
	}).fail([=](
			const MTP::Error &error) mutable {
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		auto job = not_null{ strong.get() };
		job->batchInFlight = false;
		for (auto it = chunk.rbegin(); it != chunk.rend(); ++it) {
			job->queue.insert(job->queue.begin(), *it);
			const auto item = job->session->data().message(
				FullMsgId(job->src, *it));
			job->queueGroups.insert(
				job->queueGroups.begin(),
				item ? item->groupId() : MessageGroupId());
		}
		const auto &type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			job->floodSeconds = type.mid(u"FLOOD_WAIT_"_q.size()).toInt();
			job->state = Job::State::FloodWait;
			SaveSnapshot(job);
			Notify();
			j->floodTimer.setCallback([weak] {
				if (const auto strong = weak.lock()) {
					if (strong->state == Job::State::FloodWait) {
						strong->floodSeconds = 0;
						strong->tickTimer.cancel();
						strong->state = Job::State::Running;
						SaveSnapshot(strong.get());
						Notify();
						Pump(strong.get());
					}
				}
			});
			j->floodTimer.callOnce((job->floodSeconds + 1) * 1000);
			j->tickTimer.setCallback([weak] {
				if (const auto strong = weak.lock()) {
					if (strong->floodSeconds > 0) {
						strong->floodSeconds--;
						Notify();
					}
				}
			});
			j->tickTimer.callEach(1000);
			return;
		}
		j->api->sendMessageFail(error, history->peer);
		PauseJob(j);
	}).send();
	j->requests.push_back(requestId);
	j->batchInFlight = true;
}

void Pump(not_null<Job*> j) {
	if (j->state != Job::State::Running
		|| j->pumping
		|| j->batchInFlight) {
		return;
	}
	// Separate-mode solo sends (1 id each) are independent — no
	// batchInFlight serialization needed. Batched modes serialize.
	j->pumping = true;
	const auto guard = gsl::finally([&] { j->pumping = false; });

	auto &db = Core::App().downloadManager().ensureDedupDb();
	const auto dedupOn = GetEnhancedBool("prevent_forward_duplicates");
	const auto windowSize = j->maxBatch();
	// Adaptive concurrency: when both send queues are empty the pipeline
	// is starved, so allow more fingerprint fetches to catch up.
	const auto maxInflight = (j->queue.empty() && j->soloQueue.empty())
		? kFingerprintConcurrencyMax
		: kFingerprintConcurrencyBase;

	// Check-ahead pipeline: fill the queue up to one batch ahead.
	// Doc-id check is instant; fingerprints are bounded to 2 concurrent.
	// The flush fires as soon as the queue reaches the target size.
	while (int(j->queue.size()) + int(j->soloQueue.size()) < windowSize
		&& j->scanned < j->remaining.size()) {
		const auto msgId = j->remaining[j->scanned];

		if (!dedupOn) {
			j->scanned++;
			EnqueueNatural(j, msgId);
			continue;
		}

		if (const auto it = j->hashes.find(msgId); it != end(j->hashes)) {
			j->scanned++;
			EnqueueResolved(j, msgId, it->second);
			continue;
		}

		const auto item = j->session->data().message(
			FullMsgId(j->src, msgId));
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;

		if (j->awaitingHash.contains(msgId)) {
			j->scanned++;
			continue;
		}

		if (!document && !photo) {
			j->scanned++;
			EnqueueNatural(j, msgId);
			continue;
		}

		const auto mediaId = document
			? uint64(document->id)
			: uint64(photo->id);
		if (db.containsDocId(Data::DedupDb::Table::Uploads, mediaId)) {
			j->scanned++;
			j->skipped++;
			continue;
		}

		if (j->inflightFetches >= maxInflight) {
			break;
		}

		j->mediaIds.emplace(msgId, mediaId);
		j->awaitingHash.insert(msgId);
		if (document) {
			StartFingerprint(j, msgId, document);
		} else {
			StartPhotoFingerprint(j, msgId, photo);
		}
		j->scanned++;
		continue;
	}

	if (!j->soloQueue.empty()) {
		// Album members go one per request (breaks grouping).
		auto chunk = std::vector<MsgId>(
			begin(j->soloQueue),
			begin(j->soloQueue) + 1);
		j->soloQueue.erase(begin(j->soloQueue));
		j->soloInFlight++;
		FlushForwardBatch(j, std::move(chunk));
		return;
	}

	if (!j->queue.empty()) {
		// Ensure ascending msgId order regardless of fingerprint
		// callback completion order, keeping groupIds paired.
		auto paired = std::vector<std::pair<MsgId, MessageGroupId>>();
		paired.reserve(j->queue.size());
		for (auto i = size_t(0); i < j->queue.size(); ++i) {
			paired.emplace_back(j->queue[i], j->queueGroups[i]);
		}
		std::sort(begin(paired), end(paired));
		for (auto i = size_t(0); i < paired.size(); ++i) {
			j->queue[i] = paired[i].first;
			j->queueGroups[i] = paired[i].second;
		}
		const auto readyToFlush = int(j->queue.size()) >= windowSize
			|| (j->scanned >= j->remaining.size()
				&& j->inflightFetches == 0
				&& j->awaitingHash.empty());
		if (!readyToFlush) {
			return;
		}
		auto count = std::min(j->queue.size(), size_t(windowSize));
		for (auto i = size_t(0); i < count; ++i) {
			if (i > 0
				&& j->groupOptions == Data::GroupingOptions::RegroupAll) {
				const auto prev = j->queueGroups[i - 1];
				const auto cur = j->queueGroups[i];
				const auto isSameAlbum = prev != MessageGroupId()
					&& cur != MessageGroupId()
					&& prev == cur;
				if (isSameAlbum) {
					if (i >= kMaxAlbumIds) {
						count = i;
						break;
					}
					continue;
				}
				const auto boundary = (prev != MessageGroupId())
					? (cur != prev)
					: (cur != MessageGroupId());
				if (boundary) {
					count = i;
					break;
				}
				const auto prevItem = j->session->data().message(
					FullMsgId(j->src, j->queue[i - 1]));
				const auto curItem = j->session->data().message(
					FullMsgId(j->src, j->queue[i]));
				const auto prevMedia = prevItem ? prevItem->media() : nullptr;
				const auto curMedia = curItem ? curItem->media() : nullptr;
				const auto prevKind = KindOf(prevMedia);
				const auto curKind = KindOf(curMedia);
				auto isGrouped = [&](AlbumKind k) {
					if (k == AlbumKind::NotGroupable) return false;
					if (j->groupOptions == Data::GroupingOptions::RegroupAll
						&& j->forwardOptions != Data::ForwardOptions::PreserveInfo) {
						return k == AlbumKind::AlbumsPhoto
							|| k == AlbumKind::AlbumsVideo
							|| k == AlbumKind::AlbumsMusic
							|| k == AlbumKind::AlbumsDocuments;
					}
					if (k == AlbumKind::AlbumsPhoto
						|| k == AlbumKind::AlbumsVideo) return false;
					return k == AlbumKind::AlbumsMusic
						|| k == AlbumKind::AlbumsDocuments;
				};
				if (!isGrouped(prevKind)
					|| !isGrouped(curKind)
					|| prevKind != curKind) {
					count = i;
					break;
				}
			}
		}
		auto chunk = std::vector<MsgId>(
			begin(j->queue),
			begin(j->queue) + count);
		if (j->groupOptions == Data::GroupingOptions::GroupAsIs
			&& chunk.size() > 10) {
			std::reverse(begin(chunk), end(chunk));
		}
		j->queue.erase(
			begin(j->queue),
			begin(j->queue) + count);
		j->queueGroups.erase(
			begin(j->queueGroups),
			begin(j->queueGroups) + count);
		j->batchInFlight = true;
		FlushForwardBatch(j, std::move(chunk));
		return;
	}

	if (j->scanned >= j->remaining.size()
		&& j->inflightFetches == 0
		&& j->awaitingHash.empty()
		&& j->queue.empty()
		&& j->soloQueue.empty()) {
		Finish(j);
	}
}
} // namespace

void RestartFailedFingerprints(not_null<Job*> j) {
	if (j->hashRetries.empty()) {
		return;
	}
	auto failed = base::take(j->hashRetries);
	for (const auto msgId : failed) {
		if (j->state != Job::State::Running) {
			return;
		}
		const auto item = j->session->data().message(
			FullMsgId(j->src, msgId));
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;
		if (document) {
			StartFingerprint(j, msgId, document);
		} else if (photo) {
			StartPhotoFingerprint(j, msgId, photo);
		}
	}
}

void Start(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		Fn<void()> completion,
		std::optional<TimeId> videoTimestamp) {
	if (items.empty()) {
		if (completion) {
			completion();
		}
		return;
	}
	const auto session = not_null{ &items.front()->history()->session() };
	const auto dst = action.history->peer->id;
	const auto src = items.front()->history()->peer->id;

	auto remaining = std::vector<MsgId>();
	remaining.reserve(items.size());
	for (const auto &item : items) {
		remaining.push_back(item->fullId().msg);
	}
	std::sort(remaining.begin(), remaining.end());

	auto &jobs = Jobs();
	if (const auto it = jobs.find(dst); it != end(jobs)) {
		const auto job = not_null{ it->second.get() };
		job->total += int(remaining.size());
		job->remaining.insert(
			end(job->remaining),
			std::make_move_iterator(begin(remaining)),
			std::make_move_iterator(end(remaining)));
		std::sort(job->remaining.begin(), job->remaining.end());
		if (job->state == Job::State::Paused) {
			job->state = Job::State::Running;
		}
		RestartFailedFingerprints(job);
		SaveSnapshot(job);
		Notify();
		Pump(job);
		return;
	}

	auto job = std::make_shared<Job>(
		session,
		api,
		dst,
		src,
		action,
		forwardOptions,
		groupOptions);
	if (completion) {
		job->completion = std::move(completion);
	}
	job->total = int(remaining.size());
	job->remaining = std::move(remaining);
	job->videoTimestamp = videoTimestamp;
	jobs.emplace(dst, std::move(job));
	auto &stored = jobs.find(dst)->second;
	SaveSnapshot(stored.get());
	Notify();
	Pump(stored.get());
}

void PauseAll(not_null<Main::Session*> session) {
	auto &jobs = Jobs();
	for (auto it = begin(jobs); it != end(jobs);) {
		const auto job = it->second.get();
		if (BelongsTo(*job, session) && job->alive()) {
			for (const auto requestId : base::take(job->requests)) {
				job->api->request(requestId).cancel();
			}
			StopTimers(job);
			job->batchInFlight = false;
			job->state = Job::State::Paused;
			SaveSnapshot(job);
			FireCompletion(job);
			it = jobs.erase(it);
		} else {
			++it;
		}
	}
	Notify();
}

void CancelAll(not_null<Main::Session*> session) {
	auto &jobs = Jobs();
	for (auto it = begin(jobs); it != end(jobs);) {
		const auto job = it->second.get();
		if (BelongsTo(*job, session)) {
			CancelJob(job);
			it = jobs.erase(it);
		} else {
			++it;
		}
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	db.clearNfResume(session->uniqueId());
	Notify();
}

void ResumeAll(not_null<Main::Session*> session) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &record : db.loadNfResume(session->uniqueId())) {
		if (record.remaining.empty()
			|| Jobs().contains(record.destPeerId)) {
			continue;
		}
		auto action = Api::SendAction(
			session->data().history(record.destPeerId));
		auto job = std::make_shared<Job>(
			session,
			&session->api(),
			record.destPeerId,
			record.srcPeerId,
			std::move(action),
			Data::ForwardOptions(),
			Data::GroupingOptions());
		job->total = record.total;
		job->done = record.done;
		job->skipped = record.skipped;
		job->lastMsgId = record.lastMsgId;
		job->remaining = record.remaining;
		job->state = (record.state == u"running"_q)
			? Job::State::Running
			: Job::State::Paused;
		Jobs().emplace(record.destPeerId, job);
		if (job->state == Job::State::Running) {
			Pump(job.get());
		}
	}
	Notify();
}

int UnfinishedCount(not_null<Main::Session*> session) {
	auto result = 0;
	for (const auto &[dst, job] : Jobs()) {
		if (BelongsTo(*job, session) && !job->remaining.empty()) {
			result++;
		}
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	return std::max(result, int(db.loadNfResume(session->uniqueId()).size()));
}

Counters CountersFor(not_null<Main::Session*> session) {
	auto result = Counters();
	for (const auto &[dst, job] : Jobs()) {
		if (BelongsTo(*job, session)) {
			result.done = job->done;
			result.total = job->total;
			result.skipped = job->skipped;
			result.lastFileName = job->lastFileName;
			result.floodSeconds = (job->state == Job::State::FloodWait)
				? job->floodSeconds
				: 0;
			result.active = job->alive() || !job->remaining.empty();
			break;
		}
	}
	return result;
}

rpl::producer<> countersChanged() {
	return CountersChanged.events();
}

} // namespace NormalForward
