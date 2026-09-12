/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_file_origin.h"
#include "ui/basic_click_handlers.h"

#include <functional>
#include <memory>

class DocumentData;
class HistoryItem;
class PhotoData;

class FileClickHandler : public LeftButtonClickHandler {
public:
	FileClickHandler(FullMsgId context);

	void setMessageId(FullMsgId context);

	[[nodiscard]] FullMsgId context() const;

private:
	FullMsgId _context;

};

class DocumentClickHandler : public FileClickHandler {
public:
	DocumentClickHandler(
		not_null<DocumentData*> document,
		FullMsgId context = FullMsgId());

	QString tooltip() const override;

	[[nodiscard]] not_null<DocumentData*> document() const;

private:
	const not_null<DocumentData*> _document;

};

struct DownloadBatch {
	int total = 0;
	int downloaded = 0;
	int duplicates = 0;
	int failed = 0;
	bool done = false;
	std::function<void(int downloaded, int duplicates)> onDone;

	void addDownloaded() {
		++downloaded;
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
		if (downloaded + duplicates + failed < total) {
			return;
		}
		done = true;
		if (onDone) {
			onDone(downloaded, duplicates);
		}
	}
};

[[nodiscard]] std::shared_ptr<DownloadBatch> MakeDownloadBatch(int total);

class DocumentSaveClickHandler : public DocumentClickHandler {
public:
	enum class Mode {
		ToCacheOrFile,
		ToFile,
		ToNewFile,
	};
	using DocumentClickHandler::DocumentClickHandler;
	static void Save(
		Data::FileOrigin origin,
		not_null<DocumentData*> document,
		Mode mode = Mode::ToCacheOrFile,
		Fn<void()> started = nullptr,
		PeerData *peer = nullptr,
		const QString &topicName = QString(),
		std::shared_ptr<DownloadBatch> batch = nullptr);
	static void SaveAndTrack(
		FullMsgId itemId,
		not_null<DocumentData*> document,
		Mode mode = Mode::ToCacheOrFile,
		Fn<void()> started = nullptr,
		std::shared_ptr<DownloadBatch> batch = nullptr);

protected:
	void onClickImpl() const override;

};

class DocumentOpenClickHandler : public DocumentClickHandler {
public:
	DocumentOpenClickHandler(
		not_null<DocumentData*> document,
		Fn<void(FullMsgId)> &&callback,
		FullMsgId context = FullMsgId());

protected:
	void onClickImpl() const override;

private:
	const Fn<void(FullMsgId)> _handler;

};

class DocumentCancelClickHandler : public DocumentClickHandler {
public:
	DocumentCancelClickHandler(
		not_null<DocumentData*> document,
		Fn<void(FullMsgId)> &&callback,
		FullMsgId context = FullMsgId());

protected:
	void onClickImpl() const override;

private:
	const Fn<void(FullMsgId)> _handler;

};

class DocumentOpenWithClickHandler : public DocumentClickHandler {
public:
	using DocumentClickHandler::DocumentClickHandler;
	static void Open(
		Data::FileOrigin origin,
		not_null<DocumentData*> document);

protected:
	void onClickImpl() const override;

};

class VoiceSeekClickHandler : public DocumentOpenClickHandler {
public:
	using DocumentOpenClickHandler::DocumentOpenClickHandler;

protected:
	void onClickImpl() const override {
	}

};

class DocumentWrappedClickHandler : public DocumentClickHandler {
public:
	DocumentWrappedClickHandler(
		ClickHandlerPtr wrapped,
		not_null<DocumentData*> document,
		FullMsgId context = FullMsgId());

protected:
	void onClickImpl() const override;

private:
	ClickHandlerPtr _wrapped;

};

class PhotoClickHandler : public FileClickHandler {
public:
	PhotoClickHandler(
		not_null<PhotoData*> photo,
		FullMsgId context = FullMsgId(),
		PeerData *peer = nullptr);

	[[nodiscard]] not_null<PhotoData*> photo() const;
	[[nodiscard]] PeerData *peer() const;

private:
	const not_null<PhotoData*> _photo;
	PeerData * const _peer = nullptr;

};

class PhotoOpenClickHandler : public PhotoClickHandler {
public:
	PhotoOpenClickHandler(
		not_null<PhotoData*> photo,
		Fn<void(FullMsgId)> &&callback,
		FullMsgId context = FullMsgId());

protected:
	void onClickImpl() const override;

private:
	const Fn<void(FullMsgId)> _handler;

};

class PhotoSaveClickHandler : public PhotoClickHandler {
public:
	using PhotoClickHandler::PhotoClickHandler;

protected:
	void onClickImpl() const override;

};

class PhotoCancelClickHandler : public PhotoClickHandler {
public:
	PhotoCancelClickHandler(
		not_null<PhotoData*> photo,
		Fn<void(FullMsgId)> &&callback,
		FullMsgId context = FullMsgId());

protected:
	void onClickImpl() const override;

private:
	const Fn<void(FullMsgId)> _handler;

};
