/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_gif.h"

#include "apiwrap.h"
#include "api/api_transcribes.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "media/audio/media_audio.h"
#include "media/clip/media_clip_reader.h"
#include "media/media_common.h"
#include "media/player/media_player_instance.h"
#include "media/streaming/media_streaming_instance.h"
#include "media/streaming/media_streaming_player.h"
#include "media/streaming/media_streaming_utility.h"
#include "media/view/media_view_open_common.h"
#include "media/view/media_view_playback_progress.h"
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "history/history_item_components.h"
#include "history/history_item.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/history_item_helpers.h"
#include "base/unixtime.h"
#include "history/view/history_view_message.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_reply.h"
#include "history/view/history_view_transcribe_button.h"
#include "history/view/media/history_view_document.h" // TTLVoiceStops
#include "history/view/media/history_view_ephemeral_plate.h"
#include "history/view/media/history_view_media_common.h"
#include "history/view/media/history_view_media_spoiler.h"
#include "history/view/media/history_view_video_message_seek.h"
#include "history/view/media/history_view_video_status.h"
#include "window/window_session_controller.h"
#include "core/application.h" // Application::showDocument.
#include "core/core_settings.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/chat/chat_style.h"
#include "ui/image/image.h"
#include "ui/text/format_values.h"
#include "ui/grouped_layout.h"
#include "ui/cached_round_corners.h"
#include "ui/power_saving.h"
#include "ui/ui_utility.h"
#include "ui/effects/path_shift_gradient.h"
#include "ui/effects/spoiler_mess.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_streaming.h"
#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_file_origin.h"
#include "data/data_document_media.h"
#include "data/data_web_page.h"
#include "storage/storage_account.h"
#include "styles/style_chat.h"
#include "styles/style_chat_style.h"

#include <QSvgRenderer>
#include <QtWidgets/QApplication>

namespace HistoryView {
namespace {

constexpr auto kMaxGifForwardedBarLines = 4;
constexpr auto kUseNonBlurredThreshold = 240;
constexpr auto kMaxInlineArea = 1920 * 1080;
constexpr auto kMaxInstantViewInlineArea = 1920 * 1920;
constexpr auto kSeekPreviewInterval = crl::time(100);

using ::Media::ValidFrameSize;

[[nodiscard]] bool IsHostedInstantViewMedia(not_null<const Element*> parent) {
	return parent->Get<InstantViewMediaRuntime>() != nullptr;
}

[[nodiscard]] double HostedInstantViewMediaPixelScale(
		not_null<const Element*> parent) {
	const auto runtime = parent->Get<InstantViewMediaRuntime>();
	return runtime ? runtime->mediaPixelScale : 1.;
}

[[nodiscard]] QSize ScaledInstantViewMediaSize(QSize size, double scale) {
	return (scale == 1.)
		? size
		: QSize(
			std::max(qRound(size.width() * scale), 1),
			std::max(qRound(size.height() * scale), 1));
}

[[nodiscard]] QSize HostedInstantViewForcedSize(
		not_null<const Element*> parent,
		not_null<const Media*> media) {
	const auto runtime = parent->Get<InstantViewMediaRuntime>();
	return (runtime && runtime->forcedFor == media)
		? runtime->forcedSize
		: QSize();
}

[[nodiscard]] int GifMaxStatusWidth(not_null<DocumentData*> document) {
	auto result = st::normalFont->width(
		Ui::FormatDownloadText(document->size, document->size));
	accumulate_max(
		result,
		st::normalFont->width(Ui::FormatGifAndSizeText(document->size)));
	return result;
}

[[nodiscard]] HistoryView::TtlRoundPaintCallback CreateTtlPaintCallback(
		Fn<void()> update) {
	const auto centerMargins = Margins(st::historyFileInPause.width() * 3);

	const auto renderer = std::make_shared<QSvgRenderer>(
		u":/gui/ttl/video_message_icon.svg"_q);

	return [=](QPainter &p, QRect r, const PaintContext &context) {
		const auto centerRect = r - centerMargins;
		const auto &icon = context.imageStyle()->historyVideoMessageTtlIcon;
		const auto iconRect = QRect(
			rect::right(centerRect) - icon.width() * 0.75,
			rect::bottom(centerRect) - icon.height() * 0.75,
			icon.width(),
			icon.height());
		{
			auto hq = PainterHighQualityEnabler(p);
			auto path = QPainterPath();
			path.setFillRule(Qt::WindingFill);
			path.addEllipse(centerRect);
			path.addEllipse(iconRect);
			p.fillPath(path, st::shadowFg);
			p.fillPath(path, st::shadowFg);
			p.fillPath(path, st::shadowFg);
		}

		renderer->render(&p, centerRect - Margins(centerRect.width() / 4));

		icon.paint(p, iconRect.topLeft(), centerRect.width());
	};
}

} // namespace

struct Gif::Streamed {
	Streamed(
		not_null<DocumentData*> chosen,
		std::shared_ptr<::Media::Streaming::Document> shared,
		Fn<void()> waitingCallback);
	const not_null<DocumentData*> chosen;
	::Media::Streaming::Instance instance;
	::Media::Streaming::FrameRequest frozenRequest;
	QImage frozenFrame;
	QString frozenStatusText;
};

Gif::Streamed::Streamed(
	not_null<DocumentData*> chosen,
	std::shared_ptr<::Media::Streaming::Document> shared,
	Fn<void()> waitingCallback)
: chosen(chosen)
, instance(std::move(shared), std::move(waitingCallback)) {
}

[[nodiscard]] bool IsHiddenRoundMessage(not_null<Element*> parent) {
	return parent->delegate()->elementContext() != Context::TTLViewer
		&& parent->data()->media()
		&& parent->data()->media()->ttlSeconds();
}

Gif::Gif(
	not_null<Element*> parent,
	not_null<HistoryItem*> realParent,
	not_null<DocumentData*> document,
	bool spoiler)
: File(parent, realParent)
, _data(document)
, _videoCover(LookupVideoCover(document, realParent))
, _storyId(realParent->media()
	? realParent->media()->storyId()
	: FullStoryId())
, _spoiler((spoiler
	|| IsHiddenRoundMessage(_parent)
	|| realParent->isMediaSensitive())
	? std::make_unique<MediaSpoiler>()
	: nullptr)
, _downloadSize(Ui::FormatSizeText(_data->size))
, _videoTimestamp(::Media::View::ExtractVideoTimestamp(realParent))
, _sensitiveSpoiler(realParent->isMediaSensitive())
, _ttlCover(realParent->isTtlCoveredMedia())
, _hasVideoCover(realParent->media() && realParent->media()->videoCover()) {
	const auto media = _parent->data()->media();
	if (_data->isVideoMessage() && media && media->ttlSeconds()) {
		if (_spoiler) {
			_drawTtl = CreateTtlPaintCallback([=] { repaint(); });
		}
		const auto fullId = _realParent->fullId();
		const auto &data = &_parent->data()->history()->owner();
		const auto isOut = _parent->data()->out();
		_parent->data()->removeFromSharedMediaIndex();
		setDocumentLinks(_data, realParent, [=] {
			auto lifetime = std::make_shared<rpl::lifetime>();
			TTLVoiceStops(fullId) | rpl::on_next([=]() mutable {
				if (lifetime) {
					base::take(lifetime)->destroy();
				}
				if (!isOut) {
					if (const auto item = data->message(fullId)) {
						// Destroys this.
						item->clearMediaAsExpired();
					}
				}
			}, *lifetime);

			return false;
		});
	} else {
		setDocumentLinks(_data, realParent, [=] {
			if (!_data->createMediaView()->canBePlayed()
				|| !_data->isAnimation()
				|| _data->isVideoMessage()
				|| !canPlayInline()) {
				return false;
			}
			playAnimation(false);
			return true;
		});
	}

	setStatusSize(Ui::FileStatusSizeReady);

	if (_data->isVideoMessage()) {
		_roundSeek = std::make_unique<VideoMessageSeek>([=] { repaint(); });
		if (!media || !media->ttlSeconds()) {
			_seekl = std::make_shared<VoiceSeekClickHandler>(
				_data,
				[](FullMsgId) {});
		}
	}

	if (_spoiler) {
		createSpoilerLink(_spoiler.get());
	}

	if ((_dataMedia = _data->activeMediaView())) {
		dataMediaCreated();
	} else if (_videoCover) {
		if (_videoCover->inlineThumbnailBytes().isEmpty()
			&& (_videoCover->hasExact(Data::PhotoSize::Small)
				|| _videoCover->hasExact(Data::PhotoSize::Thumbnail))) {
			_videoCover->load(Data::PhotoSize::Small, realParent->fullId());
		}
	} else {
		_data->loadThumbnail(realParent->fullId());
		if (!autoplayEnabled()) {
			_data->loadVideoThumbnail(realParent->fullId());
		}
	}
	ensureTranscribeButton();

	_purchasedPriceTag = hasPurchasedTag();
}

Gif::~Gif() {
	if (_streamed || _dataMedia) {
		if (_streamed) {
			_data->owner().streaming().keepAlive(_data);
			setStreamed(nullptr);
		}
		if (_dataMedia) {
			_data->owner().keepAlive(base::take(_dataMedia));
			_parent->checkHeavyPart();
		}
	}
	togglePollingStory(false);
}

bool Gif::CanPlayInline(not_null<DocumentData*> document) {
	return ValidFrameSize(document->dimensions, kMaxInlineArea);
}

int Gif::maxInlineArea() const {
	return IsHostedInstantViewMedia(_parent)
		? kMaxInstantViewInlineArea
		: kMaxInlineArea;
}

bool Gif::canPlayInline() const {
	return ValidFrameSize(_data->dimensions, maxInlineArea());
}

QSize Gif::sizeForAspectRatio() const {
	if (!_data->dimensions.isEmpty()) {
		return _data->dimensions;
	}
	if (_videoCover) {
		return { _videoCover->width(), _videoCover->height() };
	} else if (_data->hasThumbnail()) {
		const auto &location = _data->thumbnailLocation();
		return NonEmptySize({ location.width(), location.height() });
	}
	return { 1, 1 };
}

QSize Gif::countThumbSize(int &inOutWidthMax) const {
	const auto hostedInstantView = IsHostedInstantViewMedia(_parent);
	const auto maxSize = [&] {
		if (hostedInstantView) {
			return std::max(inOutWidthMax, 1);
		} else if (_data->isVideoFile()) {
			return st::maxMediaSize;
		} else if (_data->isVideoMessage()) {
			return st::maxVideoMessageSize;
		}
		return st::maxGifSize;
	}();
	const auto size = style::ConvertScale(videoSize());
	accumulate_min(inOutWidthMax, maxSize);
	auto result = DownscaledSize(size, { inOutWidthMax, maxSize });
	if (_data->isVideoMessage()) {
		result.setHeight(result.height() + st::msgDateFont->height + 2 * st::msgDateImgPadding.y() + 2);
	}
	return result;
}

QSize Gif::countOptimalSize() {
	if (_data->isVideoMessage() && _transcribe) {
		const auto &entry = _data->session().api().transcribes().entry(
			_realParent);
		_transcribe->setLoading(
			entry.shown && (entry.requestId || entry.pending));
	}

	if (const auto forced = HostedInstantViewForcedSize(_parent, this)
		; !forced.isEmpty()) {
		return forced;
	}
	const auto hostedInstantView = IsHostedInstantViewMedia(_parent);
	const auto maxMediaWidth = hostedInstantView
		? std::max(st::msgMaxWidth, st::maxMediaSize)
		: st::maxMediaSize;
	const auto minWidth = std::clamp(
		_parent->minWidthForMedia(),
		(_parent->hasBubble()
			? st::historyPhotoBubbleMinWidth
			: st::minPhotoSize),
		maxMediaWidth);
	auto thumbMaxWidth = st::msgMaxWidth;
	const auto scaled = countThumbSize(thumbMaxWidth);
	auto maxWidth = std::min(
		std::max(scaled.width(), minWidth),
		thumbMaxWidth);
	auto minHeight = qMax(scaled.height(), st::minPhotoSize);
	if (!activeCurrentStreamed()) {
		accumulate_max(
			maxWidth,
			GifMaxStatusWidth(_data)
				+ 2 * (st::msgDateImgDelta + st::msgDateImgPadding.x()));
	}
	if (_parent->hasBubble()) {
		maxWidth = qMax(maxWidth, _parent->textualMaxWidth());
		minHeight = adjustHeightForLessCrop(
			scaled,
			{ maxWidth, minHeight });
	} else if (isUnwrapped()) {
		const auto item = _parent->data();
		auto via = item->Get<HistoryMessageVia>();
		auto reply = _parent->Get<Reply>();
		auto forwarded = item->Get<HistoryMessageForwarded>();
		if (forwarded) {
			forwarded->create(via, item);
		}
		RefreshEphemeralPlate(_parent, _ephemeral.text);
		maxWidth += additionalWidth(reply, via, forwarded);
		accumulate_max(maxWidth, _parent->reactionsOptimalWidth());
	}
	return { maxWidth, minHeight };
}

QSize Gif::countCurrentSize(int newWidth) {
	if (const auto forced = HostedInstantViewForcedSize(_parent, this)
		; !forced.isEmpty()) {
		return forced;
	}
	auto availableWidth = newWidth;
	_ephemeral.onTop = false;
	_ephemeral.topAdded = 0;

	const auto hostedInstantView = IsHostedInstantViewMedia(_parent);
	auto thumbMaxWidth = newWidth;
	const auto scaled = countThumbSize(thumbMaxWidth);
	const auto minWidthByInfo = hostedInstantView
		? _parent->minWidthForMedia()
		: (_parent->hidesBottomInfo()
			? 0
			: (_parent->infoWidth()
				+ 2 * (st::msgDateImgDelta
					+ st::msgDateImgPadding.x())));
	const auto minPhotoWidth = std::min(st::minPhotoSize, thumbMaxWidth);
	newWidth = std::clamp(
		std::max(scaled.width(), minWidthByInfo),
		minPhotoWidth,
		thumbMaxWidth);
	auto newHeight = qMax(scaled.height(), st::minPhotoSize);
	if (!activeCurrentStreamed()) {
		accumulate_max(
			newWidth,
			GifMaxStatusWidth(_data)
				+ 2 * (st::msgDateImgDelta + st::msgDateImgPadding.x()));
	}
	if (_parent->hasBubble()) {
		accumulate_max(newWidth, _parent->minWidthForMedia());
		auto captionMaxWidth = _parent->textualMaxWidth();
		const auto botTop = _parent->Get<FakeBotAboutTop>();
		if (botTop) {
			accumulate_max(captionMaxWidth, botTop->maxWidth);
		}
		const auto maxWithCaption = qMin(st::msgMaxWidth, captionMaxWidth);
		newWidth = qMin(qMax(newWidth, maxWithCaption), thumbMaxWidth);
		newHeight = adjustHeightForLessCrop(
			scaled,
			{ newWidth, newHeight });
	} else if (isUnwrapped()) {
		accumulate_max(newWidth, _parent->reactionsOptimalWidth());

		const auto item = _parent->data();
		auto via = item->Get<HistoryMessageVia>();
		auto reply = _parent->Get<Reply>();
		auto forwarded = item->Get<HistoryMessageForwarded>();
		RefreshEphemeralPlate(_parent, _ephemeral.text);
		if (via || reply || forwarded || !_ephemeral.text.isEmpty()) {
			auto additional = additionalWidth(reply, via, forwarded);
			newWidth += additional;
			accumulate_min(newWidth, availableWidth);
			const auto usew = maxWidth() - additional;
			if (!_ephemeral.text.isEmpty()) {
				const auto contentWidth = _data->isVideoMessage()
					? std::min(usew, newHeight)
					: usew;
				const auto sideRoom = newWidth
					- contentWidth
					- st::msgReplyPadding.left();
				_ephemeral.onTop = (sideRoom
					< EphemeralPlateMaxWidth(_ephemeral.text));
			}
			const auto rectw = _ephemeral.onTop
				? std::min(newWidth - st::msgReplyPadding.left(), additional)
				: (newWidth - usew - st::msgReplyPadding.left());
			const auto availw = rectw
				- st::msgReplyPadding.left()
				- st::msgReplyPadding.left();
			if (!forwarded && via) {
				via->resize(availw);
			}
			if (reply) {
				[[maybe_unused]] int height = reply->resizeToWidth(availw);
			}
			if (_ephemeral.onTop) {
				const auto plate = EphemeralPlateSize(
					_ephemeral.text,
					newWidth - st::msgReplyPadding.left());
				_ephemeral.topAdded = plate.height()
					+ st::msgReplyPadding.top()
					+ ((via || reply || forwarded)
						? surroundingHeight(reply, via, forwarded, rectw)
						: 0);
				newHeight += _ephemeral.topAdded;
			}
		}
	}

	return { newWidth, newHeight };
}

int Gif::adjustHeightForLessCrop(QSize dimensions, QSize current) const {
	if (dimensions.isEmpty()) {
		return current.height();
	}
	// Allow some more vertical space for less cropping,
	// but not more than 1.33 * existing height.
	return qMax(
		current.height(),
		qMin(
			current.width() * dimensions.height() / dimensions.width(),
			current.height() * 4 / 3));
}

QSize Gif::videoSize() const {
	if (const auto streamed = activeCurrentStreamed()) {
		return streamed->player().videoSize();
	} else if (_videoCover) {
		return QSize(_videoCover->width(), _videoCover->height());
	} else if (!_data->dimensions.isEmpty()) {
		return _data->dimensions;
	} else if (_data->hasThumbnail()) {
		const auto &location = _data->thumbnailLocation();
		return QSize(location.width(), location.height());
	} else {
		return QSize(1, 1);
	}
}

void Gif::validateRoundingMask(QSize size) const {
	if (_roundingMask.size() != size) {
		const auto ratio = style::DevicePixelRatio();
		_roundingMask = Images::EllipseMask(size / ratio);
	}
}

bool Gif::downloadInCorner() const {
	return _data->isVideoFile()
		&& (_data->loading() || !autoplayEnabled())
		&& _realParent->allowsMediaDownloadControls()
		&& _data->canBeStreamed()
		&& !_data->inappPlaybackFailed();
}

bool Gif::autoplayUnderCursor() const {
	return (_videoTimestamp || _hasVideoCover);
}

bool Gif::underCursor(bool fullFeatured) const {
	return ClickHandler::getActive() == currentVideoLink(fullFeatured);
}

bool Gif::autoplayEnabled() const {
	auto peerId = _parent->data()->from() ? _parent->data()->from()->id : PeerId(0);
	auto user = history()->session().data().peerLoaded(_parent->data()->from() ? _parent->data()->from()->id : PeerId(0));
	if ((GetEnhancedBool("blocked_user_spoiler_mode") && blockExist(peerId.value)) || (GetEnhancedBool("blocked_user_spoiler_mode") && user && user->isBlocked())) {
		return false;
	}
	return Data::AutoDownload::ShouldAutoPlay(
		_data->session().settings().autoDownload(),
		_realParent->history()->peer,
		_data);
}

bool Gif::autoplayEligible(bool fullFeatured) const {
	ensureDataMediaCreated();
	return fullFeatured
		&& autoplayEnabled()
		&& _dataMedia->canBePlayed()
		&& canPlayInline()
		&& !(_data->uploading() && _data->uploadingData->preparing);
}

float64 Gif::revealedProgress() const {
	const auto item = _parent->data();
	const auto isRound = _data->isVideoMessage();
	const auto inTTLViewer = _parent->delegate()->elementContext()
		== Context::TTLViewer;
	return ((isRound || _ttlCover)
		&& item->media()
		&& item->media()->ttlSeconds()
		&& !inTTLViewer)
		? 0.
		: (!isRound && _spoiler)
		? _spoiler->revealAnimation.value(_spoiler->revealed ? 1. : 0.)
		: 1.;
}

bool Gif::hideMessageText() const {
	return _data->isVideoMessage();
}

void Gif::draw(Painter &p, const PaintContext &context) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) return;

	_smallGroupPart = false;

	// Calculate showInfo for hover effect
	const bool showInfo = _parent->isUnderCursor() || context.selected();

	ensureDataMediaCreated();
	const auto item = _parent->data();
	const auto loaded = dataLoaded();
	const auto displayLoading = (item->isSending() || _data->displayLoading());
	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto cornerDownload = downloadInCorner();
	const auto autoplay = autoplayEligible(true);
	const auto activeRoundPlaying = activeRoundStreamed();

	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	const bool bubble = _parent->hasBubble();
	const auto rightLayout = _parent->hasRightLayout();
	const auto inWebPage = (_parent->media() != this);
	const auto isRound = _data->isVideoMessage();
	const auto hostedInstantView = IsHostedInstantViewMedia(_parent);

	const auto inWebPageWithoutOwnRounding = inWebPage
		&& bubbleRounding() == Ui::BubbleRounding();
	const auto rounding = hostedInstantView
		? std::optional<Ui::BubbleRounding>(Ui::BubbleRounding())
		: inWebPageWithoutOwnRounding
		? std::optional<Ui::BubbleRounding>()
		: adjustedBubbleRounding();

	auto usex = 0, usew = paintw;
	const auto unwrapped = isUnwrapped();
	const auto via = unwrapped ? item->Get<HistoryMessageVia>() : nullptr;
	const auto reply = unwrapped ? _parent->Get<Reply>() : nullptr;
	const auto forwarded = unwrapped ? item->Get<HistoryMessageForwarded>() : nullptr;
	const auto rightAligned = unwrapped && rightLayout;
	if (via || reply || forwarded || !_ephemeral.text.isEmpty()) {
		usew = maxWidth() - additionalWidth(reply, via, forwarded);
		if (rightAligned) {
			usex = width() - usew;
		}
	}
	if (_ephemeral.onTop) {
		painty += _ephemeral.topAdded;
		painth -= _ephemeral.topAdded;
	}
	if (isRound) {
		accumulate_min(usew, painth);
	}
	if (rtl()) usex = width() - usex - usew;

	QRect rthumb(style::rtlrect(usex + paintx, painty, usew, isRound ? usew : painth, width()));

	const auto inTTLViewer = _parent->delegate()->elementContext()
		== Context::TTLViewer;
	const auto revealed = revealedProgress();
	const auto fullHiddenBySpoiler = (revealed == 0.);
	if (revealed < 1.) {
		validateSpoilerImageCache(rthumb.size(), rounding);
	}

	const auto canStartPlay = autoplay
		&& !_streamed
		&& !activeRoundPlaying
		&& !_seeking
		&& !fullHiddenBySpoiler;
	const auto shouldBePlaying = !autoplayUnderCursor() || underCursor(true);
	if (!shouldBePlaying && _videoTimestamp != 0) {
		const_cast<Gif*>(this)->stopAnimation();
	} else if (canStartPlay) {
		const_cast<Gif*>(this)->playAnimation(true);
	} else {
		checkStreamedIsStarted();
	}
	const auto streamingMode = _streamed || activeRoundPlaying || autoplay;
	const auto activeOwnPlaying = activeOwnStreamed();

	auto displayMute = false;
	const auto streamed = activeRoundPlaying
		? activeRoundPlaying
		: activeOwnPlaying
		? &activeOwnPlaying->instance
		: nullptr;
	const auto streamedForWaiting = activeRoundPlaying
		? activeRoundPlaying
		: _streamed
		? &_streamed->instance
		: nullptr;

	if (displayLoading
		&& (!streamedForWaiting
			|| item->isSending()
			|| _data->uploading()
			|| (cornerDownload && _data->loading()))) {
		ensureAnimation();
		if (!_animation->radial.animating()) {
			_animation->radial.start(dataProgress());
		}
	}
	updateStatusText();
	const auto radial = isRadialAnimation()
		|| (streamedForWaiting && streamedForWaiting->waitingShown());

	if (!bubble && !unwrapped && !hostedInstantView) {
		Assert(rounding.has_value());
		fillImageShadow(p, rthumb, *rounding, context);
	}

	const auto skipDrawingContent = context.skipDrawingParts
		== PaintContext::SkipDrawingParts::Content;
	const auto drawStreamed = streamed
		&& (shouldBePlaying || !_videoCover)
		&& (activeRoundPlaying || !_seeking);
	if (drawStreamed && !skipDrawingContent && !fullHiddenBySpoiler) {
		if (!_seekLastFrame.isNull()) {
			_seekLastFrame = QImage();
		}
		auto paused = context.paused || !shouldBePlaying;
		auto request = ::Media::Streaming::FrameRequest{
			.outer = (ScaledInstantViewMediaSize(
				QSize(usew, painth),
				HostedInstantViewMediaPixelScale(_parent))
				* style::DevicePixelRatio()),
			.blurredBackground = true,
		};
		if (isRound) {
			if (activeRoundStreamed()) {
				paused = false;
			} else {
				displayMute = true;
			}
			validateRoundingMask(request.outer);
			request.mask = _roundingMask;
		} else {
			request.rounding = MediaRoundingMask(rounding);
		}
		if (!activeRoundPlaying && activeOwnPlaying->instance.playerLocked()) {
			if (activeOwnPlaying->frozenFrame.isNull()) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
				activeOwnPlaying->frozenStatusText = _statusText;
			} else if (activeOwnPlaying->frozenRequest != request) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
			}
			p.drawImage(rthumb, activeOwnPlaying->frozenFrame);
		} else {
			if (activeOwnPlaying
				&& !activeOwnPlaying->frozenFrame.isNull()) {
				activeOwnPlaying->frozenFrame = QImage();
				activeOwnPlaying->frozenStatusText = QString();
			}

			const auto frame = streamed->frameWithInfo(request);
			p.drawImage(rthumb, frame.image);
			if (_seeking) {
				_seekLastFrame = frame.image;
			}
			if (!paused) {
				streamed->markFrameShown();
			}
		}
	} else if (!_seekLastFrame.isNull()
			&& !skipDrawingContent
			&& !fullHiddenBySpoiler) {
		p.drawImage(rthumb, _seekLastFrame);
	} else if (!skipDrawingContent && !fullHiddenBySpoiler) {
		ensureDataMediaCreated();
		validateThumbCache({ usew, painth }, isRound, rounding);
		p.drawImage(rthumb, _thumbCache);
	}
	if (isRound) {
		paintRoundPlaybackProgress(p, context, rthumb, inTTLViewer);
	}
	if (!isRound) {
		paintTimestampMark(p, rthumb, rounding);
	}

	if (revealed < 1.) {
		p.setOpacity(1. - revealed);
		if (!isRound) {
			p.drawImage(rthumb, _spoiler->background);
			fillImageSpoiler(p, _spoiler.get(), rthumb, context);
		} else {
			auto frame = _spoiler->background;
			{
				auto q = QPainter(&frame);
				fillImageSpoiler(q, _spoiler.get(), rthumb, context);
			}
			p.drawImage(rthumb.topLeft(), Images::Circle(std::move(frame)));
		}
		p.setOpacity(1.);
	}
	if (context.selected()) {
		if (isRound) {
			Ui::FillComplexEllipse(p, st, rthumb);
		} else {
			fillImageOverlay(p, rthumb, rounding, context);
		}
	}

	const auto ttlCovered = _ttlCover && (revealed < 1.);
	const auto paintInCenter = !_sensitiveSpoiler
		&& (radial
			|| (!streamingMode
				&& ((!loaded && !_data->loading()) || !autoplay))
			|| ttlCovered);
	if (paintInCenter) {
		const auto radialRevealed = 1.;
		const auto opacity = (item->isSending() || _data->uploading())
			? 1.
			: streamedForWaiting
			? streamedForWaiting->waitingOpacity()
			: (radial && loaded)
			? _animation->radial.opacity()
			: 1.;
		const auto radialOpacity = opacity * radialRevealed;
		const auto innerSize = st::msgFileLayout.thumbSize;
		auto inner = QRect(rthumb.x() + (rthumb.width() - innerSize) / 2, rthumb.y() + (rthumb.height() - innerSize) / 2, innerSize, innerSize);
		p.setPen(Qt::NoPen);
		if (context.selected()) {
			p.setBrush(st->msgDateImgBgSelected());
		} else if (isThumbAnimation()) {
			auto over = _animation->a_thumbOver.value(1.);
			p.setBrush(anim::brush(st->msgDateImgBg(), st->msgDateImgBgOver(), over));
		} else {
			const auto over = ClickHandler::showAsActive(
				(_data->loading() || _data->uploading()) ? _cancell : _savel);
			p.setBrush(over ? st->msgDateImgBgOver() : st->msgDateImgBg());
		}
		p.setOpacity(radialOpacity * p.opacity());

		{
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(inner);
		}

		p.setOpacity(radialOpacity);
		const auto icon = [&]() -> const style::icon * {
			switch (currentAction(true)) {
			case Action::None:
			case Action::Streaming: return nullptr;
			case Action::Open: return &sti->historyFileThumbPlay;
			case Action::Cancel: return &sti->historyFileThumbCancel;
			case Action::Download: return &sti->historyFileThumbDownload;
			}
			Unexpected("Action in Gif::draw.");
		}();
		if (ttlCovered && !radial && !_data->loading()) {
			paintTtlFire(p, inner);
			paintTtlCountdown(
				p,
				inner,
				st::msgFileRadialLine,
				sti->historyFileThumbRadialFg,
				context.paused);
			PaintTtlSingleViewBadge(p, inner, _realParent, context);
		} else {
			const auto previous = _data->waitingForAlbum()
				? &sti->historyFileThumbCancel
				: nullptr;
			if (icon) {
				if (previous && radialOpacity > 0. && radialOpacity < 1.) {
					PaintInterpolatedIcon(p, *icon, *previous, radialOpacity, inner);
				} else {
					icon->paintInCenter(p, inner);
				}
			}
		}
		p.setOpacity(radialRevealed);
		if (radial) {
			const auto line = st::historyGroupRadialLine;
			const auto rinner = inner.marginsRemoved({ line, line, line, line });
			if (streamedForWaiting && !_data->uploading()) {
				Ui::InfiniteRadialAnimation::Draw(
					p,
					streamedForWaiting->waitingState(),
					rinner.topLeft(),
					rinner.size(),
					width(),
					sti->historyFileThumbRadialFg,
					st::msgFileRadialLine);
			} else if (!cornerDownload) {
				_animation->radial.draw(
					p,
					rinner,
					st::msgFileRadialLine,
					sti->historyFileThumbRadialFg);
			}
		}
		p.setOpacity(1.);
	} else if (_sensitiveSpoiler) {
		drawSpoilerTag(p, rthumb, context, [&] {
			return spoilerTagBackground();
		});
	}
	if (displayMute) {
		auto muteRect = style::rtlrect(rthumb.x() + (rthumb.width() - st::historyVideoMessageMuteSize) / 2, rthumb.y() + st::msgDateImgDelta, st::historyVideoMessageMuteSize, st::historyVideoMessageMuteSize, width());
		p.setPen(Qt::NoPen);
		p.setBrush(sti->msgDateImgBg);
		PainterHighQualityEnabler hq(p);
		p.drawEllipse(muteRect);
		sti->historyVideoMessageMute.paintInCenter(p, muteRect);
	}

	const auto skipDrawingSurrounding = context.skipDrawingParts
		== PaintContext::SkipDrawingParts::Surrounding;

	if (!skipDrawingSurrounding && _purchasedPriceTag) {
		drawPurchasedTag(p, rthumb, context);
	}

	if (!unwrapped && !skipDrawingSurrounding) {
		const auto sponsoredSkip = !_data->isVideoFile()
			&& _realParent->isSponsored();
		if ((!isRound || !inWebPage) && !sponsoredSkip) {
			if (ttlCovered) {
				PaintTtlLabel(p, QPoint(), width(), _realParent, context);
			} else {
				drawCornerStatus(p, context, QPoint());
			}

			// --- NEW: Bottom-Right Info Bubble for Single Video ---
			// Only show if media is loaded (cover/thumbnail/video) and it's not a round message
			// and ONLY if we should show info (hover/selected).
			if (!isRound && !sponsoredSkip && showInfo && (loaded || _data->hasThumbnail() || _data->duration() > 0 || _data->size > 0)) {
				auto durMs = _data->duration();
				if (durMs <= 0 && streamed) {
					const auto played = streamed->player().prepareLegacyState().length;
					if (played != ::Media::kDurationUnavailable
						&& played > 0) {
						durMs = played;
					}
				}
				qint64 durSeconds = std::max<qint64>(0, durMs / 1000);
				qint64 sizeBytes = _data->size;

				if (sizeBytes > 0) {
					const auto font = st::msgDateFont;
					const auto sti = context.imageStyle();
					const auto sizeText = Ui::FormatSizeText(sizeBytes);
					const auto text = (durSeconds > 0)
						? (Ui::FormatDurationText(durSeconds)
							+ QChar(' ')
							+ QChar('(')
							+ sizeText
							+ QChar(')'))
						: sizeText;
					const auto textWidth = font->width(text);
					const auto textHeight = font->height;
					const auto hPadding = 2;
					const auto vPadding = st::msgDateImgPadding.y();
					const auto bubbleW = textWidth + 2 * hPadding;
					const auto bubbleH = textHeight + 2 * vPadding;

					const auto bubbleX = width() - bubbleW - st::msgDateImgDelta;
					const auto bubbleY = height() - bubbleH - st::msgDateImgDelta;

					p.save();
					p.setOpacity(0.95);
					Ui::FillRoundRect(
						p,
						bubbleX,
						bubbleY,
						bubbleW,
						bubbleH,
						sti->msgDateImgBg,
						sti->msgDateImgBgCorners);
					p.restore();

					p.setPen(st->msgDateImgFg());
					p.setFont(font->bold());
					const auto baseY = bubbleY + (bubbleH - textHeight) / 2 + font->ascent;
					p.drawText(bubbleX + hPadding, baseY, text);
				}
			}
			// -----------------------------------------------------
		}
	} else if (!skipDrawingSurrounding) {
		if (isRound) {
			const auto mediaUnread = item->hasUnreadMediaFlag();
			const auto statusText = _seeking
				? Ui::FormatDurationText(1 + int64(base::SafeRound(
					(1. - _roundSeek->progress())
						* _data->duration()
						/ 1000.)))
				: _statusText;
			auto statusW = st::normalFont->width(statusText) + 2 * st::msgDateImgPadding.x();
			auto statusH = st::normalFont->height + 2 * st::msgDateImgPadding.y();
			auto statusX = usex + paintx + st::msgDateImgDelta + st::msgDateImgPadding.x();
			auto statusY = painty + painth - st::msgDateImgDelta - statusH + st::msgDateImgPadding.y();
			if (mediaUnread) {
				statusW += st::mediaUnreadSkip + st::mediaUnreadSize;
			}
			Ui::FillRoundRect(p, style::rtlrect(statusX - st::msgDateImgPadding.x(), statusY - st::msgDateImgPadding.y(), statusW, statusH, width()), sti->msgServiceBg, sti->msgServiceBgCornersSmall);
			p.setFont(st::normalFont);
			// Use consistent edited glyph and color for status; rely on this item's edited state
			const auto editedGlyph = (item->Get<HistoryMessageEdited>() && !item->hideEditedBadge())
				? (QString::fromUtf8("✏️") + " ")
				: QString();
			const auto drawnStatusText = editedGlyph + _statusText;
			p.setPen(st->msgDateImgFg());
			p.drawTextLeft(statusX, statusY, width(), drawnStatusText, statusW - 2 * st::msgDateImgPadding.x());
			if (mediaUnread) {
				p.setPen(Qt::NoPen);
				p.setBrush(st->msgServiceFg());

				{
					PainterHighQualityEnabler hq(p);
					p.drawEllipse(style::rtlrect(statusX - st::msgDateImgPadding.x() + statusW - st::msgDateImgPadding.x() - st::mediaUnreadSize, statusY + st::mediaUnreadTop, st::mediaUnreadSize, st::mediaUnreadSize, width()));
				}
			}
			ensureTranscribeButton();
		}
		const auto ephemeralPlate = _ephemeral.text.isEmpty()
			? QSize()
			: EphemeralPlateSize(
				_ephemeral.text,
				_ephemeral.onTop
					? (width() - st::msgReplyPadding.left())
					: (width() - usew - st::msgReplyPadding.left()));
		const auto platey = painty - _ephemeral.topAdded;
		const auto plateOffset = ephemeralPlate.isEmpty()
			? 0
			: (ephemeralPlate.height() + st::msgReplyPadding.top());
		if (!ephemeralPlate.isEmpty()) {
			auto platex = _ephemeral.onTop
				? (rightAligned ? (width() - ephemeralPlate.width()) : 0)
				: (rightAligned ? 0 : (usew + st::msgReplyPadding.left()));
			if (rtl()) {
				platex = width() - platex - ephemeralPlate.width();
			}
			PaintEphemeralPlate(
				p,
				context,
				_ephemeral.text,
				platex,
				platey,
				ephemeralPlate.width(),
				width());
		}
		if (via || reply || forwarded) {
			auto rectw = _ephemeral.onTop
				? std::min(
					width() - st::msgReplyPadding.left(),
					additionalWidth(reply, via, forwarded))
				: (width() - usew - st::msgReplyPadding.left());
			auto innerw = rectw - (st::msgReplyPadding.left() + st::msgReplyPadding.right());
			auto recth = 0;
			auto forwardedHeightReal = forwarded ? forwarded->text.countHeight(innerw) : 0;
			auto forwardedHeight = qMin(forwardedHeightReal, kMaxGifForwardedBarLines * st::msgServiceNameFont->height);
			if (forwarded) {
				recth += st::msgReplyPadding.top() + forwardedHeight;
			} else if (via) {
				recth += st::msgReplyPadding.top() + st::msgServiceNameFont->height + (reply ? st::msgReplyPadding.top() : 0);
			}
			if (reply) {
				const auto replyMargins = reply->margins();
				recth += reply->height()
					- ((forwarded || via) ? 0 : replyMargins.top())
					- replyMargins.bottom();
			} else {
				recth += st::msgReplyPadding.bottom();
			}
			int rectx = _ephemeral.onTop
				? (rightAligned ? (width() - rectw) : 0)
				: (rightAligned ? 0 : (usew + st::msgReplyPadding.left()));
			int recty = platey + plateOffset;
			if (rtl()) rectx = width() - rectx - rectw;

			Ui::FillRoundRect(p, rectx, recty, rectw, recth, sti->msgServiceBg, sti->msgServiceBgCornersSmall);
			p.setPen(st->msgServiceFg());
			const auto textx = rectx + st::msgReplyPadding.left();
			const auto textw = rectw - st::msgReplyPadding.left() - st::msgReplyPadding.right();
			if (forwarded) {
				p.setTextPalette(st->serviceTextPalette());
				auto breakEverywhere = (forwardedHeightReal > forwardedHeight);
				forwarded->text.drawElided(p, textx, recty + st::msgReplyPadding.top(), textw, kMaxGifForwardedBarLines, style::al_left, 0, -1, 0, breakEverywhere);
				p.restoreTextPalette();

				const auto skip = std::min(
					forwarded->text.countHeight(textw),
					kMaxGifForwardedBarLines * st::msgServiceNameFont->height);
				recty += skip;
			} else if (via) {
				p.setFont(st::msgServiceNameFont);
				p.drawTextLeft(textx, recty + st::msgReplyPadding.top(), 2 * textx + textw, via->text);
				int skip = st::msgServiceNameFont->height + (reply ? st::msgReplyPadding.top() : 0);
				recty += skip;
			}
			if (reply) {
				if (forwarded || via) {
					recty += st::msgReplyPadding.top();
					recth -= st::msgReplyPadding.top();
				} else {
					recty -= reply->margins().top();
				}
				reply->paint(p, _parent, context, rectx, recty, rectw, false);
			}
		}
	}
	if (!inWebPage && !skipDrawingSurrounding) {
		auto fullRight = paintx + usex + usew;
		auto fullBottom = painty + painth;
		auto maxRight = _parent->width() - st::msgMargin.left();
		if (_parent->hasFromPhoto()) {
			maxRight -= st::msgMargin.right();
		} else {
			maxRight -= st::msgMargin.left();
		}
		if (unwrapped
			&& !rightAligned
			&& !_parent->hidesBottomInfo()) {
			auto infoWidth = _parent->infoWidth();
			fullRight += infoWidth - st::normalFont->height;
			if (fullRight > maxRight) {
				fullRight = maxRight;
			}
		}
		
		if (isRound) {
			// Helper to get formatted local date/time

			const auto font = st::msgDateFont;
			const auto sti = context.imageStyle();
			const auto views = item->Get<HistoryMessageViews>();
			const auto viewsText = (views && views->views.count >= 0)
				? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
				: QString();
			
			const auto authorName = [&] {
				if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
					return msgsigned->author;
				}
				return item->from()->name();
			}();
			const auto timeText = QLocale().toString(
				ItemDateTime(item).time(),
				GetEnhancedBool("show_seconds")
					? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
					: QLocale::system().timeFormat(QLocale::ShortFormat));
			const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
				? QString(" (%1)").arg(item->fullId().msg.bare)
				: QString();

			// Construct display string: "[ViewsIcon] [Count] [AuthorName] [Time] [ID]"
			// Note: We'll draw the icon manually on the far left.
			QString displayText = viewsText;
			if (!authorName.isEmpty()) displayText += " " + authorName;
			displayText += " " + timeText + msgIdText;

			const int textWidth = font->width(displayText);
			const int textHeight = font->height;
			const int iconGap = 3;
			const int iconW = st::historyViewsWidth;
			const int hPadding = st::msgDateImgPadding.x();
			const int vPadding = st::msgDateImgPadding.y();
			
			int totalW = hPadding + (viewsText.isEmpty() ? 0 : (iconW + iconGap)) + textWidth + hPadding;
			int totalH = textHeight + 2 * vPadding;

			int bubbleX = fullRight - totalW - st::msgDateImgDelta;
			int bubbleY = fullBottom - totalH - st::msgDateImgDelta;

			// Draw Bubble Background
			p.save();
			p.setOpacity(0.95);
			Ui::FillRoundRect(
				p,
				bubbleX,
				bubbleY,
				totalW,
				totalH,
				(unwrapped ? sti->msgServiceBg : sti->msgDateImgBg),
				(unwrapped ? sti->msgServiceBgCornersSmall : sti->msgDateImgBgCorners));
			p.restore();

			// Draw Content
			p.setPen(unwrapped ? st->msgServiceFg() : st->msgDateImgFg());
			p.setFont(font->bold());
			int currentX = bubbleX + hPadding;
			const int textBaseY = bubbleY + vPadding + font->ascent;

			if (!viewsText.isEmpty()) {
				const auto &icon = st->historyViewsInvertedIcon();
				const int baseIconW = std::max(1, icon.width());
				const int baseIconH = icon.height();
				const int scaledIconH = (baseIconH * iconW) / baseIconW;
				icon.paint(p, currentX, bubbleY + (totalH - scaledIconH) / 2 + 1, iconW);
				currentX += iconW + iconGap;
			}
			p.drawText(currentX, textBaseY, displayText);
		}

		if (const auto size = bubble ? std::nullopt : _parent->rightActionSize()
			; size || (_transcribe && !rightAligned)) {
			const auto rightActionWidth = size
				? size->width()
				: _transcribe->size().width();
			auto fastShareLeft = rightLayout
				? (paintx + usex - size->width() - st::historyFastShareLeft)
				: (fullRight + st::historyFastShareLeft);
			auto fastShareTop = fullBottom
				- st::historyFastShareBottom
				- (size ? size->height() : 0);
			if (fastShareLeft + rightActionWidth > maxRight) {
				const auto hidesBottomInfo = _parent->hidesBottomInfo();
				fastShareLeft = fullRight
					- rightActionWidth
					- (hidesBottomInfo ? 0 : st::msgDateImgDelta);
				if (!hidesBottomInfo) {
					fastShareTop -= st::msgDateImgDelta
						+ st::msgDateImgPadding.y()
						+ st::msgDateFont->height
						+ st::msgDateImgPadding.y();
				}
			}
			if (size) {
				_parent->drawRightAction(p, context, fastShareLeft, fastShareTop, 2 * paintx + paintw);
			}
			if (_transcribe) {
				paintTranscribe(p, fastShareLeft, fastShareTop, true, context);
			}
		} else if (rightAligned && _transcribe) {
			paintTranscribe(p, usex, fullBottom, false, context);
		}
	}
	if (_drawTtl) {
		_drawTtl(p, rthumb, context);
	}

	// --- CUSTOM TOP-RIGHT BUBBLE (For Standard Videos) ---
	// Always draw top-right for videos even if caption exists.
	// Only draw on hover/select.
	if (!isRound && !inWebPage && !_parent->data()->isFakeAboutView() && showInfo) {
		// Local definition to fix compilation error

		const auto font = st::msgDateFont;
		p.setFont(font);

		const auto edited = item->Get<HistoryMessageEdited>() && !item->hideEditedBadge();
		const auto dateText = QLocale().toString(
			ItemDateTime(item).time(),
			GetEnhancedBool("show_seconds")
				? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
				: QLocale::system().timeFormat(QLocale::ShortFormat));

		const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
			? QString(" (%1)").arg(item->fullId().msg.bare)
			: QString();

		const auto views = item->Get<HistoryMessageViews>();
		const auto viewsText = (views && views->views.count >= 0)
			? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
			: QString();

		int totalWidth = 0;
		const int textPadding = font->width(' ');
		totalWidth += font->width(dateText + msgIdText);

		if (edited) {
			totalWidth += textPadding + font->width(QString::fromUtf8("✏️"));
		}

		int viewsWidth = 0;
		if (!viewsText.isEmpty()) {
			viewsWidth = st::historyViewsWidth + 1 + font->width(viewsText);
			totalWidth += (2 * textPadding) + viewsWidth;
		}

		const auto textHeight = font->height;
		const auto hPadding = 2;
		const auto vPadding = st::msgDateImgPadding.y();
		const auto bubbleW = totalWidth + 2 * hPadding;
		const auto bubbleH = textHeight + 2 * vPadding;

		// Position: Top-Right
		const auto bubbleX = width() - bubbleW - st::msgDateImgDelta;
		const auto bubbleY = st::msgDateImgDelta;

		p.save();
		p.setOpacity(0.95);
		Ui::FillRoundRect(p, bubbleX, bubbleY, bubbleW, bubbleH, sti->msgDateImgBg, sti->msgDateImgBgCorners);
		p.restore();

		p.setPen(st->msgDateImgFg());
		p.setFont(font->bold());
		const int textBaseY = bubbleY + (bubbleH - textHeight) / 2 + font->ascent;
		int currentLeft = bubbleX + hPadding;

		if (!viewsText.isEmpty()) {
			const auto &icon = st->historyViewsInvertedIcon();
			const int baseIconW = std::max(1, icon.width());
			const int baseIconH = icon.height();
			const int scaledIconH = (baseIconH * st::historyViewsWidth) / baseIconW;
			icon.paint(p, currentLeft, bubbleY + (bubbleH - scaledIconH) / 2 + 1, st::historyViewsWidth);
			p.drawText(currentLeft + st::historyViewsWidth + 1, textBaseY, viewsText);
			currentLeft += viewsWidth + textPadding;
		}
		if (edited) {
			const auto editedText = QString::fromUtf8("✏️");
			p.setFont(font);
			p.drawText(currentLeft, textBaseY, editedText);
			currentLeft += font->width(editedText) + textPadding;
			p.setFont(font->bold());
		}
		p.drawText(currentLeft, textBaseY, dateText + msgIdText);
	}
}

void Gif::paintTranscribe(
		Painter &p,
		int x,
		int y,
		bool right,
		const PaintContext &context) const {
	if (!_transcribe) {
		return;
	}
	const auto s = _transcribe->size();
	_transcribe->paint(
		p,
		x - (right ? 0 : s.width()),
		y - s.height() - st::msgDateImgDelta,
		context);
}

void Gif::paintTimestampMark(
		Painter &p,
		QRect rthumb,
		std::optional<Ui::BubbleRounding> rounding) const {
	if (_videoTimestamp <= 0 && _videoPosition < crl::time(200)) {
		return;
	}
	PaintVideoTimestampMark(
		p,
		rthumb,
		rounding,
		((_videoPosition > 0)
			? _videoPosition
			: (_videoTimestamp * crl::time(1000))),
		_data->duration());
}

void Gif::paintRoundPlaybackProgress(
		Painter &p,
		const PaintContext &context,
		QRect rthumb,
		bool inTTLViewer) const {
	const auto playback = videoPlayback();
	_roundSeek->paint(
		p,
		context,
		rthumb,
		roundSeekShown(),
		_seeking,
		playback ? playback->value() : -1.,
		inTTLViewer);
}

void Gif::drawSpoilerTag(
		Painter &p,
		QRect rthumb,
		const PaintContext &context,
		Fn<QImage()> generateBackground) const {
	Media::drawSpoilerTag(
		p,
		_spoiler.get(),
		_spoilerTag,
		rthumb,
		context,
		std::move(generateBackground));
}

ClickHandlerPtr Gif::spoilerTagLink() const {
	return Media::spoilerTagLink(_spoiler.get(), _spoilerTag);
}

QImage Gif::spoilerTagBackground() const {
	return _spoiler ? _spoiler->background : QImage();
}

void Gif::validateVideoThumbnail() const {
	Expects(!_videoCover);

	const auto content = _dataMedia->videoThumbnailContent();
	if (_videoThumbnailFrame || content.isEmpty()) {
		return;
	}
	auto info = v::get<Ui::PreparedFileInformation::Video>(
		::Media::Clip::PrepareForSending(QString(), content).media);
	_videoThumbnailFrame = std::make_unique<Image>(info.thumbnail.isNull()
		? Image::BlankMedia()->original()
		: info.thumbnail);
}

void Gif::validateThumbCache(
		QSize outer,
		bool isEllipse,
		std::optional<Ui::BubbleRounding> rounding) const {
	const auto good = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Large)
		: _dataMedia->goodThumbnail();
	const auto normal = good
		? good
		: _videoCoverMedia
		? nullptr
		: _dataMedia->thumbnail();
	if (!normal) {
		if (_videoCoverMedia) {
			_videoCover->load(Data::PhotoSize::Small, _realParent->fullId());
		} else {
			_data->loadThumbnail(_realParent->fullId());
			validateVideoThumbnail();
		}
	}
	const auto videothumb = (normal || _videoCoverMedia)
		? nullptr
		: _videoThumbnailFrame.get();
	const auto blurred = normal
		? (!good
			&& (normal->width() < kUseNonBlurredThreshold)
			&& (normal->height() < kUseNonBlurredThreshold)
			&& !_data->isVideoFile())
		: !videothumb;
	const auto ratio = style::DevicePixelRatio();
	const auto scaled = ScaledInstantViewMediaSize(
		outer,
		HostedInstantViewMediaPixelScale(_parent));
	if (_thumbCache.size() == (scaled * ratio)
		&& _thumbCacheRounding == rounding
		&& _thumbCacheBlurred == blurred
		&& _thumbIsEllipse == isEllipse) {
		return;
	}
	auto cache = prepareThumbCache(scaled);
	_thumbCache = isEllipse
		? Images::Circle(std::move(cache))
		: Images::Round(std::move(cache), MediaRoundingMask(rounding));
	_thumbCacheRounding = rounding;
	_thumbCacheBlurred = blurred;
}

QImage Gif::prepareThumbCache(QSize outer) const {
	const auto good = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Large)
		: _dataMedia->goodThumbnail();
	const auto normal = good
		? good
		: _videoCoverMedia
		? nullptr
		: _dataMedia->thumbnail();
	const auto videothumb = (normal || _videoCoverMedia)
		? nullptr
		: _videoThumbnailFrame.get();
	auto blurred = (!good
		&& normal
		&& (normal->width() < kUseNonBlurredThreshold)
		&& (normal->height() < kUseNonBlurredThreshold)
		&& !_data->isVideoFile())
		? normal
		: nullptr;
	const auto blurFromLarge = good || (normal && !blurred);
	const auto large = blurFromLarge ? normal : videothumb;
	if (videothumb) {
	} else if (_videoCoverMedia) {
		if (const auto embedded = _videoCoverMedia->thumbnailInline()) {
			blurred = embedded;
		}
	} else if (const auto embedded = _dataMedia->thumbnailInline()) {
		blurred = embedded;
	}
	if (!large && !blurred && !_data->hasThumbnail() && _data->isVideoFile()) {
		if (GetEnhancedBool("custom_file_thumbs")) {
			auto thumb = QImage();
			const auto path = GetEnhancedString("custom_thumb_path");
			if (!path.isEmpty()) {
				thumb = QImage(path);
			}
			if (!thumb.isNull()) {
				return PrepareWithBlurredBackground(
					outer,
					::Media::Streaming::DecideVideoFrameResize(
						outer,
						thumb.size()),
					thumb,
					QImage());
			}
		}
	}
	const auto resize = large
		? ::Media::Streaming::DecideVideoFrameResize(
			outer,
			large->size())
		: ::Media::Streaming::ExpandDecision();
	return PrepareWithBlurredBackground(
		outer,
		resize,
		large,
		blurFromLarge ? large : blurred);
}

void Gif::validateSpoilerImageCache(
		QSize outer,
		std::optional<Ui::BubbleRounding> rounding) const {
	Expects(_spoiler != nullptr);

	const auto ratio = style::DevicePixelRatio();
	const auto scaled = ScaledInstantViewMediaSize(
		outer,
		HostedInstantViewMediaPixelScale(_parent));
	if (_spoiler->background.size() == (scaled * ratio)
		&& _spoiler->backgroundRounding == rounding) {
		return;
	}
	const auto normal = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Small)
		: _dataMedia->thumbnail();
	auto container = std::optional<Image>();
	const auto downscale = [&](Image *image) {
		if (!image || (image->width() <= 40 && image->height() <= 40)) {
			return image;
		}
		container.emplace(image->original().scaled(
			{ 40, 40 },
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation));
		return &*container;
	};
	const auto embedded = _videoCoverMedia
		? _videoCoverMedia->thumbnailInline()
		: _dataMedia->thumbnailInline();
	const auto blurred = embedded ? embedded : downscale(normal);
	_spoiler->background = Images::Round(
		PrepareWithBlurredBackground(
			scaled,
			::Media::Streaming::ExpandDecision(),
			nullptr,
			blurred),
		MediaRoundingMask(rounding));
	_spoiler->backgroundRounding = rounding;
}

void Gif::drawCornerStatus(
		Painter &p,
		const PaintContext &context,
		QPoint position) const {
	// Only draw if NOT loaded (i.e. if we need to show download/cancel)
	// and if we are downloading in corner (isVideoFile).
	if (!downloadInCorner() || dataLoaded()) {
		return;
	}

	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto padding = st::msgDateImgPadding;
	const auto radial = _animation && _animation->radial.animating(); // Check animation state

	// Position for Top-Left
	// Use padding.y() for both to have equal gaps.
	const auto statusX = position.x() + st::msgDateImgDelta + padding.y();
	const auto statusY = position.y() + st::msgDateImgDelta + padding.y();
	
	// Create a square rect for the button
	const auto buttonSize = 31; // 70% of 44px
	const auto inner = QRect(statusX, statusY, buttonSize, buttonSize);

	// Draw Circular Background (Semi-transparent black)
	{
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(sti->msgDateImgBg);
		p.drawEllipse(inner);
	}

	// Draw Icon (Download or Cancel)
	const auto &icon = _data->loading()
		? sti->historyVideoCancel
		: sti->historyVideoDownload;
	
	{
		p.save();
		const auto ratio = float64(buttonSize) / st::historyVideoDownloadSize;
		p.translate(inner.center());
		p.scale(ratio, ratio);
		icon.paintInCenter(p, QRect(-st::historyVideoDownloadSize / 2, -st::historyVideoDownloadSize / 2, st::historyVideoDownloadSize, st::historyVideoDownloadSize));
		p.restore();
	}

	// Draw Radial Progress if animating
	if (radial) {
		const auto radialLine = int(st::historyVideoRadialLine * (float64(buttonSize) / st::historyVideoDownloadSize));
		QRect rinner(inner.marginsRemoved(QMargins(radialLine, radialLine, radialLine, radialLine)));
		_animation->radial.draw(p, rinner, radialLine, sti->historyFileThumbRadialFg);
	}
}

TextState Gif::cornerStatusTextState(
		QPoint point,
		StateRequest request,
		QPoint position) const {
	auto result = TextState(_parent);
	if (!needCornerStatusDisplay() || !downloadInCorner() || dataLoaded()) {
		return result;
	}
	const auto padding = st::msgDateImgPadding;
	const auto statusX = position.x() + st::msgDateImgDelta + padding.y();
	const auto statusY = position.y() + st::msgDateImgDelta + padding.y();
	const auto buttonSize = 31;
	const auto inner = QRect(statusX, statusY, buttonSize, buttonSize);
	if (inner.contains(point)) {
		result.link = _data->loading() ? _cancell : _savel;
	}
	return result;
}

TextState Gif::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);

	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return result;
	}
	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	auto bubble = _parent->hasBubble();

	const auto rightLayout = _parent->hasRightLayout();
	const auto inWebPage = (_parent->media() != this);
	const auto isRound = _data->isVideoMessage();
	const auto unwrapped = isUnwrapped();
	const auto item = _parent->data();
	auto usew = paintw, usex = 0;
	const auto via = unwrapped ? item->Get<HistoryMessageVia>() : nullptr;
	const auto reply = unwrapped ? _parent->Get<Reply>() : nullptr;
	const auto forwarded = unwrapped ? item->Get<HistoryMessageForwarded>() : nullptr;
	const auto rightAligned = unwrapped && rightLayout;
	if (via || reply || forwarded || !_ephemeral.text.isEmpty()) {
		usew = maxWidth() - additionalWidth(reply, via, forwarded);
		if (rightAligned) {
			usex = width() - usew;
		}
	}
	if (_ephemeral.onTop) {
		painty += _ephemeral.topAdded;
		painth -= _ephemeral.topAdded;
	}
	if (isRound) {
		accumulate_min(usew, painth);
	}
	if (rtl()) usex = width() - usex - usew;

	// --- CUSTOM TOOLTIP LOGIC (Top-Right Bubble for Standard Videos) ---
	// FIX Issue 3: Implemented 2-zone tooltip logic here for Top-Right bubble
	if (!isRound && !inWebPage && !_parent->data()->isFakeAboutView()) {

		const auto font = st::msgDateFont;
		
		const auto edited = item->Get<HistoryMessageEdited>() && !item->hideEditedBadge();
		const auto dateText = QLocale().toString(
			ItemDateTime(item).time(),
			GetEnhancedBool("show_seconds")
				? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
				: QLocale::system().timeFormat(QLocale::ShortFormat));

		const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
			? QString(" (%1)").arg(item->fullId().msg.bare)
			: QString();

		const auto views = item->Get<HistoryMessageViews>();
		const auto viewsText = (views && views->views.count >= 0)
			? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
			: QString();

		int totalWidth = 0;
		const int textPadding = font->width(' ');
		const int dateWidth = font->width(dateText + msgIdText);
		totalWidth += dateWidth;

		int editedWidth = 0;
		if (edited) {
			editedWidth = font->width(QString::fromUtf8("✏️"));
			totalWidth += textPadding + editedWidth;
		}

		int viewsWidth = 0;
		if (!viewsText.isEmpty()) {
			viewsWidth = st::historyViewsWidth + 1 + font->width(viewsText);
			totalWidth += (2 * textPadding) + viewsWidth;
		}

		const auto textHeight = font->height;
		const auto hPadding = 2;
		const auto vPadding = st::msgDateImgPadding.y();
		const auto bubbleW = totalWidth + 2 * hPadding;
		const auto bubbleH = textHeight + 2 * vPadding;

		// Position: Top-Right
		const auto bubbleX = width() - bubbleW - st::msgDateImgDelta;
		const auto bubbleY = st::msgDateImgDelta;
		const QRect bubbleRect(bubbleX, bubbleY, bubbleW, bubbleH);

		// Hit Testing
		if (bubbleRect.contains(point)) {
			int currentX = bubbleX + hPadding;

			// --- Zone 1: Views ---
			QRect viewsRect;
			if (viewsWidth > 0) {
				viewsRect = QRect(currentX, bubbleY, viewsWidth, bubbleH);
				currentX += viewsWidth + textPadding;
			}

			// --- Zone 2: Edited + Date + ID ---
			int zone2Start = currentX;
			int calculatedZone2W = dateWidth + (edited ? (editedWidth + textPadding) : 0);
			QRect zone2Rect(zone2Start, bubbleY, calculatedZone2W, bubbleH);

			// Logic
			if (viewsWidth > 0 && viewsRect.contains(point)) {
				result.customTooltip = true;
				result.customTooltipText = QString("Views: ") + viewsText;
				return result;
			} 
			else if (zone2Rect.contains(point)) {
				const auto uploadLocal = ItemDateTime(item);
				QString text = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocal.time().toString("HH:mm:ss");
				
				if (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0) {
					text += "  ID: " + QString::number(item->fullId().msg.bare);
				}

				if (edited) {
					const auto editLocal = base::unixtime::parse(item->Get<HistoryMessageEdited>()->date);
					QString editedTrans = tr::lng_edited(tr::now);
					editedTrans = editedTrans.toUpper().left(1) + editedTrans.mid(1);
					text += "\n" + editedTrans + ": "
						+ editLocal.date().toString("dddd, dd MMMM yyyy") + " "
						+ editLocal.time().toString("HH:mm:ss");
				}

				result.customTooltip = true;
				result.customTooltipText = text;
				return result;
			}
		}
	}
	// ----------------------------------------------------------

	const auto ephemeralPlate = _ephemeral.text.isEmpty()
		? QSize()
		: EphemeralPlateSize(
			_ephemeral.text,
			_ephemeral.onTop
				? (paintw - st::msgReplyPadding.left())
				: (paintw - usew - st::msgReplyPadding.left()));
	const auto platey = painty - _ephemeral.topAdded;
	const auto plateOffset = ephemeralPlate.isEmpty()
		? 0
		: (ephemeralPlate.height() + st::msgReplyPadding.top());
	if (!ephemeralPlate.isEmpty()) {
		auto platex = _ephemeral.onTop
			? (rightAligned ? (width() - ephemeralPlate.width()) : 0)
			: (rightAligned ? 0 : (usew + st::msgReplyPadding.left()));
		if (rtl()) {
			platex = width() - platex - ephemeralPlate.width();
		}
		if (EphemeralPlateState(
				_parent,
				_ephemeral.text,
				point,
				platex,
				platey,
				ephemeralPlate.width(),
				ephemeralPlate.height(),
				request,
				result)) {
			return result;
		}
	}
	if (via || reply || forwarded) {
		auto rectw = _ephemeral.onTop
			? std::min(
				paintw - st::msgReplyPadding.left(),
				additionalWidth(reply, via, forwarded))
			: (paintw - usew - st::msgReplyPadding.left());
		auto innerw = rectw - (st::msgReplyPadding.left() + st::msgReplyPadding.right());
		auto recth = 0;
		auto forwardedHeightReal = forwarded ? forwarded->text.countHeight(innerw) : 0;
		auto forwardedHeight = qMin(forwardedHeightReal, kMaxGifForwardedBarLines * st::msgServiceNameFont->height);
		if (forwarded) {
			recth += st::msgReplyPadding.top() + forwardedHeight;
		} else if (via) {
			recth += st::msgReplyPadding.top() + st::msgServiceNameFont->height + (reply ? st::msgReplyPadding.top() : 0);
		}
		if (reply) {
			const auto replyMargins = reply->margins();
			recth += reply->height()
				- ((forwarded || via) ? 0 : replyMargins.top())
				- replyMargins.bottom();
		} else {
			recth += st::msgReplyPadding.bottom();
		}
		auto rectx = _ephemeral.onTop
			? (rightAligned ? (width() - rectw) : 0)
			: (rightAligned ? 0 : (usew + st::msgReplyPadding.left()));
		auto recty = platey + plateOffset;
		if (rtl()) rectx = width() - rectx - rectw;

		if (forwarded) {
			if (QRect(rectx, recty, rectw, st::msgReplyPadding.top() + forwardedHeight).contains(point)) {
				auto breakEverywhere = (forwardedHeightReal > forwardedHeight);
				auto textRequest = request.forText();
				if (breakEverywhere) {
					textRequest.flags |= Ui::Text::StateRequest::Flag::BreakEverywhere;
				}
				result = TextState(_parent, forwarded->text.getState(
					point - QPoint(rectx + st::msgReplyPadding.left(), recty + st::msgReplyPadding.top()),
					innerw,
					textRequest));
				result.symbol = 0;
				result.afterSymbol = false;
				if (breakEverywhere) {
					result.cursor = CursorState::Forwarded;
				} else {
					result.cursor = CursorState::None;
				}
				return result;
			}
			recty += forwardedHeight;
			recth -= forwardedHeight;
		} else if (via) {
			auto viah = st::msgReplyPadding.top() + st::msgServiceNameFont->height + (reply ? 0 : st::msgReplyPadding.bottom());
			if (QRect(rectx, recty, rectw, viah).contains(point)) {
				result.link = via->link;
				return result;
			}
			auto skip = st::msgServiceNameFont->height + (reply ? 2 * st::msgReplyPadding.top() : 0);
			recty += skip;
			recth -= skip;
		}
		if (reply) {
			if (forwarded || via) {
				recty += st::msgReplyPadding.top();
				recth -= st::msgReplyPadding.top() + reply->margins().top();
			} else {
				recty -= reply->margins().top();
			}
			const auto replyRect = QRect(rectx, recty, rectw, recth);
			if (replyRect.contains(point)) {
				result.link = reply->link();
				reply->saveRipplePoint(point - replyRect.topLeft());
				reply->createRippleAnimation(_parent, replyRect.size());
				return result;
			}
		}
	}
	if (!unwrapped) {
		if (const auto state = cornerStatusTextState(point, request, QPoint()); state.link) {
			return state;
		}
	}
	if (isRound) {
		const auto center = QPoint(usex + paintx + usew / 2, painty + usew / 2);
		const auto radius = usew / 2;
		const auto diff = point - center;
		if (diff.x() * diff.x() + diff.y() * diff.y() <= radius * radius) {
			ensureDataMediaCreated();
			result.link = (_spoiler && !_spoiler->revealed)
				? (_sensitiveSpoiler
					? spoilerTagLink()
					: (isRound && _parent->data()->media()->ttlSeconds())
					? _openl // Overriden.
					: _spoiler->link)
				: currentVideoLink(true);
		}
	} else if (QRect(usex + paintx, painty, usew, painth).contains(point)) {
		ensureDataMediaCreated();
		if (_spoiler && !_spoiler->revealed) {
			const auto media = _parent->data()->media();
			result.link = _sensitiveSpoiler
				? spoilerTagLink()
				: (isRound && media && media->ttlSeconds())
				? _openl
				: _spoiler->link;
		} else if (_seekl && isRoundSeekable()) {
			_seekStatePoint = point;
			result.link = _seekl;
		} else {
			result.link = currentVideoLink(true);
		}
	}
	const auto checkBottomInfo = !inWebPage
		&& (unwrapped || !bubble || isBubbleBottom());
	if (checkBottomInfo) {
		auto fullRight = usex + paintx + usew;
		auto fullBottom = painty + painth;
		auto maxRight = _parent->width() - st::msgMargin.left();
		if (_parent->hasFromPhoto()) {
			maxRight -= st::msgMargin.right();
		} else {
			maxRight -= st::msgMargin.left();
		}
		if (unwrapped
			&& !rightAligned
			&& !_parent->hidesBottomInfo()) {
			auto infoWidth = _parent->infoWidth();

			// This is just some arbitrary point,
			// the main idea is to make info left aligned here.
			fullRight += infoWidth - st::normalFont->height;
			if (fullRight > maxRight) {
				fullRight = maxRight;
			}
		}
		
		// FIX Issue 3: Only call standard bottom info if it's a Round Video (Video Note).
		// AND implement custom tooltip for round videos.
		if (isRound) {
			// But for hit testing we use our custom bubble rect from draw()
			// Need to replicate calculation:
			const auto font = st::msgDateFont;
			const auto views = item->Get<HistoryMessageViews>();
			const auto viewsText = (views && views->views.count >= 0)
				? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
				: QString();
			const auto authorName = [&] {
				if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
					return msgsigned->author;
				}
				return item->from()->name();
			}();
			const auto timeText = QLocale().toString(
				ItemDateTime(item).time(),
				GetEnhancedBool("show_seconds")
					? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
					: QLocale::system().timeFormat(QLocale::ShortFormat));
			const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
				? QString(" (%1)").arg(item->fullId().msg.bare)
				: QString();

			QString displayText = viewsText;
			if (!authorName.isEmpty()) displayText += " " + authorName;
			displayText += " " + timeText + msgIdText;

			const int iconW = st::historyViewsWidth;
			const int iconGap = 3;
			const int hPadding = st::msgDateImgPadding.x();
			const int vPadding = st::msgDateImgPadding.y();
			int totalW = hPadding + (viewsText.isEmpty() ? 0 : (iconW + iconGap)) + font->width(displayText) + hPadding;
			int totalH = font->height + 2 * vPadding;

			int bubbleX = fullRight - totalW - st::msgDateImgDelta;
			int bubbleY = fullBottom - totalH - st::msgDateImgDelta;
			QRect bubbleRect(bubbleX, bubbleY, totalW, totalH);

			if (bubbleRect.contains(point)) {
				QString tooltip;
				// Row 1: Author, Views, Shares
				QString row1;
				if (const auto authorName = [&] {
					if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
						return msgsigned->author;
					}
					return item->from()->name();
				}(); !authorName.isEmpty()) {
					row1 += tr::lng_signed_author(tr::now, lt_user, authorName);
				}
				if (views && views->views.count >= 0) {
					if (!row1.isEmpty()) row1 += ", ";
					row1 += tr::lng_views_tooltip(tr::now, lt_count_decimal, views->views.count);
				}
				if (views && views->forwardsCount > 0) {
					if (!row1.isEmpty()) row1 += ", ";
					row1 += tr::lng_forwards_tooltip(tr::now, lt_count_decimal, views->forwardsCount);
				}
				tooltip = row1;

				// Row 2: Uploaded (Localized) + ID
				const auto uploadLocal = ItemDateTime(item);
				
				QString row2 = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocal.time().toString("HH:mm:ss");
				
				if (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0) {
					row2 += " ID: " + QString::number(item->fullId().msg.bare);
				}

				if (const auto edited = item->Get<HistoryMessageEdited>()) {
					const auto editLocal = base::unixtime::parse(edited->date);
					QString editedTrans = tr::lng_edited(tr::now);
					editedTrans = editedTrans.toUpper().left(1) + editedTrans.mid(1);
					row2 += "\n" + editedTrans + ": "
						+ editLocal.date().toString("dddd, dd MMMM yyyy") + " "
						+ editLocal.time().toString("HH:mm:ss");
				}
				
				if (!tooltip.isEmpty()) tooltip += "\n";
				tooltip += row2;

				result.customTooltip = true;
				result.customTooltipText = tooltip;
				return result;
			}
		}
		// -----------------------------------------------------------------------

		if (const auto size = bubble ? std::nullopt : _parent->rightActionSize()) {
			const auto rightActionWidth = size->width();
			auto fastShareLeft = _parent->hasRightLayout()
				? (paintx + usex - size->width() - st::historyFastShareLeft)
				: (fullRight + st::historyFastShareLeft);
			auto fastShareTop = fullBottom
				- st::historyFastShareBottom
				- size->height();
			if (fastShareLeft + rightActionWidth > maxRight) {
				const auto hidesBottomInfo = _parent->hidesBottomInfo();
				fastShareLeft = fullRight
					- rightActionWidth
					- (hidesBottomInfo ? 0 : st::msgDateImgDelta);
				if (!hidesBottomInfo) {
					fastShareTop -= st::msgDateImgDelta
						+ st::msgDateImgPadding.y()
						+ st::msgDateFont->height
						+ st::msgDateImgPadding.y();
				}
			}
			if (QRect(QPoint(fastShareLeft, fastShareTop), *size).contains(point)) {
				result.link = _parent->rightActionLink(point
					- QPoint(fastShareLeft, fastShareTop));
			}
		}
		if (_transcribe && _transcribe->contains(point)) {
			result.link = _transcribe->link();
		}
	}
	return result;
}

void Gif::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (_seekl && handler == _seekl) {
		if (pressed && !_seeking) {
			_seekPressPoint = QPoint(-1, -1);
			if (const auto playback = videoPlayback()) {
				_roundSeek->setProgress(playback->value());
			}
			const auto rthumb = roundThumbRect();
			if (roundSeekShown()
				&& _roundSeek->grabPoint(rthumb, _seekStatePoint)) {
				startRoundSeeking();
				updateRoundSeeking(rthumb, _seekStatePoint);
			}
		} else if (!pressed) {
			if (_seeking) {
				if (isRoundSeekable()) {
					::Media::Player::instance()->finishSeeking(
						AudioMsgId::Type::Voice,
						_roundSeek->progress());
				}
				_seeking = false;
				_roundSeek->setGrabbed(false);
				repaint();
			} else if (_seekPressPoint != QPoint()) {
				_seekPressPoint = QPoint();
				::Media::Player::instance()->playPauseCancelClicked(
					AudioMsgId::Type::Voice);
			}
		}
	}
	File::clickHandlerPressedChanged(handler, pressed);
	if (!handler) {
		return;
	} else if (_transcribe && (handler == _transcribe->link())) {
		if (pressed) {
			_transcribe->addRipple([=] { repaint(); });
		} else {
			_transcribe->stopRipple();
		}
	}
}

QRect Gif::roundThumbRect() const {
	const auto item = _parent->data();
	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	const auto unwrapped = isUnwrapped();
	auto usew = paintw, usex = 0;
	const auto via = unwrapped ? item->Get<HistoryMessageVia>() : nullptr;
	const auto reply = unwrapped ? _parent->Get<Reply>() : nullptr;
	const auto forwarded = unwrapped
		? item->Get<HistoryMessageForwarded>()
		: nullptr;
	if (via || reply || forwarded || !_ephemeral.text.isEmpty()) {
		usew = maxWidth() - additionalWidth(reply, via, forwarded);
		if (unwrapped && _parent->hasRightLayout()) {
			usex = width() - usew;
		}
	}
	if (_ephemeral.onTop) {
		painty += _ephemeral.topAdded;
		painth -= _ephemeral.topAdded;
	}
	accumulate_min(usew, painth);
	if (rtl()) usex = width() - usex - usew;
	return style::rtlrect(usex + paintx, painty, usew, painth, width());
}

void Gif::captureRoundSeekFrame() const {
	// Restart would flash cover thumbnail in place of shown frame.
	const auto streamed = activeRoundStreamed();
	if (!streamed) {
		return;
	}
	const auto size = roundThumbRect().size();
	auto request = ::Media::Streaming::FrameRequest{
		.outer = (ScaledInstantViewMediaSize(
			size,
			HostedInstantViewMediaPixelScale(_parent))
			* style::DevicePixelRatio()),
		.blurredBackground = true,
	};
	validateRoundingMask(request.outer);
	request.mask = _roundingMask;
	_seekLastFrame = streamed->frame(request);
}

void Gif::startRoundSeeking() {
	captureRoundSeekFrame();
	_seeking = true;
	_seekPressPoint = QPoint();
	_seekPreviewTime = 0;
	::Media::Player::instance()->startSeeking(AudioMsgId::Type::Voice);
	_roundSeek->setGrabbed(true);
}

void Gif::updateRoundSeeking(QRect rthumb, QPoint point) {
	const auto center = rthumb.center();
	const auto dx = float64(point.x() - center.x());
	const auto dy = float64(point.y() - center.y());
	const auto angle = atan2(-dy, dx);
	const auto now = std::clamp(
		fmod((M_PI / 2. - angle) / (2. * M_PI) + 1., 1.),
		0.,
		1.);
	const auto changed = (now != _roundSeek->progress());
	_roundSeek->setDraggedProgress(now);
	// Piled up restarts would cancel each other before showing a frame.
	if (changed && activeRoundStreamed()) {
		const auto ms = crl::now();
		if (ms - _seekPreviewTime >= kSeekPreviewInterval) {
			_seekPreviewTime = ms;
			captureRoundSeekFrame();
			::Media::Player::instance()->updateSeeking(
				AudioMsgId::Type::Voice,
				now);
		}
	}
	repaint();
}

void Gif::updatePressed(QPoint point) {
	if (!_seeking && _seekPressPoint == QPoint()) {
		return;
	}
	const auto rthumb = roundThumbRect();
	if (!_seeking) {
		if (_seekPressPoint == QPoint(-1, -1)) {
			_seekPressPoint = point;
			return;
		} else if ((point - _seekPressPoint).manhattanLength()
				<= QApplication::startDragDistance()) {
			return;
		} else if (!_roundSeek->grabPoint(rthumb, _seekPressPoint)) {
			// Angle from near the center jumps on the smallest move.
			_seekPressPoint = QPoint();
			return;
		}
		startRoundSeeking();
	}
	updateRoundSeeking(rthumb, point);
}

bool Gif::fullFeaturedGrouped(RectParts sides) const {
	return (sides & RectPart::Left) && (sides & RectPart::Right);
}

QSize Gif::sizeForGroupingOptimal(int maxWidth, bool last) const {
	return sizeForAspectRatio();
}

QSize Gif::sizeForGrouping(int width) const {
	return sizeForAspectRatio();
}

void Gif::drawGrouped(
		Painter &p,
		const PaintContext &context,
		const QRect &geometry,
		RectParts sides,
		Ui::BubbleRounding rounding,
		float64 highlightOpacity,
		not_null<uint64*> cacheKey,
		not_null<QPixmap*> cache) const {
	ensureDataMediaCreated();
	const auto item = _parent->data();
	const auto loaded = dataLoaded();
	const auto displayLoading = item->isSending()
		|| item->hasFailed()
		|| _data->displayLoading();
	const auto st = context.st;
	const auto sti = context.imageStyle();
	_smallGroupPart = !fullFeaturedGrouped(sides);
	const auto cornerDownload = downloadInCorner();

	const auto revealed = revealedProgress();
	const auto fullHiddenBySpoiler = (revealed == 0.);
	if (revealed < 1.) {
		validateSpoilerImageCache(geometry.size(), rounding);
	}

	const auto autoplay = autoplayEligible(!_smallGroupPart);
	const auto canStartPlay = autoplay
		&& !_streamed
		&& !fullHiddenBySpoiler;
	const auto shouldBePlaying = !autoplayUnderCursor()
		|| underCursor(!_smallGroupPart);
	if (!shouldBePlaying && _videoTimestamp != 0) {
		const_cast<Gif*>(this)->stopAnimation();
	} else if (canStartPlay) {
		const_cast<Gif*>(this)->playAnimation(true);
	} else {
		checkStreamedIsStarted();
	}

	const auto streamingMode = _streamed || autoplay;
	const auto activeOwnPlaying = activeOwnStreamed();

	const auto streamed = activeOwnPlaying
		? &activeOwnPlaying->instance
		: nullptr;
	const auto streamedForWaiting = _streamed
		? &_streamed->instance
		: nullptr;

	if (displayLoading
		&& (!streamedForWaiting
			|| item->isSending()
			|| _data->uploading()
			|| (cornerDownload && _data->loading()))) {
		ensureAnimation();
		if (!_animation->radial.animating()) {
			_animation->radial.start(dataProgress());
		}
	}
	updateStatusText();
	const auto radial = isRadialAnimation()
		|| (streamedForWaiting && streamedForWaiting->waitingShown());

	if (streamed && !fullHiddenBySpoiler) {
		const auto original = sizeForAspectRatio();
		const auto originalWidth = style::ConvertScale(original.width());
		const auto originalHeight = style::ConvertScale(original.height());
		const auto scaled = ScaledInstantViewMediaSize(
			geometry.size(),
			HostedInstantViewMediaPixelScale(_parent));
		const auto pixSize = Ui::GetImageScaleSizeForGeometry(
			{ originalWidth, originalHeight },
			{ scaled.width(), scaled.height() });
		const auto ratio = style::DevicePixelRatio();
		auto request = ::Media::Streaming::FrameRequest{
			.resize = pixSize * ratio,
			.outer = scaled * ratio,
			.rounding = MediaRoundingMask(rounding),
		};
		if (activeOwnPlaying->instance.playerLocked()) {
			if (activeOwnPlaying->frozenFrame.isNull()) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
				activeOwnPlaying->frozenStatusText = _statusText;
			} else if (activeOwnPlaying->frozenRequest != request) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
			}
			p.drawImage(geometry, activeOwnPlaying->frozenFrame);
		} else {
			if (activeOwnPlaying) {
				activeOwnPlaying->frozenFrame = QImage();
				activeOwnPlaying->frozenStatusText = QString();
			}
			p.drawImage(geometry, streamed->frame(request));
			const auto paused = context.paused
				|| (autoplayUnderCursor() && !underCursor(!_smallGroupPart));
			if (!paused) {
				streamed->markFrameShown();
			}
		}
	} else if (!fullHiddenBySpoiler) {
		validateGroupedCache(geometry, rounding, cacheKey, cache);
		p.drawPixmap(geometry, *cache);
	}

	if (revealed < 1.) {
		p.setOpacity(1. - revealed);
		p.drawImage(geometry, _spoiler->background);
		fillImageSpoiler(p, _spoiler.get(), geometry, context);
		p.setOpacity(1.);
	}

	const auto overlayOpacity = context.selected()
		? (1. - highlightOpacity)
		: highlightOpacity;
	if (overlayOpacity > 0.) {
		p.setOpacity(overlayOpacity);
		fillImageOverlay(p, geometry, rounding, context);
		if (!context.selected()) {
			fillImageOverlay(p, geometry, rounding, context);
		}
		p.setOpacity(1.);
	}

	// Force show standard icon for streamable videos that aren't playing/fully loaded,
	// even if the thumbnail is loaded (which sets loaded=true).
	// This ensures grid videos always show the download/play arrow.
	const auto forceShowForVideo = !_sensitiveSpoiler 
		&& _data->isVideoFile() 
		&& !_dataMedia->loaded(); // loaded() is true only if full file is loaded

	const auto paintInCenter = !_sensitiveSpoiler
		&& (radial
			|| (!streamingMode
				&& ((!loaded && !_data->loading()) || !autoplay))
			|| forceShowForVideo);
	if (paintInCenter) {
		const auto radialRevealed = 1.;
		const auto opacity = (item->isSending() || _data->uploading())
			? 1.
			: streamedForWaiting
			? streamedForWaiting->waitingOpacity()
			: (radial && loaded)
			? _animation->radial.opacity()
			: 1.;
		const auto radialOpacity = opacity * radialRevealed;
		const auto radialSize = st::historyGroupRadialSize;
		const auto inner = QRect(
			geometry.x() + (geometry.width() - radialSize) / 2,
			geometry.y() + (geometry.height() - radialSize) / 2,
			radialSize,
			radialSize);
		p.setPen(Qt::NoPen);
		if (context.selected()) {
			p.setBrush(st->msgDateImgBgSelected());
		} else if (isThumbAnimation()) {
			auto over = _animation->a_thumbOver.value(1.);
			p.setBrush(anim::brush(st->msgDateImgBg(), st->msgDateImgBgOver(), over));
		} else {
			auto over = ClickHandler::showAsActive(
				(_data->loading() || _data->uploading()) ? _cancell : _savel);
			p.setBrush(over ? st->msgDateImgBgOver() : st->msgDateImgBg());
		}
		p.setOpacity(radialOpacity * p.opacity());

		{
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(inner);
		}

		p.setOpacity(radialOpacity);
		const auto icon = [&]() -> const style::icon * {
			if (_data->waitingForAlbum()) {
				return &sti->historyFileThumbWaiting;
			}
			switch (currentAction(!_smallGroupPart)) {
			case Action::None:
			case Action::Streaming: return nullptr;
			case Action::Open: return &sti->historyFileThumbPlay;
			case Action::Cancel: return &sti->historyFileThumbCancel;
			case Action::Download: return &sti->historyFileThumbDownload;
			}
			Unexpected("Action in Gif::drawGrouped.");
		}();
		const auto previous = _data->waitingForAlbum()
			? &sti->historyFileThumbCancel
			: nullptr;
		if (icon) {
			if (previous && radialOpacity > 0. && radialOpacity < 1.) {
				PaintInterpolatedIcon(p, *icon, *previous, radialOpacity, inner);
			} else {
				icon->paintInCenter(p, inner);
			}
		}
		p.setOpacity(radialRevealed);
		if (radial) {
			const auto line = st::historyGroupRadialLine;
			const auto rinner = inner.marginsRemoved({ line, line, line, line });
			if (streamedForWaiting && !_data->uploading()) {
				Ui::InfiniteRadialAnimation::Draw(
					p,
					streamedForWaiting->waitingState(),
					rinner.topLeft(),
					rinner.size(),
					width(),
					sti->historyFileThumbRadialFg,
					st::msgFileRadialLine);
			} else if (!cornerDownload) {
				_animation->radial.draw(
					p,
					rinner,
					st::msgFileRadialLine,
					sti->historyFileThumbRadialFg);
			}
		}
		p.setOpacity(1.);
	}
	// Differentiation removed. All items (Single Row or Side-by-Side)
	// now use the unified paintInCenter logic above to draw the icon.
	if (cornerDownload) {
		drawCornerStatus(p, context, geometry.topLeft());
	}
	if (!fullHiddenBySpoiler && !_data->isVideoMessage()) {
		auto durMs = _data->duration();
		if (durMs <= 0 && streamed) {
			const auto played = streamed->player().prepareLegacyState().length;
			if (played != ::Media::kDurationUnavailable
				&& played > 0) {
				durMs = played;
			}
		}
		const auto durSeconds = std::max<qint64>(0, durMs / 1000);
		const auto sizeBytes = _data->size;
		if (sizeBytes > 0) {
			const auto font = st::msgDateFont;
			const auto sizeText = Ui::FormatSizeText(sizeBytes);
			const auto text = (durSeconds > 0)
				? (Ui::FormatDurationText(durSeconds)
					+ QChar(' ')
					+ QChar('(')
					+ sizeText
					+ QChar(')'))
				: sizeText;
			const auto textWidth = font->width(text);
			const auto textHeight = font->height;
			const auto hPadding = 2;
			const auto vPadding = st::msgDateImgPadding.y();
			const auto bubbleW = textWidth + 2 * hPadding;
			const auto bubbleH = textHeight + 2 * vPadding;
			if (bubbleW + 2 * st::msgDateImgDelta <= geometry.width()
				&& bubbleH + 2 * st::msgDateImgDelta <= geometry.height()) {
				const auto bubbleX = geometry.x()
					+ geometry.width()
					- bubbleW
					- st::msgDateImgDelta;
				const auto bubbleY = geometry.y()
					+ geometry.height()
					- bubbleH
					- st::msgDateImgDelta;
				p.save();
				p.setOpacity(0.95);
				Ui::FillRoundRect(
					p,
					bubbleX,
					bubbleY,
					bubbleW,
					bubbleH,
					sti->msgDateImgBg,
					sti->msgDateImgBgCorners);
				p.restore();

				p.setPen(st->msgDateImgFg());
				p.setFont(font->bold());
				const auto baseY = bubbleY
					+ (bubbleH - textHeight) / 2
					+ font->ascent;
				p.drawText(bubbleX + hPadding, baseY, text);
			}
		}
	}
}

TextState Gif::getStateGrouped(
		const QRect &geometry,
		RectParts sides,
		QPoint point,
		StateRequest request) const {
	if (!geometry.contains(point)) {
		return {};
	}
	const auto fullFeatured = fullFeaturedGrouped(sides);
	if (fullFeatured) {
		const auto state = cornerStatusTextState(
			point,
			request,
			geometry.topLeft());
		if (state.link) {
			return state;
		}
	}
	ensureDataMediaCreated();

	auto link = (_spoiler && !_spoiler->revealed)
		? (_sensitiveSpoiler ? spoilerTagLink() : _spoiler->link)
		: currentVideoLink(fullFeatured);
	return TextState(_parent, std::move(link));
}

Gif::Action Gif::currentAction(bool fullFeatured) const {
	ensureDataMediaCreated();
	if (_data->waitingForAlbum()) {
		return Action::None;
	} else if (_data->uploading()) {
		return Action::Cancel;
	} else if (_realParent->isSending()) {
		return Action::None;
	} else if (_streamed
		|| activeRoundStreamed()
		|| autoplayEligible(fullFeatured)) {
		return Action::Streaming;
	}
	const auto cornerDownload = fullFeatured && downloadInCorner();
	if ((dataLoaded() || _dataMedia->canBePlayed())
		&& (!_data->displayLoading() || cornerDownload)) {
		return Action::Open;
	} else if (_data->loading()) {
		return Action::Cancel;
	}
	return Action::Download;
}

ClickHandlerPtr Gif::currentVideoLink(bool fullFeatured) const {
	switch (currentAction(fullFeatured)) {
	case Action::None: return nullptr;
	case Action::Open:
	case Action::Streaming: return _openl;
	case Action::Cancel: return _cancell;
	case Action::Download: return _savel;
	}
	Unexpected("Action in Gif::currentVideoLink.");
}

void Gif::ensureDataMediaCreated() const {
	if (_dataMedia && (!_videoCover || _videoCoverMedia)) {
		return;
	}
	_dataMedia = _data->createMediaView();
	_videoCoverMedia = _videoCover
		? _videoCover->createMediaView()
		: nullptr;
	dataMediaCreated();
}

void Gif::dataMediaCreated() const {
	Expects(_dataMedia != nullptr);

	if (_videoCoverMedia) {
		_videoCoverMedia->wanted(
			Data::PhotoSize::Large,
			_realParent->fullId());
	} else {
		_dataMedia->goodThumbnailWanted();
		_dataMedia->thumbnailWanted(_realParent->fullId());
		if (!autoplayEnabled()) {
			_dataMedia->videoThumbnailWanted(_realParent->fullId());
		}
	}
	history()->owner().registerHeavyViewPart(_parent);
	togglePollingStory(true);
}

void Gif::togglePollingStory(bool enabled) const {
	if (!_storyId || _pollingStory == enabled) {
		return;
	}
	const auto polling = Data::Stories::Polling::Chat;
	if (!enabled) {
		_data->owner().stories().unregisterPolling(_storyId, polling);
	} else if (
			!_data->owner().stories().registerPolling(_storyId, polling)) {
		return;
	}
	_pollingStory = enabled;
}

bool Gif::uploading() const {
	return _data->uploading();
}

void Gif::hideSpoilers() {
	if (_spoiler) {
		_spoiler->revealed = false;
	}
}

bool Gif::needsBubble() const {
	if (_storyId) {
		return true;
	} else if (_data->isVideoMessage()) {
		return false;
	}
	const auto item = _parent->data();
	return item->repliesAreComments()
		|| item->externalReply()
		|| item->viaBot()
		|| !item->emptyText()
		|| _parent->displayReply()
		|| _parent->displayForwardedFrom()
		|| _parent->displayFromName()
		|| _parent->displayedTopicButton();
}

//bool Gif::needInfoDisplay() const {
//	return _data->isVideoMessage();
//}

bool Gif::unwrapped() const {
	return isUnwrapped();
}

QRect Gif::contentRectForReactions() const {
	if (!isUnwrapped()) {
		return QRect(0, 0, width(), height());
	}
	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	auto usex = 0, usew = paintw;
	const auto rightAligned = _parent->hasRightLayout();
	const auto item = _parent->data();
	const auto via = item->Get<HistoryMessageVia>();
	const auto reply = _parent->Get<Reply>();
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	if (via || reply || forwarded || !_ephemeral.text.isEmpty()) {
		usew = maxWidth() - additionalWidth(reply, via, forwarded);
	}
	accumulate_max(usew, _parent->reactionsOptimalWidth());
	if (rightAligned) {
		usex = width() - usew;
	}
	if (rtl()) usex = width() - usex - usew;
	return style::rtlrect(usex + paintx, painty, usew, painth, width());
}

std::optional<int> Gif::reactionButtonCenterOverride() const {
	if (!isUnwrapped() || _parent->hidesBottomInfo()) {
		return std::nullopt;
	}
	const auto right = resolveCustomInfoRightBottom().x()
		- _parent->infoWidth()
		- 3 * st::msgDateImgPadding.x();
	return right - st::reactionCornerSize.width() / 2;
}

QPoint Gif::resolveCustomInfoRightBottom() const {
	const auto inner = contentRectForReactions();
	auto fullBottom = inner.y() + inner.height();
	auto fullRight = inner.x() + inner.width();
	if (_parent->hidesBottomInfo()) {
		return QPoint(fullRight, fullBottom);
	}
	const auto unwrapped = isUnwrapped();
	if (unwrapped) {
		auto maxRight = _parent->width() - st::msgMargin.left();
		if (_parent->hasFromPhoto()) {
			maxRight -= st::msgMargin.right();
		} else {
			maxRight -= st::msgMargin.left();
		}
		const auto infoWidth = _parent->infoWidth();
		const auto rightAligned = _parent->hasRightLayout();
		if (!rightAligned) {
			// This is just some arbitrary point,
			// the main idea is to make info left aligned here.
			fullRight += infoWidth - st::normalFont->height;
			if (fullRight > maxRight) {
				fullRight = maxRight;
			}
		}
	}
	const auto skipx = unwrapped
		? st::msgDateImgPadding.x()
		: (st::msgDateImgDelta + st::msgDateImgPadding.x());
	const auto skipy = unwrapped
		? st::msgDateImgPadding.y()
		: (st::msgDateImgDelta + st::msgDateImgPadding.y());
	return QPoint(fullRight - skipx, fullBottom - skipy);
}

int Gif::additionalWidth() const {
	const auto item = _parent->data();
	return additionalWidth(
		_parent->Get<Reply>(),
		item->Get<HistoryMessageVia>(),
		item->Get<HistoryMessageForwarded>());
}

bool Gif::isUnwrapped() const {
	return _data->isVideoMessage() && (_parent->media() == this);
}

void Gif::validateGroupedCache(
		const QRect &geometry,
		Ui::BubbleRounding rounding,
		not_null<uint64*> cacheKey,
		not_null<QPixmap*> cache) const {
	using Option = Images::Option;

	ensureDataMediaCreated();

	const auto good = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Large)
		: _dataMedia->goodThumbnail();
	const auto thumb = _videoCoverMedia
		? nullptr
		: _dataMedia->thumbnail();
	const auto image = good
		? good
		: thumb
		? thumb
		: _videoCoverMedia
		? _videoCoverMedia->thumbnailInline()
		: _dataMedia->thumbnailInline();
	const auto blur = !good
		&& (!thumb
			|| (thumb->width() < kUseNonBlurredThreshold
				&& thumb->height() < kUseNonBlurredThreshold));

	const auto loadLevel = good ? 3 : thumb ? 2 : image ? 1 : 0;
	const auto scaled = ScaledInstantViewMediaSize(
		geometry.size(),
		HostedInstantViewMediaPixelScale(_parent));
	const auto width = scaled.width();
	const auto height = scaled.height();
	const auto options = (blur ? Option::Blur : Option(0));
	const auto key = (uint64(width) << 48)
		| (uint64(height) << 32)
		| (uint64(options) << 16)
		| (uint64(rounding.key()) << 8)
		| (uint64(loadLevel));
	if (*cacheKey == key) {
		return;
	}

	if (!image && !_data->hasThumbnail() && _data->isVideoFile()) {
		if (GetEnhancedBool("custom_file_thumbs")) {
			auto thumb = QImage();
			const auto path = GetEnhancedString("custom_thumb_path");
			if (!path.isEmpty()) {
				thumb = QImage(path);
			}
			if (!thumb.isNull()) {
				*cacheKey = key;
				const auto pixSize = thumb.size()
					.scaled(geometry.size(), Qt::KeepAspectRatio);
				const auto ratio = style::DevicePixelRatio();
				auto scaled = Images::Prepare(
					thumb,
					pixSize * ratio,
					{ .options = options, .outer = geometry.size() });
				auto rounded = Images::Round(
					std::move(scaled),
					MediaRoundingMask(rounding));
				*cache = Ui::PixmapFromImage(std::move(rounded));
				return;
			}
		}
	}

	const auto imageSize = image ? image->size() : sizeForAspectRatio();
	const auto originalWidth = style::ConvertScale(imageSize.width());
	const auto originalHeight = style::ConvertScale(imageSize.height());
	const auto pixSize = Ui::GetImageScaleSizeForGeometry(
		{ originalWidth, originalHeight },
		{ width, height });
	const auto ratio = style::DevicePixelRatio();

	*cacheKey = key;
	auto prepared = Images::Prepare(
		(image ? image : Image::BlankMedia().get())->original(),
		pixSize * ratio,
		{ .options = options, .outer = { width, height } });
	auto rounded = Images::Round(
		std::move(prepared),
		MediaRoundingMask(rounding));
	*cache = Ui::PixmapFromImage(std::move(rounded));
}

void Gif::setStatusSize(int64 newSize) const {
	if (newSize < 0) {
		_statusSize = newSize;
		_statusText = Ui::FormatDurationText(-newSize - 1);
	} else if (_data->isVideoMessage()) {
		_statusSize = newSize;
		_statusText = Ui::FormatDurationText(_data->duration() / 1000);
	} else {
		File::setStatusSize(
			newSize,
			_data->size,
			_data->isVideoFile() ? (_data->duration() / 1000) : -2,
			0);
	}
}

void Gif::updateStatusText() const {
	ensureDataMediaCreated();
	auto statusSize = int64();
	if (_data->status == FileDownloadFailed || _data->status == FileUploadFailed) {
		statusSize = Ui::FileStatusSizeFailed;
	} else if (_data->uploading()) {
		statusSize = _data->uploadingData->offset;
	} else if (!downloadInCorner() && _data->loading()) {
		statusSize = _data->loadOffset();
	} else if (dataLoaded() || _dataMedia->canBePlayed()) {
		statusSize = Ui::FileStatusSizeLoaded;
	} else {
		statusSize = Ui::FileStatusSizeReady;
	}
	const auto round = activeRoundStreamed();
	const auto own = activeOwnStreamed();
	if (round || (own && _data->isVideoFile())) {
		const auto frozen = own && !own->frozenFrame.isNull();
		const auto streamed = round ? round : &own->instance;
		const auto state = streamed->player().prepareLegacyState();
		if (state.length) {
			auto position = int64(0);
			if (::Media::Player::IsStoppedAtEnd(state.state)) {
				position = state.length;
			} else if (!::Media::Player::IsStoppedOrStopping(state.state)) {
				position = state.position;
			}
			if (!frozen) {
				statusSize = -1 - int((state.length - position) / state.frequency + 1);
			}
			_videoPosition = std::max(
				crl::time(position * crl::time(1000) / state.frequency),
				crl::time(1));
		} else {
			if (!frozen) {
				statusSize = -1 - (_data->duration() / 1000);
			}
			_videoPosition = 0;
		}
	}
	if (statusSize != _statusSize) {
		setStatusSize(statusSize);
	}
	if (_data->uploading() && _data->uploadingData->preparing) {
		const auto percent = int(base::SafeRound(
			_data->uploadingData->prepareProgress * 100));
		_statusText = tr::lng_send_video_preparing(
			tr::now,
			lt_progress,
			QString::number(percent));
		_statusSize = Ui::FileStatusSizeReady;
	}
}

QString Gif::additionalInfoString() const {
	if (_data->isVideoMessage()) {
		updateStatusText();
		return _statusText;
	}
	return QString();
}

bool Gif::isReadyForOpen() const {
	return true;
}

bool Gif::hasHeavyPart() const {
	return (_spoiler && _spoiler->animation) || _streamed || _dataMedia;
}

void Gif::unloadHeavyPart() {
	stopAnimation();
	_dataMedia = nullptr;
	if (_spoiler) {
		_spoiler->background = _spoiler->cornerCache = QImage();
		_spoiler->animation = nullptr;
	}
	_thumbCache = QImage();
	_seekLastFrame = QImage();
	if (_roundSeek) {
		_roundSeek->unloadHeavyPart();
	}
	_videoThumbnailFrame = nullptr;
	togglePollingStory(false);
}

bool Gif::enforceBubbleWidth() const {
	return true;
}

int Gif::bubbleWidthLimit() const {
	if (_ephemeral.text.isEmpty()
		|| !_data->isVideoMessage()
		|| !isUnwrapped()) {
		return 0;
	}
	const auto item = _parent->data();
	const auto via = item->Get<HistoryMessageVia>();
	const auto reply = _parent->Get<Reply>();
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	const auto content = maxWidth() - additionalWidth(reply, via, forwarded);
	return content
		+ st::msgReplyPadding.left()
		+ EphemeralPlateMaxWidth(_ephemeral.text);
}

int Gif::additionalWidth(
		const Reply *reply,
		const HistoryMessageVia *via,
		const HistoryMessageForwarded *forwarded) const {
	int result = 0;
	if (forwarded) {
		accumulate_max(result, st::msgReplyPadding.left() + st::msgReplyPadding.left() + forwarded->text.maxWidth() + st::msgReplyPadding.right());
	} else if (via) {
		accumulate_max(result, st::msgReplyPadding.left() + st::msgReplyPadding.left() + via->maxWidth + st::msgReplyPadding.left());
	}
	if (reply) {
		accumulate_max(result, st::msgReplyPadding.left() + reply->maxWidth());
	}
	if (!_ephemeral.text.isEmpty()) {
		accumulate_max(
			result,
			st::msgReplyPadding.left()
				+ EphemeralPlateMaxWidth(_ephemeral.text));
	}
	return result;
}

int Gif::surroundingHeight(
		const Reply *reply,
		const HistoryMessageVia *via,
		const HistoryMessageForwarded *forwarded,
		int rectw) const {
	const auto innerw = rectw
		- (st::msgReplyPadding.left() + st::msgReplyPadding.right());
	auto recth = 0;
	const auto forwardedHeightReal = forwarded
		? forwarded->text.countHeight(innerw)
		: 0;
	const auto forwardedHeight = qMin(
		forwardedHeightReal,
		kMaxGifForwardedBarLines * st::msgServiceNameFont->height);
	if (forwarded) {
		recth += st::msgReplyPadding.top() + forwardedHeight;
	} else if (via) {
		recth += st::msgReplyPadding.top()
			+ st::msgServiceNameFont->height
			+ (reply ? st::msgReplyPadding.top() : 0);
	}
	if (reply) {
		const auto replyMargins = reply->margins();
		recth += reply->height()
			- ((forwarded || via) ? 0 : replyMargins.top())
			- replyMargins.bottom();
	} else {
		recth += st::msgReplyPadding.bottom();
	}
	return recth;
}

::Media::Streaming::Instance *Gif::activeRoundStreamed() const {
	return ::Media::Player::instance()->roundVideoStreamed(_parent->data());
}

bool Gif::roundSeekShown() const {
	if (!_seekl) {
		return false;
	} else if (_seeking) {
		return true;
	}
	const auto streamed = activeRoundStreamed();
	return streamed && streamed->paused();
}

bool Gif::isRoundSeekable() const {
	// Player goes not-ready while a seek is applied.
	if (!activeRoundStreamed() && !_seeking) {
		return false;
	}
	const auto state = ::Media::Player::instance()->getState(
		AudioMsgId::Type::Voice);
	return (state.id == AudioMsgId(
			_data,
			_realParent->fullId(),
			state.id.externalPlayId()))
		&& !::Media::Player::IsStoppedOrStopping(state.state);
}

Gif::Streamed *Gif::activeOwnStreamed() const {
	return (_streamed
		&& _streamed->instance.player().ready()
		&& !_streamed->instance.player().videoSize().isEmpty())
		? _streamed.get()
		: nullptr;
}

::Media::Streaming::Instance *Gif::activeCurrentStreamed() const {
	if (const auto streamed = activeRoundStreamed()) {
		return streamed;
	} else if (const auto owned = activeOwnStreamed()) {
		return &owned->instance;
	}
	return nullptr;
}

::Media::View::PlaybackProgress *Gif::videoPlayback() const {
	return ::Media::Player::instance()->roundVideoPlayback(_parent->data());
}

void Gif::playAnimation(bool autoplay) {
	ensureDataMediaCreated();
	if (_data->isVideoMessage() && !autoplay) {
		return;
	} else if (_streamed && autoplay) {
		return;
	} else if ((_streamed && autoplayEnabled())
		|| (!autoplay && _data->isVideoFile())) {
		_parent->delegate()->elementOpenDocument(
			_data,
			_parent->data()->fullId(),
			true);
		return;
	}
	if (_streamed) {
		stopAnimation();
	} else if (_dataMedia->canBePlayed()) {
		if (!autoplayEnabled()) {
			history()->owner().checkPlayingAnimations();
		}
		createStreamedPlayer();
	}
}

void Gif::createStreamedPlayer() {
	const auto quality = _data->initialPlaybackVideoQuality(
		Core::App().settings().videoQuality());
	const auto chosen = _data->chooseQuality(_realParent, quality);
	if (_streamed && _streamed->chosen == chosen) {
		return;
	}
	auto shared = _data->owner().streaming().sharedDocument(
		chosen,
		_data,
		_realParent,
		_realParent->fullId());
	if (!shared) {
		return;
	}
	setStreamed(std::make_unique<Streamed>(
		chosen,
		std::move(shared),
		[=] { repaintStreamedContent(); }));

	_streamed->instance.player().updates(
	) | rpl::on_next_error([=](::Media::Streaming::Update &&update) {
		handleStreamingUpdate(std::move(update));
	}, [=](::Media::Streaming::Error &&error) {
		handleStreamingError(std::move(error));
	}, _streamed->instance.lifetime());

	_streamed->instance.switchQualityRequests(
	) | rpl::on_next([=](int requested) {
		if (quality.manual) {
			return;
		}
		auto now = Core::App().settings().videoQuality();
		if (now.manual || now.height == requested) {
			return;
		}
		Core::App().settings().setVideoQuality({
			.manual = 0,
			.height = uint32(requested),
		});
		Core::App().saveSettingsDelayed();
		createStreamedPlayer();
	}, _streamed->instance.lifetime());

	if (_streamed->instance.ready()) {
		streamingReady(base::duplicate(_streamed->instance.info()));
	}
	checkStreamedIsStarted();
}

void Gif::startStreamedPlayer() const {
	Expects(_streamed != nullptr);

	auto options = ::Media::Streaming::PlaybackOptions();
	options.audioId = AudioMsgId(_data, _realParent->fullId());
	options.waitForMarkAsShown = true;
	//if (!_streamed->withSound) {
	options.mode = ::Media::Streaming::Mode::Video;
	options.loop = true;
	options.position = _videoTimestamp
		? (_videoTimestamp * crl::time(1000))
		: _parent->history()->session().local().mediaLastPlaybackPosition(
			_data->id);
	//}
	_streamed->instance.play(options);
}

void Gif::checkStreamedIsStarted() const {
	if (!_streamed || _streamed->instance.playerLocked()) {
		return;
	}
	if (_streamed->instance.active()) {
		if (_streamed->instance.paused()) {
			_streamed->instance.resume();
		}
	} else if (!_streamed->instance.failed()) {
		startStreamedPlayer();
	}
}

void Gif::setStreamed(std::unique_ptr<Streamed> value) {
	const auto removed = (_streamed && !value);
	const auto set = (!_streamed && value);
	_streamed = std::move(value);
	if (set) {
		history()->owner().registerHeavyViewPart(_parent);
		togglePollingStory(true);
	} else if (removed) {
		_videoPosition = 0;
		_parent->checkHeavyPart();
	}
}

void Gif::handleStreamingUpdate(::Media::Streaming::Update &&update) {
	using namespace ::Media::Streaming;

	v::match(update.data, [&](Information &update) {
		streamingReady(std::move(update));
	}, [](PreloadedVideo) {
	}, [&](UpdateVideo) {
		repaintStreamedContent();
	}, [](PreloadedAudio) {
	}, [](UpdateAudio) {
	}, [](WaitingForData) {
	}, [](SpeedEstimate) {
	}, [](MutedByOther) {
	}, [](Finished) {
	});
}

void Gif::handleStreamingError(::Media::Streaming::Error &&error) {
}

void Gif::repaintStreamedContent() {
	const auto own = activeOwnStreamed();
	if (own && !own->frozenFrame.isNull()) {
		return;
	} else if (_parent->delegate()->elementAnimationsPaused()
		&& !activeRoundStreamed()) {
		return;
	}
	repaint();
}

void Gif::streamingReady(::Media::Streaming::Information &&info) {
	if (!ValidFrameSize(info.video.size, maxInlineArea())) {
		if (!info.video.size.isEmpty()) {
			_data->dimensions = info.video.size;
		}
		stopAnimation();
	} else {
		history()->owner().requestViewResize(_parent);
	}
}

void Gif::stopAnimation() {
	if (_streamed) {
		setStreamed(nullptr);
		history()->owner().requestViewResize(_parent);
	}
}

void Gif::checkAnimation() {
	if (_streamed && !autoplayEnabled()) {
		stopAnimation();
	}
}

float64 Gif::dataProgress() const {
	ensureDataMediaCreated();
	return (_data->uploading()
		|| (!_parent->data()->isSending() && !_parent->data()->hasFailed()))
		? _dataMedia->progress()
		: 0;
}

bool Gif::dataFinished() const {
	return (!_parent->data()->isSending() && !_parent->data()->hasFailed())
		? (!_data->loading() && !_data->uploading())
		: false;
}

bool Gif::dataLoaded() const {
	ensureDataMediaCreated();
	return !_parent->data()->isSending()
		&& !_parent->data()->hasFailed()
		&& _dataMedia->loaded();
}

//bool Gif::needInfoDisplay() const {
//	const auto item = _parent->data();
//	if (item->isFakeAboutView()) {
//		return false;
//	}
//	return item->isSending()
//		|| item->awaitingVideoProcessing()
//		|| _data->uploading()
//		|| _parent->isUnderCursor()
//		|| (_parent->delegate()->elementContext() == Context::ChatPreview)
//		// Don't show the GIF badge if this message has text.
//		|| (!_parent->hasBubble() && _parent->isLastAndSelfMessage());
//}

//bool Gif::needCornerStatusDisplay() const {
//	return _data->isVideoFile()
//		|| needInfoDisplay();
//}

bool Gif::needCornerStatusDisplay() const {
	return _data->isVideoFile();
}

void Gif::ensureTranscribeButton() const {
	const auto media = _parent->data()->media();
	if (_data->isVideoMessage()
		&& (!media || !media->ttlSeconds())
		&& !_parent->data()->isScheduled()
		&& !_parent->data()->isAdminLogEntry()
		&& (_data->session().premium()
			|| _data->session().api().transcribes().trialsSupport())) {
		if (!_transcribe) {
			_transcribe = std::make_unique<TranscribeButton>(
				_realParent,
				true);
		}
	} else {
		_transcribe = nullptr;
	}
}

} // namespace HistoryView
