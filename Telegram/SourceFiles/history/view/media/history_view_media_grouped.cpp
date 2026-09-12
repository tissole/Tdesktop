/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_media_grouped.h"

#include <set>
#include "history/history_item_components.h"
#include "history/history_item.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_message.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "storage/storage_shared_media.h"
#include "lang/lang_keys.h"
#include "media/streaming/media_streaming_utility.h"
#include "ui/grouped_layout.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/message_bubble.h"
#include "ui/text/text_options.h"
#include "ui/text/format_values.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/item_text_options.h"
#include "history/history_item_helpers.h"
#include "base/unixtime.h"
#include "layout/layout_selection.h"
#include "styles/style_chat.h"
#include "styles/style_basic.h"
#include "core/enhanced_settings.h"
#include "core/ui_integration.h"
#include "data/data_photo.h"

namespace HistoryView {
namespace {

[[nodiscard]] bool IsHostedInstantViewMedia(not_null<const Element*> parent) {
	return parent->Get<InstantViewMediaRuntime>() != nullptr;
}

std::vector<Ui::GroupMediaLayout> LayoutPlaylist(
		const std::vector<QSize> &sizes) {
	Expects(!sizes.empty());

	auto result = std::vector<Ui::GroupMediaLayout>();
	result.reserve(sizes.size());
	const auto width = ranges::max_element(
			sizes,
			std::less<>(),
			&QSize::width)->width();
	auto top = 0;
	const auto skip = st::historyGroupSkip;
	for (auto i = 0, count = int(sizes.size()); i != count; ++i) {
		result.push_back({
			.geometry = QRect(0, top, width, sizes[i].height()),
			.sides = RectPart::Left | RectPart::Right
		});
		top += sizes[i].height();
		if (i + 1 != count) {
			top += skip;
		}
	}
	result.front().sides |= RectPart::Top;
	result.back().sides |= RectPart::Bottom;
	return result;
}

} // namespace

GroupedMedia::Part::Part(
	not_null<Element*> parent,
	not_null<Data::Media*> media)
: item(media->parent())
, content(media->createView(parent, item)) {
	Assert(media->canBeGrouped());
}

void GroupedMedia::drawMessageIdInfo(
		Painter &p,
		const PaintContext &context,
		const QRect &itemGeometry,
		not_null<HistoryItem*> item) const {
	QString infoText;
	const auto edited = item->Get<HistoryMessageEdited>();
	if (edited && !item->hideEditedBadge()) {
		infoText += QString::fromUtf8("✏️");
	}

	if (GetEnhancedBool("show_messages_id")) {
		const auto msgId = item->fullId().msg;
		if (msgId > 0) {
			if (!infoText.isEmpty()) {
				infoText += ' ';
				infoText += QString("(%1)").arg(msgId.bare);
			} else {
				infoText += QString::number(msgId.bare);
			}
		}
	}

	if (infoText.isEmpty()) {
		return;
	}

	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto font = st::msgDateFont;
	p.setFont(font);

	auto textWidth = font->width(infoText);
	const auto textHeight = font->height;
	
	const auto hPadding = 2;
	const auto vPadding = st::msgDateImgPadding.y();

	auto dateW = textWidth + (2 * hPadding);
	const auto dateH = textHeight + (2 * vPadding);

	if (dateW > itemGeometry.width()) {
		const auto availableWidth = itemGeometry.width()
			- (2 * hPadding);
		if (availableWidth > font->width("...")) {
			const QFontMetrics metrics(font);
			infoText = metrics.elidedText(
				infoText,
				Qt::ElideRight,
				availableWidth);
			textWidth = font->width(infoText);
			dateW = textWidth + (2 * hPadding);
		} else {
			return;
		}
	}

	const auto bubbleY = itemGeometry.y() + st::msgDateImgDelta;
	const auto bubbleX = itemGeometry.x() + itemGeometry.width() - dateW - st::msgDateImgDelta;

	// Draw with uniform style brush to avoid edge transparency differences
	p.save();
	p.setOpacity(0.95);
	Ui::FillRoundRect(
		p,
		bubbleX,
		bubbleY,
		dateW,
		dateH,
		sti->msgDateImgBg,
		sti->msgDateImgBgCorners);
	p.restore();

	// Use proper white color for text on image bubbles
	p.setPen(st->msgDateImgFg());
	p.setFont(font->bold());
	p.drawText(
		bubbleX + hPadding,
		bubbleY + vPadding + font->ascent,
		infoText);
}

GroupedMedia::GroupedMedia(
	not_null<Element*> parent,
	const std::vector<std::unique_ptr<Data::Media>> &medias) 
: Media(parent) {
	const auto truncated = ranges::views::all(
			medias
		) | ranges::views::transform([](const std::unique_ptr<Data::Media> &v) {
			return v.get();
		}) | ranges::views::take(kMaxSize);
	const auto result = applyGroup(truncated);

	Ensures(result);
}

GroupedMedia::GroupedMedia(
	not_null<Element*> parent,
	const std::vector<not_null<HistoryItem*>> &items) 
: Media(parent) {
	const auto medias = ranges::views::all(
			items
		) | ranges::views::transform([](not_null<HistoryItem*> item) {
			return item->media();
		}) | ranges::views::take(kMaxSize);
	const auto result = applyGroup(medias);

	Ensures(result);
}

GroupedMedia::~GroupedMedia() {
	// Destroy all parts while the media object is still not destroyed.
	base::take(_parts);
}

HistoryItem *GroupedMedia::itemForText() const {
	if (_mode == Mode::Column) {
		return Media::itemForText();
	} else if (!_captionItem) {
		_captionItem = [&]() -> HistoryItem* {
			auto result = (HistoryItem*)nullptr;
			auto count = 0;
			for (const auto &part : _parts) {
				if (!part.item->originalText().empty()) {
					if (result && result != part.item) {
						// Multiple items have text
						count = 2; 
						break;
					}
					result = part.item;
					count = 1;
				}
			}
			if (_mode == Mode::Grid) {
				return (count == 1) ? result : nullptr;
			}
			return result;
		}();
	}
	return *_captionItem;
}

bool GroupedMedia::hideMessageText() const {
	return (_mode == Mode::Column);
}

GroupedMedia::Mode GroupedMedia::DetectMode(not_null<Data::Media*> media) {
	const auto document = media->document();
	return (document
		&& !document->isVideoFile()
		&& !document->isAnimation())
		? Mode::Column
		: Mode::Grid;
}

QSize GroupedMedia::countOptimalSize() {
	_purchasedPriceTag = hasPurchasedTag();

	std::vector<QSize> sizes;
	const auto partsCount = _parts.size();
	sizes.reserve(partsCount);
	auto maxWidth = 0;
	if (_mode == Mode::Column) {
		for (const auto &part : _parts) {
			const auto &media = part.content;
			media->setBubbleRounding(bubbleRounding());
			media->initDimensions();
			accumulate_max(maxWidth, media->maxWidth());
		}
		auto index = 0;
		for (const auto &part : _parts) {
			const auto last = (++index == _parts.size());
			accumulate_max(
				maxWidth,
				part.content->widenGroupingMaxWidth(maxWidth, last));
		}
	}
	auto index = 0;
	for (const auto &part : _parts) {
		++index;
		const auto last = (_mode == Mode::Column)
			? (index == _parts.size())
			: (index == _parts.size());
		
		auto size = part.content->sizeForGroupingOptimal(maxWidth, last);
		if (_mode == Mode::Column && last) {
			// No extra bottom padding here, handled by spacing logic below
		}
		sizes.push_back(size);
	}

	const auto layout = (_mode == Mode::Grid)
		? Ui::LayoutMediaGroup(
			sizes,
			st::historyGroupWidthMax,
			st::historyGroupWidthMin,
			st::historyGroupSkip)
		: LayoutPlaylist(sizes);
	Assert(layout.size() == _parts.size());

	auto minHeight = 0;

	if (_mode == Mode::Column) {
		auto top = 0;
		for (auto i = 0, count = int(layout.size()); i != count; ++i) {
			const auto &item = layout[i];
			accumulate_max(maxWidth, item.geometry.x() + item.geometry.width());

			auto partHeight = sizes[i].height();

			_parts[i].initialGeometry = QRect(0, top, item.geometry.width(), partHeight);
			_parts[i].sides = item.sides;

			top += partHeight;
		}
		minHeight = top;
	} else {
		// Grid Mode
		for (auto i = 0, count = int(layout.size()); i != count; ++i) {
			const auto &item = layout[i];
			accumulate_max(maxWidth, item.geometry.x() + item.geometry.width());
			accumulate_max(minHeight, item.geometry.y() + item.geometry.height());
			_parts[i].initialGeometry = item.geometry;
			_parts[i].sides = item.sides;
		}

		std::map<int, std::vector<int>> rows;
		for (auto i = 0; i != _parts.size(); ++i) {
			rows[_parts[i].initialGeometry.y()].push_back(i);
		}

		const auto textHeight = st::messageTextStyle.font->height;
		const auto uniformCaptionHeight = 4 + textHeight + 5; // 4px top + text height + 5px bottom

		int captionsCount = 0;
		for (const auto &part : _parts) {
			if (!part.item->originalText().empty()) {
				captionsCount++;
			}
		}
		const bool usePerItemCaptions = (captionsCount > 1);

		int totalShift = 0;
		for (auto const& [rowY, indices] : rows) {
			bool rowHasCaption = false;
			if (usePerItemCaptions) {
				for (const auto i : indices) {
					if (!_parts[i].item->originalText().empty()) {
						rowHasCaption = true;
						break;
					}
				}
			}

			if (rowHasCaption) {
				const auto spacing = st::historyGroupSkip;
				const auto isLastRow = (rowY == rows.rbegin()->first);
				for (const auto i : indices) {
					auto &part = _parts[i];
					if (!part.item->originalText().empty()) {
						part._captionHeight = uniformCaptionHeight;
						part._captionText = Ui::Text::String(
							st::messageTextStyle,
							part.item->originalText(),
							Ui::ItemTextDefaultOptions(),
							st::msgMinWidth,
							Core::TextContext({
								.session = &_parent->history()->session(),
								.repaint = [=] { _parent->customEmojiRepaint(); },
							}));
					} else {
						part._captionHeight = 0;
					}
				}
				totalShift += isLastRow ? uniformCaptionHeight : (uniformCaptionHeight - spacing);
			} else {
				for (const auto i : indices) {
					_parts[i]._captionHeight = 0;
				}
			}
		}
		minHeight += totalShift;
	}

	const auto groupPadding = groupedPadding();
	minHeight += groupPadding.top() + groupPadding.bottom();

	return { maxWidth, int(base::SafeRound(minHeight)) };
}

QSize GroupedMedia::countCurrentSize(int newWidth) {
	const auto hostedInstantView = (_mode == Mode::Grid)
		&& IsHostedInstantViewMedia(_parent);
	if (!hostedInstantView) {
		accumulate_min(newWidth, maxWidth());
	}
	auto newHeight = 0;

	if (_mode == Mode::Grid && newWidth < st::historyGroupWidthMin) {
		return { newWidth, newHeight };
	} else if (_mode == Mode::Column) {
		auto top = 0;
		const auto skip = st::historyGroupSkip;
		for (auto i = 0; i != _parts.size(); ++i) {
			auto &part = _parts[i];
			auto size = part.content->sizeForGrouping(newWidth);

			part.geometry = QRect(0, top, newWidth, size.height());
			top += size.height();
			if (i + 1 != _parts.size()) {
				top += skip;
			}
		}
		newHeight = top;
	} else {
		// Grid Resize Logic
		const auto initialSpacing = st::historyGroupSkip;
		const auto factor = newWidth / float64(maxWidth());
		const auto scale = [&](int value) {
			return int(base::SafeRound(value * factor));
		};
		const auto spacing = scale(initialSpacing);

		for (auto &part : _parts) {
			const auto sides = part.sides;
			const auto initialGeometry = part.initialGeometry;
			const auto needRightSkip = !(sides & RectPart::Right);
			const auto needBottomSkip = !(sides & RectPart::Bottom);
			const auto initialLeft = initialGeometry.x();
			const auto initialTop = initialGeometry.y();
			const auto initialRight = initialLeft
				+ initialGeometry.width()
				+ (needRightSkip ? initialSpacing : 0);
			const auto initialBottom = initialTop
				+ initialGeometry.height()
				+ (needBottomSkip ? initialSpacing : 0);
			const auto left = scale(initialLeft);
			const auto top = scale(initialTop);
			const auto width = scale(initialRight)
				- left
				- (needRightSkip ? spacing : 0);
			const auto height = scale(initialBottom)
				- top
				- (needBottomSkip ? spacing : 0);
			part.geometry = QRect(left, top, width, height);
		}

		std::map<int, std::vector<int>> rows;
		for (auto i = 0; i != _parts.size(); ++i) {
			rows[_parts[i].initialGeometry.y()].push_back(i);
		}

		const auto textHeight = st::messageTextStyle.font->height;
		const auto uniformCaptionHeight = 4 + textHeight + 5; // 4px top + text height + 5px bottom

		int captionsCount = 0;
		for (const auto &part : _parts) {
			if (!part.item->originalText().empty()) {
				captionsCount++;
			}
		}
		const bool usePerItemCaptions = (captionsCount > 1);

		int totalShift = 0;
		newHeight = 0;

		for (auto const& [rowY, indices] : rows) {
			bool rowHasCaption = false;
			int rowBottomMax = 0;

			for (const auto i : indices) {
				_parts[i].geometry.translate(0, totalShift);
				rowBottomMax = std::max(rowBottomMax, _parts[i].geometry.y() + _parts[i].geometry.height());
			}
			
			accumulate_max(newHeight, rowBottomMax);

			if (usePerItemCaptions) {
				for (const auto i : indices) {
					if (!_parts[i].item->originalText().empty()) {
						rowHasCaption = true;
						break;
					}
				}
			}

			if (rowHasCaption) {
				for (const auto i : indices) {
					auto &part = _parts[i];
					if (!part.item->originalText().empty()) {
						part._captionHeight = uniformCaptionHeight;
						part._captionText = Ui::Text::String(
							st::messageTextStyle,
							part.item->originalText(),
							Ui::ItemTextDefaultOptions(),
							st::msgMinWidth,
							Core::TextContext({
								.session = &_parent->history()->session(),
								.repaint = [=] { _parent->customEmojiRepaint(); },
							}));

						part.captionRect = QRect(
							part.geometry.left(),
							part.geometry.y() + part.geometry.height(),
							part.geometry.width(),
							uniformCaptionHeight);
					} else {
						part.captionRect = QRect();
					}
				}
				const auto isLastRow = (rowY == rows.rbegin()->first);
				const auto shift = isLastRow ? uniformCaptionHeight : (uniformCaptionHeight - spacing);
				totalShift += shift;
				newHeight += shift;
			} else {
				for (const auto i : indices) {
					_parts[i].captionRect = QRect();
				}
			}
		}
	}

	const auto groupPadding = groupedPadding();
	newHeight += groupPadding.top() + groupPadding.bottom();

	return { newWidth, int(base::SafeRound(newHeight)) };
}


void GroupedMedia::refreshParentId(
		not_null<HistoryItem*> realParent) {
	for (const auto &part : _parts) {
		part.content->refreshParentId(part.item);
	}
}

Ui::BubbleRounding GroupedMedia::applyRoundingSides(
		Ui::BubbleRounding already,
		RectParts sides) const {
	auto result = Ui::GetCornersFromSides(sides);
	if (!(result & RectPart::TopLeft)) {
		already.topLeft = Ui::BubbleCornerRounding::None;
	}
	if (!(result & RectPart::TopRight)) {
		already.topRight = Ui::BubbleCornerRounding::None;
	}
	if (!(result & RectPart::BottomLeft)) {
		already.bottomLeft = Ui::BubbleCornerRounding::None;
	}
	if (!(result & RectPart::BottomRight)) {
		already.bottomRight = Ui::BubbleCornerRounding::None;
	}
	return already;
}

QMargins GroupedMedia::groupedPadding() const {
	const auto normal = st::msgFileLayout.padding;
	const auto grouped = st::msgFileLayoutGrouped.padding;
	const auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
	// Return 0 bottom padding so we control it manually
	return QMargins(
		0,
		(normal.top() - grouped.top()) - topMinus,
		0,
		0); 
}

QRect GroupedMedia::groupItemRect(int index) const {
	if (index >= 0 && index < int(_parts.size())) {
		return _parts[index].geometry.translated(
			0,
			groupedPadding().top());
	}
	return {};
}

Media *GroupedMedia::partMediaAt(QPoint point) const {
	point -= QPoint(0, groupedPadding().top());
	for (const auto &part : _parts) {
		if (part.geometry.contains(point)) {
			return part.content.get();
		}
	}
	return nullptr;
}

Media *GroupedMedia::lookupSpoilerTagMedia() const {
	// Only Photo and Gif parts implement the spoiler tag methods, and they
	// are the only kinds a Mode::Grid group holds. A Mode::Column group is
	// built from Document parts, which would hit the base class Unexpected().
	if (_parts.empty() || _mode == Mode::Column) {
		return nullptr;
	}
	const auto media = _parts.front().content.get();
	if (media && _parts.front().item->isMediaSensitive()) {
		return media;
	}
	const auto photo = media ? media->getPhoto() : nullptr;
	return (photo && photo->extendedMediaPreview()) ? media : nullptr;
}

QImage GroupedMedia::generateSpoilerTagBackground(QRect full) const {
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(
		full.size() * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	auto p = QPainter(&result);
	const auto shift = -full.topLeft();
	const auto skip1 = st::historyGroupSkip / 2;
	const auto skip2 = st::historyGroupSkip - skip1;
	for (const auto &part : _parts) {
		auto background = part.content->spoilerTagBackground();
		const auto extended = part.geometry.translated(shift).marginsAdded(
			{ skip1, skip1, skip2, skip2 });
		if (background.isNull()) {
			p.fillRect(extended, Qt::black);
		} else {
			p.drawImage(extended, background);
		}
	}
	p.end();

	return ::Media::Streaming::PrepareBlurredBackground(
		full.size(),
		std::move(result));
}

void GroupedMedia::drawHighlight(
		Painter &p,
		const PaintContext &context,
		int top) const {
	if (context.highlight.opacity == 0.) {
		return;
	}
	auto selection = context.highlight.range;
	
	const auto empty = selection.empty();
	const auto subpart = IsSubGroupSelection(selection);
	const auto skip = top + groupedPadding().top();

	std::set<int> highlightedRows;
	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];

		// Calculate length based on mode
		const auto length = (_mode == Mode::Grid)
			? part.item->originalText().text.size()
			: part.content->fullSelectionLength();

		// Check if the global selection intersects with this part's local range [0, length)
		bool full = (!i && empty)
			|| (subpart && IsGroupItemSelection(selection, i))
			|| (!subpart
				&& !selection.empty()
				&& (selection.from < length)); // Standard check: if start is before end of this part

		if (full) {
			highlightedRows.emplace(part.geometry.y());
		}

		if (!subpart) {
			if (_mode == Mode::Column) {
				selection = part.content->skipSelection(selection);
			} else if (length > 0) {
				// Shift selection for Grid mode (subtract length)
				selection = UnshiftItemSelection(selection, length);
			}
		}
	}

	for (const auto rowY : highlightedRows) {
		auto rowTop = rowY;
		auto rowBottom = rowY;
		auto initialized = false;
		
		// Find the row boundaries
		for (const auto &part : _parts) {
			if (part.geometry.y() == rowY) {
				auto r = part.geometry;
				if (_mode == Mode::Grid && !part.captionRect.isEmpty()) {
					r = r.united(part.captionRect);
				}
				if (!initialized) {
					rowTop = r.top();
					rowBottom = r.top() + r.height();
					initialized = true;
				} else {
					rowTop = std::min(rowTop, r.top());
					rowBottom = std::max(rowBottom, r.top() + r.height());
				}
			}
		}

		if (initialized) {
			auto copy = context;
			copy.highlight.range = {};

			auto highlightY = rowTop + skip;
			auto highlightHeight = rowBottom - rowTop;

			if (_mode == Mode::Column) {
				highlightHeight -= 10;
			}
			
			// For highlighting context, we try to find the item corresponding to the row.
			// In Grid mode, since we determined which row to highlight based on global selection,
			// any item in that row is a valid anchor for paintCustomHighlight.
			// Using the first item of the row is standard behavior.
			const HistoryItem* highlightItem = nullptr;
             for (const auto &part : _parts) {
                 if (part.geometry.y() == rowY) {
                     highlightItem = part.item;
                     break;
                 }
             }
             if (!highlightItem) highlightItem = _parts.front().item;

			_parent->paintCustomHighlight(
				p,
				copy,
				highlightY,
				highlightHeight,
				highlightItem);
		}
	}
}


void GroupedMedia::draw(Painter &p, const PaintContext &context) const {
	auto wasCache = false;
	auto nowCache = false;
	const auto groupPadding = groupedPadding();
	
	auto selection = context.selection;
	const auto fullSelection = context.selected();
	const auto textSelection = (_mode == Mode::Column)
		&& !fullSelection
		&& !IsSubGroupSelection(selection);

	const auto inWebPage = (_parent->media() != this);
	constexpr auto kSmall = Ui::BubbleCornerRounding::Small;
	const auto rounding = IsHostedInstantViewMedia(_parent)
		? Ui::BubbleRounding()
		: inWebPage
		? Ui::BubbleRounding{ kSmall, kSmall, kSmall, kSmall }
		: adjustedBubbleRounding();
	
	auto highlight = context.highlight.range;
	const auto tagged = lookupSpoilerTagMedia();
	auto fullRect = QRect();
	const auto subpartHighlight = IsSubGroupSelection(highlight);
	const auto st = context.st;
	const auto stm = context.messageStyle();

	const bool showInfo = _parent->isUnderCursor() || context.selected();

	// Calculate if we have a single caption for the whole grid
	bool singleCaptionAnywhere = false;
	if (_mode == Mode::Grid) {
		int count = 0;
		for (const auto &part : _parts) {
			if (!part.item->originalText().empty()) count++;
		}
		singleCaptionAnywhere = (count == 1);
	}

	// For Grid mode text selection offset calculation
	int textOffset = 0;

	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];
		
		// Prepare selection for this specific part
		auto partSelection = FullSelection;
		
		if (fullSelection) {
			partSelection = FullSelection;
		} else if (_mode == Mode::Column) {
			if (textSelection) {
				// Column mode handles skipping internally via media classes
				// We update the main 'selection' variable at the end of the loop
				partSelection = selection; 
			} else if (IsGroupItemSelection(selection, i)) {
				partSelection = FullSelection;
			} else {
				partSelection = TextSelection();
			}
		} else {
			// Grid Mode Selection Logic
			const auto textLen = part.item->originalText().text.size();
			if (textLen > 0) {
				if (selection == FullSelection) {
					partSelection = FullSelection;
				} else if (!selection.empty() && !IsGroupItemSelection(selection, i)) {
					// Map global symbols to local range
					// Global range: [selection.from, selection.to)
					// Local range:  [textOffset, textOffset + textLen)
					const int localFrom = std::max((int)selection.from, textOffset);
					const int localTo = std::min((int)selection.to, textOffset + (int)textLen);

					if (localFrom < localTo) {
						partSelection = TextSelection(
							(uint16)(localFrom - textOffset), 
							(uint16)(localTo - textOffset)
						);
					} else {
						partSelection = TextSelection();
					}
				} else if (IsGroupItemSelection(selection, i)) {
					partSelection = FullSelection;
				} else {
					partSelection = TextSelection();
				}
			} else {
				partSelection = IsGroupItemSelection(selection, i) ? FullSelection : TextSelection();
			}
		}

		auto partContext = context.withSelection(partSelection);

		bool textHighlighted = false;
		if (_mode == Mode::Grid
			&& !highlight.empty()
			&& !IsGroupItemSelection(highlight, i)) {
			const auto len = part.item->originalText().text.size();
			if (len > 0) {
				const int localFrom = std::max((int)highlight.from, textOffset);
				const int localTo = std::min((int)highlight.to, textOffset + (int)len);
				if (localFrom < localTo) {
					textHighlighted = true;
				}
			}
		}

		const auto highlighted = (highlight.empty() && !i)
			|| IsGroupItemSelection(highlight, i)
			|| textHighlighted;
		const auto highlightOpacity = highlighted
			? context.highlight.opacity
			: 0.;
		partContext.highlight.range = highlighted
			? TextSelection()
			: highlight;

		// Draw Media Content
		if (!part.cache.isNull()) {
			wasCache = true;
		}

		part.content->drawGrouped(
			p,
			partContext,
			part.geometry.translated(0, groupPadding.top()),
			part.sides,
			applyRoundingSides(rounding, part.sides),
			highlightOpacity,
			&part.cacheKey,
			&part.cache);

		// --- Draw Video Duration Overlays ---
		// Grid and file-playlist rows alike show duration and size
		// inside video parts, so mixed groups don't lose that info.
		if (showInfo) {
			const auto dataMedia = part.item->media();
			qint64 durSeconds = -1;
			qint64 sizeBytes = -1;
			if (const auto file = dynamic_cast<Data::MediaFile*>(dataMedia)) {
				const auto document = file->document();
				if (document && document->isVideoFile()) {
					durSeconds = std::max<qint64>(0, document->duration() / 1000);
					sizeBytes = document->size;
				}
			} else if (const auto photoMedia = dynamic_cast<Data::MediaPhoto*>(dataMedia)) {
				const auto photo = photoMedia->photo();
				if (photo && photo->videoCanBePlayed()) {
					if (const auto d = photo->extendedMediaVideoDuration()) {
						durSeconds = std::max<qint64>(0, *d);
					} else {
						durSeconds = 0;
					}
					sizeBytes = photo->videoByteSize(Data::PhotoSize::Large);
				}
			}

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

				auto mediaGeometry = part.geometry.translated(0, groupPadding.top());
				const auto bubbleX = mediaGeometry.x() + mediaGeometry.width() - bubbleW - st::msgDateImgDelta;
				const auto bubbleY = mediaGeometry.y() + mediaGeometry.height() - bubbleH - st::msgDateImgDelta;

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

		// --- Draw Column Mode Info ---
		if (_mode == Mode::Column) {
			const auto item = part.item;
			const auto edited = item->Get<HistoryMessageEdited>();
			const bool editedNow = (edited && !item->hideEditedBadge());
			
			if (i == 0) {
				// First Item: Full Info
				const auto views = item->Get<HistoryMessageViews>();
				const auto viewsText = (views && views->views.count >= 0)
					? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
					: QString();
				const auto editText = editedNow ? (QString::fromUtf8("✏️")) : QString();
				const auto timeText = QLocale().toString(
					ItemDateTime(item).time(),
					GetEnhancedBool("show_seconds")
						? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
						: QLocale::system().timeFormat(QLocale::ShortFormat));
				const auto idText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
					? (QString(" (") + QString::number(item->fullId().msg.bare) + QString(")"))
					: QString();

				p.setFont(st::msgDateFont);
				p.setPen(stm->msgDateFg);

				const auto itemRect = part.geometry.translated(0, groupPadding.top());
				bool hasThumb = false;
				if (const auto fileMedia = dynamic_cast<Data::MediaFile*>(part.item->media())) {
					if (const auto document = fileMedia->document()) {
						hasThumb = document->hasThumbnail() && !document->isSong();
					}
				}
				const auto &docStyle = hasThumb ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped;
				
				// Fix alignment: Matches Document::draw LayoutMode::Grouped logic
				// statustop = st.statusTop - st.padding.top() - topMinus
				const auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
				const auto statustop = docStyle.statusTop - docStyle.padding.top() - topMinus;
				const auto baseY = itemRect.y() + statustop + st::normalFont->ascent + 5;

				const int iconGap = 1;
				const int textGap = st::msgDateFont->width(' ');
				const int iconW = st::historyViewsWidth;
				const int viewsW = viewsText.isEmpty() ? 0 : (iconW + iconGap + st::msgDateFont->width(viewsText));
				const int editedW = editedNow ? st::msgDateFont->width(editText) : 0;
				const int timeIdW = st::msgDateFont->width(timeText + idText);
				
				int totalW = 0;
				if (viewsW > 0) totalW += viewsW + textGap;
				if (editedW > 0) totalW += editedW + textGap;
				totalW += timeIdW;

				int x = itemRect.x() + itemRect.width() - totalW - st::msgDateImgDelta;
				
				// Prevent overlap
				int reservedLeft = 0;
				const int nameleft = docStyle.padding.left() + docStyle.thumbSize + docStyle.thumbSkip;
				QString statusText;
				if (const auto fileMedia = dynamic_cast<Data::MediaFile*>(part.item->media())) {
					if (const auto document = fileMedia->document()) {
						statusText = Ui::FormatSizeText(document->size);
					}
				}
				if (!statusText.isEmpty()) {
					reservedLeft = nameleft + st::normalFont->width(statusText) + st::normalFont->spacew;
				}
				if (reservedLeft > 0) {
					const auto minGap = st::normalFont->spacew;
					const auto minX = reservedLeft + minGap;
					if (x < minX) x = minX;
				}

				if (viewsW > 0) {
					const auto &icon = stm->historyViewsIcon;
					const int iconH = icon.height();
					const int scaledH = (iconH * iconW) / std::max(1, icon.width());
					
					const int lineH = st::msgDateFont->height;
					const int lineTop = baseY - st::msgDateFont->ascent;
					
					const int iconTop = lineTop + (lineH - scaledH) / 2;
					icon.paint(p, x, iconTop, iconW);
					p.drawText(x + iconW + iconGap, baseY, viewsText);
					x += viewsW + textGap;
				}
				if (editedW > 0) {
					p.drawText(x, baseY, editText);
					x += editedW + textGap;
				}
				p.drawText(x, baseY, timeText + idText);
			} else {
				QString infoText;
				if (editedNow) infoText += QString::fromUtf8("✏️");
				if (GetEnhancedBool("show_messages_id")) {
					const auto msgId = item->fullId().msg;
					if (msgId > 0) {
						if (!infoText.isEmpty()) {
							infoText += ' ';
							infoText += QString("(%1)").arg(msgId.bare);
						} else {
							infoText += QString::number(msgId.bare);
						}
					}
				}

				if (!infoText.isEmpty()) {
					p.setFont(st::msgDateFont);
					p.setPen(stm->msgDateFg);

					const auto itemRect = part.geometry.translated(0, groupPadding.top());
					bool hasThumb = false;
					if (const auto fileMedia = dynamic_cast<Data::MediaFile*>(part.item->media())) {
						if (const auto document = fileMedia->document()) {
							hasThumb = document->hasThumbnail() && !document->isSong();
						}
					}
					const auto &docStyle = hasThumb ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped;

					
					// Fix alignment: Matches Document::draw LayoutMode::Grouped logic
					const auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
					const auto statustop = docStyle.statusTop - docStyle.padding.top() - topMinus;
					const auto baseY = itemRect.y() + statustop + st::normalFont->ascent + 5;
					
					const auto textWidth = st::msgDateFont->width(infoText);
					
					const auto textX = itemRect.x() + itemRect.width() - textWidth - st::msgDateImgDelta;
					
					p.drawText(textX, baseY, infoText);
				}
			}
		} 
		// --- Grid Mode Info Bubbles ---
		else if (_mode == Mode::Grid && showInfo) {
			if (i == 0) {
				const auto item = part.item;
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
				const int dateWidth = font->width(dateText + msgIdText);
				totalWidth += dateWidth;

				if (edited) {
					totalWidth += textPadding + font->width(QString::fromUtf8("✏️"));
				}

				int viewsWidth = 0;
				const int viewsIconGap = 1;
				if (!viewsText.isEmpty()) {
					viewsWidth = st::historyViewsWidth + viewsIconGap + font->width(viewsText);
					totalWidth += textPadding + viewsWidth;
				}

				const auto sti = context.imageStyle();
				const auto textHeight = font->height;
				const auto hPadding = 2;
				const auto vPadding = st::msgDateImgPadding.y();
				const auto bubbleW = totalWidth + 2 * hPadding;
				const auto bubbleH = textHeight + 2 * vPadding;

				const auto itemGeometry = part.geometry.translated(0, groupPadding.top());
				const auto bubbleX = itemGeometry.x() + itemGeometry.width() - bubbleW - st::msgDateImgDelta;
				const auto bubbleY = itemGeometry.y() + st::msgDateImgDelta;

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
					const int iconY = bubbleY + (bubbleH - scaledIconH) / 2 + 1;
					icon.paint(p, currentLeft, iconY, st::historyViewsWidth);
					p.drawText(currentLeft + st::historyViewsWidth + viewsIconGap, textBaseY, viewsText);
					currentLeft += viewsWidth + textPadding;
				}
				if (edited) {
					const auto editedText = QString::fromUtf8("✏️");
					const auto editedWidth = font->width(editedText);
					p.setFont(font);
					p.drawText(currentLeft, textBaseY, editedText);
					currentLeft += editedWidth + textPadding;
					p.setFont(font->bold());
				}
				p.drawText(currentLeft, textBaseY, dateText + msgIdText);

				// Right Action
				if (const auto size = _parent->hasBubble() ? std::nullopt : _parent->rightActionSize()) {
					auto fullRight = width();
					auto fullBottom = height();
					auto fastShareLeft = _parent->hasRightLayout()
						? (-size->width() - st::historyFastShareLeft)
						: (fullRight + st::historyFastShareLeft);
					auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
					_parent->drawRightAction(p, context, fastShareLeft, fastShareTop, width());
				}
			} else {
				drawMessageIdInfo(p, context, part.geometry.translated(0, groupPadding.top()), part.item);
			}
		}

		// --- Draw Caption ---
		if ((_mode == Mode::Grid) && part._captionHeight > 0 && !part.captionRect.isEmpty()) {
			auto captionRect = part.captionRect.translated(0, groupPadding.top());

			p.setPen(stm->historyTextFg);
			p.setFont(st::messageTextStyle.font);

			p.save();
			p.setClipRect(captionRect);

			const auto padding = QMargins(8, 0, 8, 0); // Only horizontal padding, vertical handled separately
			const auto availableWidth = captionRect.width() - padding.left() - padding.right();

			auto highlightRequest = context.computeHighlightCache();
			if (highlightRequest) {
				const auto len = part.item->originalText().text.size();
				const auto range = highlightRequest->range;
				
				// Map Global Range to Local Part Range
				// Global Part Interval: [textOffset, textOffset + len)
				// Highlight Range: [range.from, range.to)
				
				// Intersection
				const int from = std::max(static_cast<int>(range.from), textOffset);
				const int to = std::min(static_cast<int>(range.to), static_cast<int>(textOffset + len));

				if (from < to) {
					// Valid intersection, map to local
					highlightRequest->range = TextSelection(
						(uint16)(from - textOffset),
						(uint16)(to - textOffset)
					);
				} else {
					highlightRequest = std::nullopt;
				}
			}

			TextSelection paintSelection = partSelection;

			const auto availableHeight = captionRect.height();
			const auto textHeight = part._captionText.countHeight(availableWidth);

			const auto requiredSpace = textHeight + 4 + 5; // 4px top + text height + 5px bottom
			int verticalOffset = 4; // Always start with 4px from top
			if (requiredSpace <= availableHeight) {
				verticalOffset = (availableHeight - requiredSpace) / 2 + 4;
			} else {
				verticalOffset = 4;
			}

			const auto captionLeft = captionRect.left() + padding.left();
			const auto captionTop = captionRect.top() + verticalOffset;
			_parent->prepareCustomEmojiPaint(p, context, part._captionText);

			part._captionText.draw(p, {
				.position = QPoint(captionLeft, captionTop),
				.outerWidth = _parent->width(),
				.availableWidth = availableWidth,
				.geometry = {},
				.align = style::al_left,
				.clip = QRect(),
				.palette = &stm->textPalette,
				.pre = stm->preCache.get(),
				.blockquote = context.quoteCache(
					_parent->contentColorCollectible(),
					_parent->contentColorIndex()),
				.colors = context.st->highlightColors(),
				.spoiler = Ui::Text::DefaultSpoilerCache(),
				.now = context.now,
				.paused = context.paused,
				.pausedEmoji = context.paused || On(PowerSaving::kEmojiChat),
				.pausedSpoiler = context.paused || On(PowerSaving::kChatSpoiler),
				.fullWidthSelection = false,
				.selection = paintSelection,
				.highlight = highlightRequest ? &*highlightRequest : nullptr,
				.elisionLines = 1,
			});
			p.restore();
		}

		if (!part.cache.isNull()) {
			nowCache = true;
		}
		if (tagged || _purchasedPriceTag) {
			fullRect = fullRect.united(part.geometry);
		}

		// Update offset for next part in Grid mode
		if (_mode == Mode::Grid) {
			if (!part.item->originalText().empty()) {
				textOffset += part.item->originalText().text.size();
			}
		} else {
			// Column mode
			if (textSelection) {
				selection = part.content->skipSelection(selection);
			}
			if (!subpartHighlight) {
				highlight = part.content->skipSelection(highlight);
			}
		}
	}
	
	if (nowCache && !wasCache) {
		history()->owner().registerHeavyViewPart(_parent);
	}

	if (tagged) {
		tagged->drawSpoilerTag(p, fullRect, context, [&] {
			return generateSpoilerTagBackground(fullRect);
		});
	} else if (_purchasedPriceTag) {
		drawPurchasedTag(p, fullRect, context);
	}
}

TextState GroupedMedia::getPartState(
		QPoint point,
		StateRequest request) const {
	auto shift = 0;
	auto i = 0;
	
	for (const auto &part : _parts) {
		const auto groupPadding = groupedPadding();
		const auto mediaGeo = part.geometry.translated(0, groupPadding.top());
		const auto captionGeo = part.captionRect.translated(0, groupPadding.top());

		const auto isInside = mediaGeo.contains(point)
			|| (!captionGeo.isEmpty() && captionGeo.contains(point));

		if (isInside) {
			if (_mode == Mode::Grid
				&& !captionGeo.isEmpty()
				&& captionGeo.contains(point)) {
				
				const auto originalText = part.item->originalText();
				if (!originalText.empty()) {
					auto result = TextState(part.item);
					const auto padding = QMargins(8, 0, 8, 0);
					const auto captionWidth = captionGeo.width() - padding.left() - padding.right();
					
					const auto clickX = point.x() - captionGeo.left() - padding.left();
					const auto clickY = point.y() - captionGeo.top() - 2; 
					
					const auto textStateResult = part._captionText.getState(
						QPoint(clickX, clickY), 
						captionWidth, 
						request.forText());

					result.cursor = CursorState::Text;
					result.link = textStateResult.link;
					result.symbol = textStateResult.symbol + shift;
					result.afterSymbol = textStateResult.afterSymbol;
					result.itemId = part.item->fullId();

					// Only show tooltip if we are NOT looking for text selection/symbol (which is used for copying).
					if (part._captionText.maxWidth() > captionWidth && !(request.flags & Ui::Text::StateRequest::Flag::LookupSymbol)) {
						result.customTooltip = true;
						result.customTooltipText = originalText.text;
					}
					
					return result;
				}
			}
			
			auto result = part.content->getStateGrouped(
				part.geometry, 
				part.sides,
				point - QPoint(0, groupPadding.top()), 
				request);
			AddTextStateOffset(&result, uint16(shift));
			result.itemId = part.item->fullId();

			const auto item = part.item;
			const auto edited = item->Get<HistoryMessageEdited>();

			if (_mode == Mode::Column) {
				bool hasThumb = false;
				if (const auto fileMedia = dynamic_cast<Data::MediaFile*>(part.item->media())) {
					if (const auto document = fileMedia->document()) {
						hasThumb = document->hasThumbnail() && !document->isSong();
					}
				}
				const auto &docStyle = hasThumb ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped;
				const auto topMinus = st::msgFileTopMinus;
				const auto statustop = docStyle.statusTop - topMinus;
				const auto itemRect = part.geometry;
				const auto y = itemRect.y() + statustop;
				const int currentRight = itemRect.x() + itemRect.width();

				if (i == 0) {
					const auto font = st::msgDateFont;
					const auto timeText = QLocale().toString(
						ItemDateTime(item).time(),
						GetEnhancedBool("show_seconds")
							? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
							: QLocale::system().timeFormat(QLocale::ShortFormat));
					const auto idText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
						? QString(" (%1)").arg(item->fullId().msg.bare)
						: QString();
					const bool editedNow = (edited && !item->hideEditedBadge());
					const int editedW = editedNow ? font->width(QString::fromUtf8("✏️")) : 0;
					const int timeIdW = font->width(timeText + idText);
					const auto views = item->Get<HistoryMessageViews>();
					const auto viewsText = (views && views->views.count >= 0)
						? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
						: QString();
					const int iconGap = 1;
					const int iconW = st::historyViewsWidth;
					const int viewsW = viewsText.isEmpty() ? 0 : (iconW + iconGap + font->width(viewsText));

					const int textGap = font->spacew;
					const int lineTop = y;
					const int lineH = font->height;

					int totalW = 0;
					if (viewsW > 0) totalW += viewsW + textGap;
					if (editedW > 0) totalW += editedW + textGap;
					totalW += timeIdW;

					const int startX = currentRight - st::msgDateImgDelta - totalW;
					int currentX = startX;

					QRect viewsRect;
					if (viewsW > 0) {
						viewsRect = QRect(currentX, lineTop, viewsW, lineH);
						currentX += viewsW + textGap;
					}
					QRect editedOnlyRect;
					if (editedW > 0) {
						editedOnlyRect = QRect(currentX, lineTop, editedW, lineH);
						currentX += editedW + textGap;
					}
					const QRect timeIdRect(currentX, lineTop, timeIdW, lineH);
					const QRect editedRect = editedNow
						? QRect(editedOnlyRect.isNull() ? timeIdRect.left() : editedOnlyRect.left(),
							lineTop,
							(editedOnlyRect.isNull() ? 0 : editedOnlyRect.width()) + timeIdRect.width(),
							lineH)
						: QRect();

					if (viewsW > 0 && viewsRect.contains(point - QPoint(0, groupPadding.top()))) {
						result.customTooltip = true;
						result.customTooltipText = QString("Views: ") + viewsText;
					} else if (editedNow && editedRect.contains(point - QPoint(0, groupPadding.top()))) {
						const auto uploadLocal = ItemDateTime(item);
						QString tooltipText = tr::lng_uploaded(tr::now) + ": "
							+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
							+ uploadLocal.time().toString("HH:mm:ss");
						const auto msgIdValue = item->fullId().msg;
						if (msgIdValue > 0 && GetEnhancedBool("show_messages_id")) {
							tooltipText += "  ID: " + QString::number(msgIdValue.bare);
						}
						const auto editLocal = base::unixtime::parse(edited->date);
						QString editedTranslation = tr::lng_edited(tr::now);
						editedTranslation = editedTranslation.toUpper().left(1) + editedTranslation.mid(1);
						tooltipText += "\n" + editedTranslation + ": "
							+ editLocal.date().toString("dddd, dd MMMM yyyy") + " "
							+ editLocal.time().toString("HH:mm:ss");
						result.customTooltip = true;
						result.customTooltipText = tooltipText;
					} else if (timeIdRect.contains(point - QPoint(0, groupPadding.top()))) {
						const auto uploadLocal = ItemDateTime(item);
						QString tooltipText = tr::lng_uploaded(tr::now) + ": "
							+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
							+ uploadLocal.time().toString("HH:mm:ss");
						const auto msgIdValue = item->fullId().msg;
						if (msgIdValue > 0 && GetEnhancedBool("show_messages_id")) {
							tooltipText += "  ID: " + QString::number(msgIdValue.bare);
						}
						result.customTooltip = true;
						result.customTooltipText = tooltipText;
					}
				} else {
					QString infoText;
					if (edited && !item->hideEditedBadge()) {
						infoText += QString::fromUtf8("✏️");
					}
					if (GetEnhancedBool("show_messages_id")) {
						const auto msgId = item->fullId().msg;
						if (msgId > 0) {
							if (!infoText.isEmpty()) {
								infoText += ' ';
								infoText += QString("(%1)").arg(msgId.bare);
							} else {
								infoText += QString::number(msgId.bare);
							}
						}
					}
					if (!infoText.isEmpty()) {
						const auto textWidth = st::msgDateFont->width(infoText);
						const auto textHeight = st::msgDateFont->height;
						const auto hPadding = 2;
						const auto vPadding = st::msgDateImgPadding.y();
						const auto dateW = textWidth + (2 * hPadding);
						const auto dateH = textHeight + 2 * vPadding;
						const auto bubbleX = currentRight - dateW - st::msgDateImgDelta;
						const auto bubbleY = y;
						const QRect infoRect(bubbleX, bubbleY, dateW, dateH);

						if (infoRect.contains(point - QPoint(0, groupPadding.top()))) {
							QString tooltipText;
							if (GetEnhancedBool("show_messages_id")) {
								const auto msgId = item->fullId().msg;
								if (msgId > 0) {
									tooltipText = QString("Message ID: ") + QString::number(msgId.bare);
								}
							}
							if (edited && !item->hideEditedBadge()) {
								const auto editLocalTime = base::unixtime::parse(edited->date);
								QString editedTranslation = tr::lng_edited(tr::now);
								editedTranslation = editedTranslation.toUpper().left(1) + editedTranslation.mid(1);
								if (!tooltipText.isEmpty()) tooltipText += "\n";
								tooltipText += editedTranslation + ": "
									+ editLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
									+ editLocalTime.time().toString("HH:mm:ss");
							}
							if (!tooltipText.isEmpty()) {
								result.customTooltip = true;
								result.customTooltipText = tooltipText;
							}
						}
					}
				}
			} else if (_mode == Mode::Grid && (&part != &_parts.front())) {
				QString infoText;
				if (edited && !item->hideEditedBadge()) {
					infoText += QString::fromUtf8("✏️");
				}
				if (GetEnhancedBool("show_messages_id")) {
					const auto msgId = item->fullId().msg;
					if (msgId > 0) {
						if (!infoText.isEmpty()) {
							infoText += ' ';
							infoText += QString("(%1)").arg(msgId.bare);
						} else {
							infoText += QString::number(msgId.bare);
						}
					}
				}

				if (!infoText.isEmpty()) {
					const auto textWidth = st::msgDateFont->width(infoText);
					const auto textHeight = st::msgDateFont->height;
					const auto hPadding = 2;
					const auto vPadding = st::msgDateImgPadding.y();
					const auto dateW = textWidth + (2 * hPadding);
					const auto dateH = textHeight + 2 * vPadding;
					const auto itemRect = part.geometry;

					const auto bubbleX = (dateW > itemRect.width())
						? itemRect.x()
						: (itemRect.x() + itemRect.width() - dateW);
					const auto bubbleY = itemRect.y();
					const QRect infoRect(bubbleX, bubbleY, dateW, dateH);

					if (infoRect.contains(point - QPoint(0, groupPadding.top()))) {
						QString tooltipText;
						if (GetEnhancedBool("show_messages_id")) {
							const auto msgId = item->fullId().msg;
							if (msgId > 0) {
								tooltipText = QString("Message ID: ") + QString::number(msgId.bare);
							}
						}
						if (edited && !item->hideEditedBadge()) {
							const auto editLocalTime = base::unixtime::parse(edited->date);
							QString editedTranslation = tr::lng_edited(tr::now);
							editedTranslation = editedTranslation.toUpper().left(1) + editedTranslation.mid(1);
							if (!tooltipText.isEmpty()) tooltipText += "\n";
							tooltipText += editedTranslation + ": "
								+ editLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
								+ editLocalTime.time().toString("HH:mm:ss");
						}
						if (!tooltipText.isEmpty()) {
							result.customTooltip = true;
							result.customTooltipText = tooltipText;
						}
					}
				}
			}
			return result;
		}
		if (_mode == Mode::Grid) {
			const auto originalText = part.item->originalText();
			if (!originalText.text.isEmpty()) {
				shift += originalText.text.size();
			}
		} else {
			shift += part.content->fullSelectionLength();
		}
		++i;
	}

	return TextState(_parent->data());
}

PointState GroupedMedia::pointState(QPoint point) const {
	if (!QRect(0, 0, width(), height()).contains(point)) {
		return PointState::Outside;
	}
	const auto groupPadding = groupedPadding();
	point -= QPoint(0, groupPadding.top());
	for (const auto &part : _parts) {
		if (part.geometry.contains(point)
			|| (!part.captionRect.isEmpty() && part.captionRect.contains(point))) {
			return PointState::GroupPart;
		}
	}
	return PointState::Inside;
}

TextState GroupedMedia::textState(QPoint point, StateRequest request) const {
	const auto groupPadding = groupedPadding();
	auto result = getPartState(point, request);
	if (const auto tagged = lookupSpoilerTagMedia()) {
		if (QRect(0, 0, width(), height()).contains(point)) {
			if (auto link = tagged->spoilerTagLink()) {
				result.link = std::move(link);
			}
		}
	}

	if ((_mode == Mode::Grid || _mode == Mode::Column) && !_parts.empty() && _parent->media() == this) {
		const auto firstPart = &_parts.front();
		const auto firstItemGeometry = firstPart->geometry.translated(0, groupPadding.top());

		if (_mode == Mode::Grid) {
			const auto right = firstItemGeometry.x() + firstItemGeometry.width();
			const auto bottom = firstItemGeometry.y(); // Match top-right alignment.

			// Manually calculate the info rect to match the drawing logic.
			const auto item = firstPart->item;
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

			int totalWidth = font->width(dateText + msgIdText);
			const int viewsIconGap = 1;
			const int textPadding = font->spacew;
			if (edited) {
				const auto editedWidth = font->width(QString::fromUtf8("✏️"));
				totalWidth += textPadding + editedWidth;
			}
			int viewsWidth = 0;
			if (!viewsText.isEmpty()) {
				viewsWidth = st::historyViewsWidth + viewsIconGap + font->width(viewsText);
				totalWidth += textPadding + viewsWidth;
			}

			const auto hPadding = 2;
			const auto vPadding = st::msgDateImgPadding.y();
			const auto bubbleW = totalWidth + 2 * hPadding;
			const auto bubbleH = font->height + 2 * vPadding;
			const auto bubbleX = right - bubbleW - st::msgDateImgDelta;
			const auto bubbleY = bottom + st::msgDateImgDelta; // 'bottom' here is actually the top edge.

			const QRect infoRect(bubbleX, bubbleY, bubbleW, bubbleH);
				// Build three hover areas for Grid first bubble from the left edge:
				// views (icon+count), edited+time+id (if edited), time+id.
				const int iconGap = 1;
				const int iconW = st::historyViewsWidth;
				const int viewsW = viewsText.isEmpty() ? 0 : (iconW + iconGap + font->width(viewsText));
				const int editedW = edited ? font->width(QString::fromUtf8("✏️")) : 0;
				const int timeIdW = font->width(dateText + msgIdText);
				int hoverLeft = bubbleX + hPadding;
				QRect viewsRect, timeIdRect, editedRect;
				if (viewsW > 0) {
					viewsRect = QRect(hoverLeft, bubbleY, viewsW, bubbleH);
					hoverLeft += viewsW + textPadding;
				}
				QRect editedOnlyRect;
				if (edited) {
					editedOnlyRect = QRect(hoverLeft, bubbleY, editedW, bubbleH);
					hoverLeft += editedW + textPadding;
				}
				timeIdRect = QRect(hoverLeft, bubbleY, timeIdW, bubbleH);
				// Edited tooltip covers both the edited icon and the time+id area when present.
				if (edited) {
					editedRect = QRect(editedOnlyRect.isNull() ? timeIdRect.left() : editedOnlyRect.left(),
						bubbleY,
						(editedOnlyRect.isNull() ? 0 : editedOnlyRect.width()) + timeIdRect.width(),
						bubbleH);
				}
				// Views tooltip
				if (viewsW > 0 && viewsRect.contains(point)) {
				result.customTooltip = true;
				result.customTooltipText = QString("Views: ") + viewsText;
				return result;
			}
			// Edited tooltip includes Uploaded + Edited lines
			if (edited && editedRect.contains(point)) {
				const auto uploadLocalTime = ItemDateTime(item);
				QString tooltipText = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocalTime.time().toString("HH:mm:ss");
				const auto msgIdValue = item->fullId().msg;
				if (msgIdValue > 0 && GetEnhancedBool("show_messages_id")) {
					tooltipText += "  ID: " + QString::number(msgIdValue.bare);
				}
				const auto editLocalTime = base::unixtime::parse(item->Get<HistoryMessageEdited>()->date);
				QString editedTranslation = tr::lng_edited(tr::now);
				editedTranslation = editedTranslation.toUpper().left(1) + editedTranslation.mid(1);
				tooltipText += "\n" + editedTranslation + ": "
					+ editLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
					+ editLocalTime.time().toString("HH:mm:ss");
				result.customTooltip = true;
				result.customTooltipText = tooltipText;
				return result;
			}
			// Uploaded tooltip for time+id area
			if (timeIdRect.contains(point)) {
				const auto uploadLocalTime = ItemDateTime(item);
				QString tooltipText = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocalTime.time().toString("HH:mm:ss");
				const auto msgIdValue = item->fullId().msg;
				if (msgIdValue > 0 && GetEnhancedBool("show_messages_id")) {
					tooltipText += "  ID: " + QString::number(msgIdValue.bare);
				}
				result.customTooltip = true;
				result.customTooltipText = tooltipText;
				return result;
			}

				// Remove conflicting right-anchored views hover area; views belong to the left.

			if (const auto size = _parent->hasBubble() ? std::nullopt : _parent->rightActionSize()) {
				auto fullRight = width();
				auto fullBottom = height();
				auto fastShareLeft = _parent->hasRightLayout()
					? (-size->width() - st::historyFastShareLeft)
					: (fullRight + st::historyFastShareLeft);
				auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
				if (QRect(fastShareLeft, fastShareTop, size->width(), size->height()).contains(point)) {
					result.link = _parent->rightActionLink(point
						- QPoint(fastShareLeft, fastShareTop));
				}
			}
		}
	}
	return result;
}

bool GroupedMedia::toggleSelectionByHandlerClick(
		const ClickHandlerPtr &p) const {
	for (const auto &part : _parts) {
		if (part.content->toggleSelectionByHandlerClick(p)) {
			return true;
		}
	}
	return false;
}

bool GroupedMedia::dragItemByHandler(const ClickHandlerPtr &p) const {
	for (const auto &part : _parts) {
		if (part.content->dragItemByHandler(p)) {
			return true;
		}
	}
	return false;
}

void GroupedMedia::setCaptionSelection(int partIndex, TextSelection selection) {
	if (partIndex >= 0 && partIndex < _parts.size()) {
		// Clear previous selection
		if (_selectedCaptionIndex >= 0 && _selectedCaptionIndex < _parts.size()) {
			_parts[_selectedCaptionIndex]._captionSelection = {};
		}

		// Set new selection
		_selectedCaptionIndex = partIndex;
		_parts[partIndex]._captionSelection = selection;
	}
}

TextSelection GroupedMedia::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	if (_mode == Mode::Column) {
		auto checked = 0;
		for (const auto &part : _parts) {
			const auto modified = ShiftItemSelection(
				part.content->adjustSelection(
					UnshiftItemSelection(selection, checked),
					type),
				checked);
			const auto till = checked + part.content->fullSelectionLength();
			if (selection.from >= checked && selection.from < till) {
				selection.from = modified.from;
			}
			if (selection.to <= till) {
				selection.to = modified.to;
				return selection;
			}
			checked = till;
		}
		return selection;
	} else if (_mode == Mode::Grid) {
		auto offset = 0;
		auto result = selection;
		auto found = false;
		for (auto i = 0; i < _parts.size(); ++i) {
			const auto &part = _parts[i];
			const auto textLen = part.item->originalText().text.size();
			
			if (textLen > 0) {
				const auto partFrom = std::max((int)selection.from, offset);
				const auto partTo = std::min(
					(int)selection.to,
					offset + (int)textLen);
				if (partFrom < partTo
					|| (selection.empty()
						&& selection.from >= offset
						&& selection.from <= offset + textLen)) {
					const auto localFrom = std::clamp(
						(int)selection.from,
						offset,
						offset + (int)textLen);
					const auto localTo = std::clamp(
						(int)selection.to,
						offset,
						offset + (int)textLen);
					auto localSelection = TextSelection(
						(uint16)(localFrom - offset), 
						(uint16)(localTo - offset));
					auto adjusted = part._captionText.adjustSelection(
						localSelection,
						type);
					part._captionSelection = adjusted;

					if (!adjusted.empty()) {
						const auto globalFrom = (uint16)(adjusted.from + offset);
						const auto globalTo = (uint16)(adjusted.to + offset);
						if (found) {
							result.from = std::min(result.from, globalFrom);
							result.to = std::max(result.to, globalTo);
						} else {
							result = TextSelection(globalFrom, globalTo);
							found = true;
						}
					}
				} else {
					part._captionSelection = {};
				}
				offset += textLen;
			} else {
				part._captionSelection = {};
			}
		}
		return result;
	}
	return {};
}

uint16 GroupedMedia::fullSelectionLength() const {
	if (_mode != Mode::Column) {
		// For Grid mode, return the caption text length for the current item
		// This enables text selection for Grid captions
		auto result = 0;
		for (const auto &part : _parts) {
			const auto originalText = part.item->originalText();
			if (!originalText.text.isEmpty()) {
				result += originalText.text.size();
			}
		}
		return result;
	}
	auto result = 0;
	for (const auto &part : _parts) {
		result += part.content->fullSelectionLength();
	}
	return result;
}

bool GroupedMedia::hasTextForCopy() const {
	// Support copy if any part has text, regardless of mode (Grid or Column)
	for (const auto &part : _parts) {
		if (_mode == Mode::Column) {
			if (part.content->hasTextForCopy()) return true;
		} else {
			if (!part.item->originalText().empty()) return true;
		}
	}
	return false;
}

TextWithEntities GroupedMedia::getPartText(FullMsgId partId) const {
	for (const auto &part : _parts) {
		if (part.item->fullId() == partId) {
			return part.item->originalText();
		}
	}
	return TextWithEntities();
}


TextForMimeData GroupedMedia::selectedText(
		TextSelection selection) const {
	if (_mode == Mode::Column) {
		auto result = TextForMimeData();
		for (const auto &part : _parts) {
			auto text = part.content->selectedText(selection);
			if (!text.empty()) {
				if (result.empty()) {
					result = std::move(text);
				} else {
					result.append(u"\n\n"_q).append(std::move(text));
				}
			}
			selection = part.content->skipSelection(selection);
		}
		return result;
	} else if (_mode == Mode::Grid) {
		// New Logic for Grid Selection Copy
		auto result = TextForMimeData();
		int offset = 0;
		for (const auto &part : _parts) {
			const auto originalText = part.item->originalText();
			const int textLen = originalText.text.size();
			
			// Always process FullSelection if requested, even if textLen is 0 (though loop prevents that)
			// But check originalText emptiness explicitly
			if (!originalText.text.isEmpty()) {
				if (!selection.empty()) {
					// Map global selection to local part
					const int localFrom = std::max((int)selection.from, offset);
					const int localTo = std::min((int)selection.to, offset + textLen);
					
					if (localFrom < localTo) {
						int start = localFrom - offset;
						int length = localTo - localFrom;
						
						auto partText = originalText.text.mid(start, length);
						
						if (result.empty()) {
							result = TextForMimeData::Simple(partText);
						} else {
							result.append(u"\n\n"_q).append(TextForMimeData::Simple(partText));
						}
					}
				}
				offset += textLen;
			}
		}
		return result;
	}
	return {};
}

SelectedQuote GroupedMedia::selectedQuote(TextSelection selection) const {
	if (_mode == Mode::Column) {
		for (const auto &part : _parts) {
			const auto next = part.content->skipSelection(selection);
			if (next.to - next.from != selection.to - selection.from) {
				if (!next.empty()) {
					return SelectedQuote();
				}
				auto result = part.content->selectedQuote(selection);
				result.item = part.item;
				return result;
			}
			selection = next;
		}
	} else if (_mode == Mode::Grid) {
		auto offset = 0;
		for (const auto &part : _parts) {
			const auto &text = part.item->originalText();
			const auto length = text.text.size();
			if (length > 0) {
				const auto localFrom = std::max(static_cast<int>(selection.from), offset);
				const auto localTo = std::min(static_cast<int>(selection.to), static_cast<int>(offset + length));
				if (localFrom < localTo) {
					auto localSelection = TextSelection(
						(uint16)(localFrom - offset),
						(uint16)(localTo - offset));
					return Element::FindSelectedQuote(
						part._captionText,
						localSelection,
						part.item);
				}
				offset += length;
			}
		}
	}
	return {};
}

TextSelection GroupedMedia::selectionFromQuote(
		const SelectedQuote &quote) const {
	Expects(quote.item != nullptr);

	if (_mode == Mode::Column) {
		const auto i = ranges::find(_parts, not_null(quote.item), &Part::item);
		if (i == end(_parts)) {
			return {};
		}
		const auto index = int(i - begin(_parts));
		auto result = i->content->selectionFromQuote(quote);
		if (result.empty()) {
			return AddGroupItemSelection({}, index);
		}
		for (auto j = i; j != begin(_parts);) {
			--j;
			result = ShiftItemSelection(
				result,
				j->content->fullSelectionLength());
		}
		return result;
	} else {
		// Mode::Grid
		const auto i = ranges::find(_parts, not_null(quote.item), &Part::item);
		if (i == end(_parts)) {
			return {};
		}
		
		// Find the selection within the item's original text
		auto localResult = Element::FindSelectionFromQuote(
			i->_captionText,
			quote);
			
		const auto len = i->item->originalText().text.size();
		if (localResult.empty()) {
			// Fallback: If exact text match fails but we know the item,
			// select the entire item to ensure the row is highlighted.
			if (len > 0) {
				localResult = TextSelection(0, len);
			} else {
				return {};
			}
		}
		
		// Shift the selection by the accumulated length of previous parts
		auto result = localResult;
		for (auto j = i; j != begin(_parts);) {
			--j;
			const auto prevLen = j->item->originalText().text.size();
			if (prevLen > 0) {
				result = ShiftItemSelection(result, prevLen);
			}
		}
		return result;
	}
}

auto GroupedMedia::getBubbleSelectionIntervals(
		TextSelection selection) const
-> std::vector<Ui::BubbleSelectionInterval> {
	if (_mode != Mode::Column) {
		return {};
	}
	auto result = std::vector<Ui::BubbleSelectionInterval>();
	const auto groupPadding = groupedPadding();

	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];
		if (!IsGroupItemSelection(selection, i)) {
			continue;
		}
		const auto &geometry = part.geometry;
		
		const int topGap = groupPadding.top();
		const int visualTopOffset = topGap;
		
		int selectionTop = geometry.top() + visualTopOffset;

		// Calculate bottom gap to exclude it from selection.
		const int bottomGap = 10;
		int selectionHeight = geometry.height() - bottomGap;

		if (result.empty()
			|| (result.back().top + result.back().height < selectionTop)
			|| (result.back().top > selectionTop + selectionHeight)) {
			result.push_back({ selectionTop, selectionHeight });
		} else {
			auto &last = result.back();
			const auto newTop = std::min(last.top, selectionTop);
			const auto newHeight = std::max(
				last.top + last.height - newTop,
				selectionTop + selectionHeight - newTop);
			last = Ui::BubbleSelectionInterval{ newTop, newHeight };
		}
	}
	
	return result;
}


void GroupedMedia::clickHandlerActiveChanged(
		const ClickHandlerPtr &p,
		bool active) {
	for (const auto &part : _parts) {
		part.content->clickHandlerActiveChanged(p, active);
	}
}

void GroupedMedia::clickHandlerPressedChanged(
		const ClickHandlerPtr &p,
		bool pressed) {
	for (auto i = 0; i < _parts.size(); ++i) {
		auto &part = _parts[i];
		part.content->clickHandlerPressedChanged(p, pressed);
		if (pressed && part.content->dragItemByHandler(p)) {
			// #TODO drag by item from album
			// App::pressedLinkItem(part.view);
		}
	}
}

template <typename DataMediaRange>
bool GroupedMedia::applyGroup(const DataMediaRange &medias) {
	if (validateGroupParts(medias)) {
		return true;
	}

	auto modeChosen = false;
	for (const auto media : medias) {
		const auto mediaMode = DetectMode(media);
		if (!modeChosen) {
			_mode = mediaMode;
			modeChosen = true;
		} else if (mediaMode != _mode) {
			_mode = Mode::Column;
			break;
		}
	}
	for (const auto media : medias) {
		_parts.push_back(Part(_parent, media));
	}
	if (_parts.empty()) {
		return false;
	}

	Ensures(_parts.size() <= kMaxSize);
	return true;
}

template <typename DataMediaRange>
bool GroupedMedia::validateGroupParts(
		const DataMediaRange &medias) const {
	auto i = 0;
	const auto count = _parts.size();
	for (const auto media : medias) {
		if (i >= count || _parts[i].item != media->parent()) {
			return false;
		}
		++i;
	}
	return (i == count);
}

not_null<Media*> GroupedMedia::main() const {
	Expects(!_parts.empty());

	return _parts.back().content.get();
}

void GroupedMedia::hideSpoilers() {
	for (const auto &part : _parts) {
		part.content->hideSpoilers();
	}
}

Storage::SharedMediaTypesMask GroupedMedia::sharedMediaTypes() const {
	return main()->sharedMediaTypes();
}

PhotoData *GroupedMedia::getPhoto() const {
	return main()->getPhoto();
}

DocumentData *GroupedMedia::getDocument() const {
	return main()->getDocument();
}

HistoryMessageEdited *GroupedMedia::displayedEditBadge() const {
	if (_mode == Mode::Column) {
		if (_parts.empty()) {
			return nullptr;
		}
		const auto &part = _parts.front();
		if (!part.item->hideEditedBadge()) {
			if (const auto edited = part.item->Get<HistoryMessageEdited>()) {
				return edited;
			}
		}
		return nullptr;
	}
	for (const auto &part : _parts) {
		if (!part.item->hideEditedBadge()) {
			if (const auto edited = part.item->Get<HistoryMessageEdited>()) {
				return edited;
			}
		}
	}
	return nullptr;
}

void GroupedMedia::updateNeedBubbleState() {
	_needBubble = computeNeedBubble();
}

void GroupedMedia::stopAnimation() {
	for (const auto &part : _parts) {
		part.content->stopAnimation();
	}
}

void GroupedMedia::checkAnimation() {
	for (const auto &part : _parts) {
		part.content->checkAnimation();
	}
}

bool GroupedMedia::hasHeavyPart() const {
	for (const auto &part : _parts) {
		if (!part.cache.isNull() || part.content->hasHeavyPart()) {
			return true;
		}
	}
	return false;
}

void GroupedMedia::unloadHeavyPart() {
	for (const auto &part : _parts) {
		part.content->unloadHeavyPart();
		part.cacheKey = 0;
		part.cache = QPixmap();
	}
}

void GroupedMedia::parentTextUpdated() {
	if (_parent->media() == this) {
		if (_mode == Mode::Column) {
			for (const auto &part : _parts) {
				part.content->parentTextUpdated();
			}
		} else {
			_captionItem = std::nullopt;
		}
	}
}

bool GroupedMedia::needsBubble() const {
	return _needBubble;
}

QPoint GroupedMedia::resolveCustomInfoRightBottom() const {
	// Anchor the default info to the first item's top-right row
	if (_parts.empty()) {
		const auto skipx = (st::msgDateImgDelta + st::msgDateImgPadding.x());
		const auto skipy = (st::msgDateImgDelta + st::msgDateImgPadding.y());
		return QPoint(width() - skipx, skipy);
	}
	const auto groupPad = groupedPadding();
	const auto &first = _parts.front();
	return QPoint(first.geometry.x() + first.geometry.width(), first.geometry.y() + groupPad.top());
}

std::optional<PaidInformation> GroupedMedia::paidInformation() const {
	auto result = PaidInformation();
	for (const auto &part : _parts) {
		++result.messages;
		result.stars += part.item->starsPaid();
	}
	return result;
}

bool GroupedMedia::enforceBubbleWidth() const {
	return _mode == Mode::Grid;
}

int GroupedMedia::contributedMaxMonospaceWidth() const {
	if (_mode != Mode::Column) {
		return 0;
	}
	auto result = 0;
	for (const auto &part : _parts) {
		accumulate_max(
			result,
			part.content->contributedMaxMonospaceWidth());
	}
	return result;
}

bool GroupedMedia::computeNeedBubble() const {
	if (_mode == Mode::Column) {
		return true;
	}
	if (itemForText()) {
		return true;
	}

	// If there's no single caption, we still need a bubble if there are
	// replies, forwards, or other elements that are drawn in the bubble.
	if (const auto item = _parent->data()) {
		if (item->repliesAreComments()
			|| item->externalReply()
			|| item->viaBot()
			|| _parent->displayReply()
			|| _parent->displayForwardedFrom()
			|| _parent->displayFromName()
			|| _parent->displayedTopicButton()
			) {
			return true;
		}
	}
	return false;
}

bool GroupedMedia::needInfoDisplay() const {
	const auto item = _parent->data();
	return (_mode != Mode::Column)
		&& (item->isSending()
			|| item->awaitingVideoProcessing()
			|| item->hasFailed()
			|| _parent->isUnderCursor()
			|| (_parent->delegate()->elementContext() == Context::ChatPreview)
			|| _parent->isLastAndSelfMessage());
}

} // namespace HistoryView