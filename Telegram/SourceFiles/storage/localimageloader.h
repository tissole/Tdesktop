/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/variant.h"
#include "api/api_common.h"

#include <memory>
#include <functional>

namespace Ui {
struct PreparedFileInformation;
} // namespace Ui

namespace Media::Encode {
struct Job;
struct VideoSource;
} // namespace Media::Encode

namespace Main {
class Session;
} // namespace Main

struct FilePrepareResult;

// Load files up to 2'000 MB.
constexpr auto kFileSizeLimit = 2'000 * int64(1024 * 1024);

// Load files up to 4'000 MB.
constexpr auto kFileSizePremiumLimit = 4'000 * int64(1024 * 1024);

[[nodiscard]] int PhotoSideLimit(bool large);
[[nodiscard]] int PhotoSideLimit();

enum class SendMediaType {
	Photo,
	Audio,
	Round,
	File,
	ThemeFile,
	Secure,

	// Uploaded just to get an InputFile, without a document or a message.
	SecondaryFile,
};

using TaskId = void*; // no interface, just id
inline constexpr auto kEmptyTaskId = TaskId();

class Task {
public:
	virtual void process() = 0; // is executed in a separate thread
	virtual void finish() = 0; // is executed in the same as TaskQueue thread
	virtual ~Task() = default;

	TaskId id() const {
		return static_cast<TaskId>(const_cast<Task*>(this));
	}

};

class TaskQueueWorker;
class TaskQueue : public QObject {
	Q_OBJECT

public:
	explicit TaskQueue(crl::time stopTimeoutMs = 0); // <= 0 - never stop worker

	TaskId addTask(std::unique_ptr<Task> &&task);
	void addTasks(std::vector<std::unique_ptr<Task>> &&tasks);
	void cancelTask(TaskId id); // this task finish() won't be called

	~TaskQueue();

Q_SIGNALS:
	void taskAdded();

public Q_SLOTS:
	void onTaskProcessed();
	void stop();

private:
	friend class TaskQueueWorker;

	void wakeThread();

	std::deque<std::unique_ptr<Task>> _tasksToProcess;
	std::deque<std::unique_ptr<Task>> _tasksToFinish;
	TaskId _taskInProcessId = TaskId();
	QMutex _tasksToProcessMutex, _tasksToFinishMutex;
	QThread *_thread = nullptr;
	TaskQueueWorker *_worker = nullptr;
	QTimer *_stopTimer = nullptr;

};

class TaskQueueWorker : public QObject {
	Q_OBJECT

public:
	TaskQueueWorker(TaskQueue *queue) : _queue(queue) {
	}

Q_SIGNALS:
	void taskProcessed();

public Q_SLOTS:
	void onTaskAdded();

private:
	TaskQueue *_queue;
	bool _inTaskAdded = false;

};

struct UploadBatch {
	int total = 0;
	int uploaded = 0;
	int duplicates = 0;
	int failed = 0;
	bool done = false;
	std::function<void(int uploaded, int duplicates)> onDone;

	void addUploaded() {
		++uploaded;
		checkDone();
	}
	void addDuplicate() {
		++duplicates;
		checkDone();
	}
	void addFailed() {
		++failed;
		checkDone();
	}
	void checkDone() {
		if (done || total <= 0) {
			return;
		}
		if (uploaded + duplicates + failed < total) {
			return;
		}
		done = true;
		if (onDone) {
			onDone(uploaded, duplicates);
		}
	}
};

struct SendingAlbum {
	struct Item {
		explicit Item(TaskId taskId);

		TaskId taskId = kEmptyTaskId;
		uint64 randomId = 0;
		FullMsgId msgId;
		std::optional<MTPInputSingleMedia> media;
		std::shared_ptr<FilePrepareResult> prepared;
	};

	SendingAlbum();

	[[nodiscard]] bool preparedMusicBatching() const;
	[[nodiscard]] bool preparedMusicReady() const;
	[[nodiscard]] std::shared_ptr<FilePrepareResult> preparedMusicSample() const;
	[[nodiscard]] std::vector<std::shared_ptr<FilePrepareResult>>
		takePreparedMusic();

	void fillMedia(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		uint64 randomId);
	void refreshMediaCaption(not_null<HistoryItem*> item);
	void removeItem(not_null<HistoryItem*> item);
	void removeTask(TaskId taskId);

	uint64 groupId = 0;
	std::vector<Item> items;
	Api::SendOptions options;
	bool musicPreparedBatching = false;
	bool sent = false;
	uint32 expectedCount = 0;
	uint64 sendOrder = 0;
	PeerId peerId;

};

struct FileLoadTo {
	FileLoadTo(
		PeerId peer,
		Api::SendOptions options,
		FullReplyTo replyTo,
		MsgId replaceMediaOf)
	: peer(peer)
	, options(options)
	, replyTo(replyTo)
	, replaceMediaOf(replaceMediaOf) {
	}
	PeerId peer;
	Api::SendOptions options;
	FullReplyTo replyTo;
	MsgId replaceMediaOf;
};

using UploadFileParts = std::vector<QByteArray>;
struct FilePrepareDescriptor {
	TaskId taskId = kEmptyTaskId;
	base::required<uint64> id;
	SendMediaType type = SendMediaType::File;
	FileLoadTo to = { PeerId(), Api::SendOptions(), FullReplyTo(), MsgId() };
	TextWithTags caption;
	bool spoiler = false;
	std::shared_ptr<SendingAlbum> album;
	std::shared_ptr<UploadBatch> batch;
};
struct FilePrepareResult {
	explicit FilePrepareResult(FilePrepareDescriptor &&descriptor);
	~FilePrepareResult();

	TaskId taskId = kEmptyTaskId;
	uint64 id = 0;
	uint64 fileId = 0; // explicit file_id for saveFilePart (0 = use id/thumbId)
	FileLoadTo to;
	std::shared_ptr<SendingAlbum> album;
	std::shared_ptr<UploadBatch> batch;
	SendMediaType type = SendMediaType::File;
	QString filepath;
	QByteArray content;

	QString filename;
	QString filemime;
	int64 filesize = 0;
	UploadFileParts fileparts;
	QByteArray filemd5;
	int64 partssize = 0;

	uint64 thumbId = 0; // id is always file-id of media, thumbId is file-id of thumb ( == id for photos)
	QString thumbname;
	UploadFileParts thumbparts;
	QByteArray thumbbytes;
	QByteArray thumbmd5;
	QImage thumb;

	QImage goodThumbnail;
	QByteArray goodThumbnailBytes;

	MTPPhoto photo = MTP_photoEmpty(MTP_long(0));
	MTPDocument document = MTP_documentEmpty(MTP_long(0));

	PreparedPhotoThumbs photoThumbs;
	TextWithTags caption;
	bool spoiler = false;
	bool forceFile = false;
	std::shared_ptr<Media::Encode::VideoSource> videoSource;
	crl::time videoCoverOffset = 0;
	std::shared_ptr<Media::Encode::Job> animationJob;
	QString transcodedTempPath;

	std::vector<MTPInputDocument> attachedStickers;
	std::shared_ptr<FilePrepareResult> videoCover;
	void setFileData(const QByteArray &filedata);
	void setThumbData(const QByteArray &thumbdata);

};

[[nodiscard]] std::shared_ptr<FilePrepareResult> MakePreparedFile(
	FilePrepareDescriptor &&descriptor);

// Computes the exact JPEG bytes the photo uploader would submit for this
// source file (re-encode + optional downscale), used for pick-time dedup.
[[nodiscard]] QByteArray PreparePhotoUploadBytes(
	const QString &filepath,
	bool sendLargePhotos);

class FileLoadTask final : public Task {
public:
	static std::unique_ptr<Ui::PreparedFileInformation> ReadMediaInformation(
		const QString &filepath,
		const QByteArray &content,
		const QString &filemime);
	static bool FillImageInformation(
		QImage &&image,
		bool animated,
		std::unique_ptr<Ui::PreparedFileInformation> &result,
		QByteArray content = {},
		QByteArray format = {});

	struct Args {
		not_null<Main::Session*> session;
		QString filepath;
		QByteArray content;
		std::unique_ptr<Ui::PreparedFileInformation> information;
		std::unique_ptr<FileLoadTask> videoCover;
		SendMediaType type;
		FileLoadTo to;
		TextWithTags caption;
		bool spoiler = false;
		std::shared_ptr<SendingAlbum> album;
		std::shared_ptr<UploadBatch> batch;
		bool forceFile = false;
		bool sendLargePhotos = false;
		std::shared_ptr<Media::Encode::Job> animationJob;
		uint64 idOverride = 0;
		QString displayName;
	};

	struct VoiceArgs {
		not_null<Main::Session*> session;
		QByteArray voice;
		crl::time duration = 0;
		VoiceWaveform waveform;
		bool video = false;
		FileLoadTo to;
		TextWithTags caption;
	};

	explicit FileLoadTask(Args &&args);
	explicit FileLoadTask(VoiceArgs &&args);
	~FileLoadTask();

	uint64 fileid() const {
		return _id;
	}

	struct ProcessArgs {
		bool generateGoodThumbnail = true;
	};
	void process(ProcessArgs &&args);

	void process() override {
		process({});
	}
	void finish() override;

	[[nodiscard]] auto peekResult() const
		-> const std::shared_ptr<FilePrepareResult> &;

private:
	static bool CheckForSong(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result);
	static bool CheckForVideo(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result);
	static bool CheckForImage(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result);
	static bool CheckForDocument(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result);

	template <typename Mimes, typename Extensions>
	static bool CheckMimeOrExtensions(const QString &filepath, const QString &filemime, Mimes &mimes, Extensions &extensions);

	std::unique_ptr<Ui::PreparedFileInformation> readMediaInformation(const QString &filemime) const;
	void removeFromAlbum();

	uint64 _id = 0;
	base::weak_ptr<Main::Session> _session;
	MTP::DcId _dcId = 0;
	FileLoadTo _to;
	const std::shared_ptr<SendingAlbum> _album;
	const std::shared_ptr<UploadBatch> _batch;
	QString _filepath;
	QString _displayName;
	QByteArray _content;
	std::unique_ptr<FileLoadTask> _videoCover;
	std::unique_ptr<Ui::PreparedFileInformation> _information;
	crl::time _duration = 0;
	VoiceWaveform _waveform;
	SendMediaType _type;
	TextWithTags _caption;
	bool _spoiler = false;
	bool _forceFile = false;
	bool _sendLargePhotos = false;
	std::shared_ptr<Media::Encode::Job> _animationJob;

	std::shared_ptr<FilePrepareResult> _result;

};
