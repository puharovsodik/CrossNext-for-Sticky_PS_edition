#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "ProgressFile.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "XtcPageView.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "XtcScroll.h"
#include "apps_local/ui/Rotation.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Pages may be written straight into the panel framebuffer only when no strip mode or clip rect needs drawPixel.
bool directFrameBuffer(const GfxRenderer& renderer) {
  uint8_t* const fb = renderer.getFrameBuffer();
  const auto clip = renderer.getClipRect();
  return fb && renderer.getWriteTarget() == fb && clip[0] <= 0 && clip[1] <= 0 &&
         clip[0] + clip[2] >= renderer.getScreenWidth() && clip[1] + clip[3] >= renderer.getScreenHeight();
}
}  // namespace

void XtcReaderActivity::openTextSettings() {
  if (!xtc || isAtEndOfBook() || pageSettings) return;
  RenderLock lock;
  pageSettings = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
  if (!pageSettings) {
    LOG_ERR("XTR", "OOM: page sheet");
    return;
  }
  settingsTab = xtc->getBitDepth() == 1 ? 1 : 3;
  pageSettings->begin();
  renderPageSettings();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
void XtcReaderActivity::renderPageSettings() {
  if (!pageSettings) return;
  static constexpr StrId positions[] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP};
  static constexpr StrId refresh[] = {StrId::STR_PAGES_1,  StrId::STR_PAGES_5,  StrId::STR_PAGES_10,
                                      StrId::STR_PAGES_15, StrId::STR_PAGES_30, StrId::STR_NEVER};
  static_assert(std::size(refresh) == CrossPointSettings::REFRESH_FREQUENCY_COUNT);
  auto& model = overlayModel;
  model = {};
  model.textSheet = true;
  auto& text = model.text;
  text.fixed = true;
  text.statusAvailable = xtc && xtc->getBitDepth() == 1;
  text.tab = settingsTab;
  text.contentsAvailable = !xtc->getChapters().empty();
  text.listContext = this;
  text.listPage = settingsPage;
  text.listCount = settingsTab == 2 ? xtc->getChapters().size() : 4;
  text.listLabel = [](const void* context, int n, char* out, size_t size) {
    const auto* self = static_cast<const XtcReaderActivity*>(context);
    std::snprintf(out, size, "%s",
                  self->settingsTab == 2 ? self->xtc->getChapters()[n].name.c_str()
                  : n == 0               ? tr(STR_REFRESH_FREQ)
                  : n == 1               ? I18N.get(self->completionMenuLabelId())
                  : n == 2               ? tr(STR_ORIENTATION)
                                         : tr(STR_XTC_DOUBLE_TAP_ZOOM));
  };
  text.listCheck = [](const void* context, int n) {
    const auto* self = static_cast<const XtcReaderActivity*>(context);
    (void)self;
    (void)n;
    return -1;
  };
  text.listValue = [](const void* context, int n, char* out, size_t size) {
    static constexpr StrId orientations[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
    static_assert(std::size(orientations) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
    const auto* self = static_cast<const XtcReaderActivity*>(context);
    if (self->settingsTab != 3) return;
    if (n == 0) std::snprintf(out, size, "%s", self->overlayModel.text.refresh);
    if (n == 2)
      std::snprintf(out, size, "%s",
                    I18N.get(orientations[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]));
    if (n == 3) std::snprintf(out, size, "%s", SETTINGS.xtcDoubleTapZoom ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
  };
  text.placement = I18N.get(positions[SETTINGS.xtcStatusBarMode % 3]);
  text.refresh = I18N.get(refresh[SETTINGS.refreshFrequency % CrossPointSettings::REFRESH_FREQUENCY_COUNT]);
  text.percentage = SETTINGS.statusBarBookProgressPercentage;
  text.night = SETTINGS.screenInverted;
  pageSettings->setModel(model);
  pageSettings->render();
}

bool XtcReaderActivity::loadBook() {
  auto loadedXtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
  if (!loadedXtc) {
    LOG_ERR("XTR", "Failed to allocate XTC object");
    return false;
  }
  if (!loadedXtc->load()) {
    LOG_ERR("XTR", "Failed to load XTC");
    return false;
  }
  xtc = std::move(loadedXtc);
  xtc->setupCacheDir();
  loadProgress();
  return true;
}

void XtcReaderActivity::openChapterSelection() {
  if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
    const auto origin = currentPage;
    startActivityForResult(std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
                           [this, origin](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               RenderLock lock;
                               rememberContentsPosition({0, origin});
                               jumpToPage(std::get<PageResult>(result.data).page);
                               requestUpdate();
                             }
                           });
  }
}

bool XtcReaderActivity::handleFormatInput() {
  if (!xtc) {
    return false;
  }

  if (pageSettings) {
    const auto routed = pageSettings->route(mappedInput);
    int swipeStep = 0;
    if (settingsTab == 2 || settingsTab == 3) {
      switch (mappedInput.wasSwipe()) {
        case MappedInputManager::SwipeDir::Left:
          swipeStep = 1;
          break;
        case MappedInputManager::SwipeDir::Right:
          swipeStep = -1;
          break;
        case MappedInputManager::SwipeDir::None:
        case MappedInputManager::SwipeDir::Up:
        case MappedInputManager::SwipeDir::Down:
          break;
      }
    }
    if (routed.event == ReaderToolbarUi::Event::Home || (routed.event == ReaderToolbarUi::Event::Row && routed.value == readertext::GoHome)) {
      if (routed.routed) haptic_feedback::touchAction();
      RenderLock lock;
      pageSettings.reset();
      saveProgress();
      onGoHome();
      return true;
    }
    if (routed.event == ReaderToolbarUi::Event::Dismiss ||
        (!swipeStep && mappedInput.wasReleased(MappedInputManager::Button::Back)) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Down) {
      RenderLock lock;
      pageSettings.reset();
      requestUpdate();
      return true;
    }
    if (routed.event == ReaderToolbarUi::Event::Tool) {
      RenderLock lock;
      settingsTab = std::clamp(routed.value, 1, 3);
      settingsPage = 0;
      pageSettings->begin();
      renderPageSettings();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return true;
    }
    if (routed.event == ReaderToolbarUi::Event::Row || swipeStep) {
      if (!swipeStep && settingsTab == 3 && routed.value == readertext::MorePick + 1) {
        {
          RenderLock lock;
          pageSettings.reset();
        }
        requestFinishBook();
        return true;
      }
      {
        RenderLock lock;
        int control = routed.value;
        if (swipeStep || control == readertext::SheetPrevious || control == readertext::SheetNext) {
          const int count = overlayModel.text.listCount;  // as last rendered for this tab
          const int capacity =
              readertext::sheetRowsPerPage(pageSettings->uiTarget.deviceContext(), true, settingsTab);
          const int step = swipeStep ? swipeStep : (control == readertext::SheetNext ? 1 : -1);
          const int page = std::clamp(settingsPage + step, 0, std::max(0, (count - 1) / capacity));
          if (page == settingsPage) return true;
          settingsPage = page;
          pageSettings->begin();
          renderPageSettings();
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          return true;
        }
        if (settingsTab == 2 && control >= readertext::ChapterPick) {
          const int n = control - readertext::ChapterPick;
          if (n < int(xtc->getChapters().size())) {
            rememberContentsPosition({0, currentPage});
            jumpToPage(xtc->getChapters()[n].startPage);
            pageSettings.reset();
            requestUpdate();
          }
          return true;
        }
        if (settingsTab == 3 && control == readertext::MorePick + 2) {
          // Rotate in place: the page and the open sheet are laid out again for the new screen.
          SETTINGS.orientation = uint8_t(rotation::readerNext(SETTINGS.orientation, SETTINGS.rotationDirection != 0,
                                                              SETTINGS.readerFourOrientations != 0));
          SETTINGS.saveToFile();
          ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
          zoomPage = UINT32_MAX;
          scrollZoomed = tapPending = dragging = false;
          pagesUntilFullRefresh = 1;  // clean the old layout's ghosting, as handleForcedRefresh does
          settingsPage = 0;
          pageSettings->begin();
          requestUpdate();
          return true;
        }
        if (settingsTab == 3 && control == readertext::MorePick + 3) {
          SETTINGS.xtcDoubleTapZoom = !SETTINGS.xtcDoubleTapZoom;
          SETTINGS.saveToFile();
          zoomPage = UINT32_MAX;
          scrollZoomed = tapPending = false;
          requestUpdate();
          return true;
        }
        if (settingsTab == 3 && control >= readertext::MorePick && control < readertext::MorePick + 1)
          control = readertext::Refresh;
        switch (control) {
          case readertext::Placement:
            if (xtc->getBitDepth() != 1) return true;
            SETTINGS.xtcStatusBarMode = (SETTINGS.xtcStatusBarMode + 1) % 3;
            break;
          case readertext::Percentage:
            if (xtc->getBitDepth() != 1) return true;
            SETTINGS.statusBarBookProgressPercentage = !SETTINGS.statusBarBookProgressPercentage;
            if (SETTINGS.statusBarBookProgressPercentage &&
                SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_HIDE)
              SETTINGS.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_BOTTOM;
            break;
          case readertext::Night:
            SETTINGS.screenInverted = !SETTINGS.screenInverted;
            renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
            break;
          case readertext::Refresh:
            SETTINGS.refreshFrequency = (SETTINGS.refreshFrequency + 1) % CrossPointSettings::REFRESH_FREQUENCY_COUNT;
            break;
          default:
            return true;
        }
      }
      SETTINGS.saveToFile();
      requestUpdate();
    }
    return true;
  }

  if (!isAtEndOfBook()) {
    int scrollTop = 0, scrollHeight = 0;
    // With double-tap zoom off, fitted pages skip handleZoomInput: taps turn at once, with no double-tap wait.
    if (scrollLayout(scrollTop, scrollHeight) ? handleScrollInput(scrollTop, scrollHeight)
                                              : SETTINGS.xtcDoubleTapZoom && handleZoomInput())
      return true;
  }

  // Enter text settings (CrossNext menu) on Confirm release or touch menu gesture
  const bool touchMenu = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || touchMenu) {
    if (touchMenu && xtc->hasChapters() && !xtc->getChapters().empty()) haptic_feedback::touchAction();
    openTextSettings();
    return true;
  }
  return false;
}

bool XtcReaderActivity::handleZoomInput() {
  const unsigned long now = millis();
  if (tapPending && now - pendingTapAt > DOUBLE_TAP_MS) {
    tapPending = false;
    const auto turn = ReaderUtils::tapPageTurn(renderer, pendingTapX, false,
                                               SETTINGS.xtcInvertPageDirection && FsHelpers::hasXtcExtension(bookPath));
    applyPageTurn(turn.prev, turn.next, true, pendingTapHeldMs, false);
  }

  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (!tapPending) {
      tapPending = true;
      pendingTapAt = now;
      pendingTapX = x;
      pendingTapHeldMs = gpio.lastTouchHeldMs();
      return true;
    }
    tapPending = false;
    RenderLock lock;
    if (zoomPage == currentPage) {
      zoomPage = UINT32_MAX;
    } else {
      // Centre the 2x window (half the page each way) on the tapped source pixel.
      zoomPage = currentPage;
      zoomX = fitView.sourceX(std::clamp(x - fitView.x, 0, std::max(0, fitView.width - 1))) - fitView.sourceWidth / 4;
      zoomY = fitView.sourceY(std::clamp(y - fitView.y, 0, std::max(0, fitView.height - 1))) - fitView.sourceHeight / 4;
    }
    requestUpdate();
    return true;
  }

  if (zoomPage != currentPage) return false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    RenderLock lock;
    zoomPage = UINT32_MAX;
    requestUpdate();
    return true;
  }
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!mappedInput.decodeSwipe(sx, sy, ex, ey)) return false;
  RenderLock lock;
  // The page follows the finger: at 2x, one screen pixel is half a fitted pixel.
  const int oldX = zoomX, oldY = zoomY;
  zoomX -= int(int64_t(ex - sx) * fitView.sourceWidth / std::max(1, 2 * fitView.width));
  zoomY -= int(int64_t(ey - sy) * fitView.sourceHeight / std::max(1, 2 * fitView.height));
  xtcview::zoom(fitView, zoomX, zoomY);
  if (zoomX != oldX || zoomY != oldY) requestUpdate();
  return true;
}

// Scroll mode. At 1x a vertical drag scrolls at once by its own travel (faster flicks go further), a sideways swipe falls
// through to the normal swipe page turn, and a tap waits DOUBLE_TAP_MS: alone it steps 80% like the normal tap handler
// (pageTurn), doubled it zooms 2x on the tapped point. At 2x a drag in any direction pans 1:1, a single tap does
// nothing, and a double tap or Back returns to 1x.
bool XtcReaderActivity::handleScrollInput(const int top, const int viewHeight) {
  static constexpr int TAP_SLOP_PX = 28;  // InputManager's TOUCH_TAP_SLOP_PX, which is private
  const unsigned long now = millis();
  if (tapPending && now - pendingTapAt > DOUBLE_TAP_MS) {
    tapPending = false;
    if (!scrollZoomed) {
      const auto turn = ReaderUtils::tapPageTurn(
          renderer, pendingTapX, false, SETTINGS.xtcInvertPageDirection && FsHelpers::hasXtcExtension(bookPath));
      applyPageTurn(turn.prev, turn.next, true, pendingTapHeldMs, false);
    }
  }
  // An edge swipe also reads as Back; at 2x it is a pan, so only a real Back leaves zoom.
  if (scrollZoomed && !mappedInput.wasBackGesture() && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    {
      RenderLock lock;
      toggleScrollZoom(0, 0, viewHeight);
    }
    requestUpdate();
    return true;
  }

  int x = 0, y = 0;
  // Only a touch-down seen here starts a drag, so a contact begun or ended under a global edge gesture is ignored.
  if (mappedInput.wasScreenTouchPressed(x, y)) {
    dragging = true;
    dragStartX = dragLastX = x;
    dragStartY = dragLastY = y;
  }
  if (dragging && mappedInput.isScreenTouchHeld(x, y)) {
    dragLastX = x;
    dragLastY = y;
    return false;
  }
  // The release edge lasts one frame; a release consumed elsewhere just drops the drag. A fast flick can lift before
  // its last held sample is seen, so the swipe's own end points (and start, if the touch-down was missed) win.
  const bool releaseEdge = mappedInput.wasScreenTouchReleased();
  int sx = 0, sy = 0, ex = 0, ey = 0;
  const bool swipe = mappedInput.decodeSwipe(sx, sy, ex, ey);  // only ever true on a release frame
  if (swipe) {
    if (!dragging) dragStartX = sx, dragStartY = sy;
    dragLastX = ex;
    dragLastY = ey;
  }
  const bool released = (releaseEdge && dragging) || swipe;
  dragging = false;
  const int dx = dragLastX - dragStartX, dy = dragLastY - dragStartY;
  if (released && (scrollZoomed ? xtcscroll::isPanDrag(dx, dy, TAP_SLOP_PX)
                                : xtcscroll::isScrollDrag(dx, dy, TAP_SLOP_PX))) {
    tapPending = false;  // a tap just before the drag must not also turn the page
    const uint32_t topPage = currentPage;
    bool changed;
    {
      RenderLock lock;
      if (scrollZoomed) {
        const auto zoom = xtcscroll::pan({scrollZoomX, scrollZoomY}, dx, dy, scrollStrip(viewHeight));
        changed = setScrollZoom(zoom.x, zoom.y, viewHeight);
      } else {
        const int travel = -dy;  // finger up moves the content up, so y grows
        changed = scrollBy(int64_t(travel * xtcscroll::flickMultiplier(travel, gpio.lastTouchHeldMs())), viewHeight);
      }
    }
    if (currentPage != topPage) notePageTurn(currentPage > topPage, true);  // stats count pages, not scroll steps
    if (changed) requestUpdate();
    return true;  // consumes the release so the swipe and tap handlers do not also act on it
  }

  // At 2x every other release is swallowed too, so a sideways swipe never reaches the swipe page turn.
  if (!mappedInput.wasScreenTapped(x, y)) return scrollZoomed && releaseEdge;
  if (!SETTINGS.xtcDoubleTapZoom) return false;  // zoom off: the normal tap handler steps 80% at once
  if (!tapPending) {
    tapPending = true;
    pendingTapAt = now;
    pendingTapX = x;
    pendingTapHeldMs = gpio.lastTouchHeldMs();
    return true;
  }
  tapPending = false;
  {
    RenderLock lock;
    toggleScrollZoom(x, std::clamp(y - top, 0, viewHeight - 1), viewHeight);
  }
  requestUpdate();
  return true;
}

void XtcReaderActivity::applyInitialOrientation() { ReaderActivity::applyInitialOrientation(); }

void XtcReaderActivity::renderBook() {
  if (!xtc) {
    return;
  }

  renderPage();
  saveProgress();
  if (pageSettings) {
    renderPageSettings();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const auto sb = SETTINGS.statusBarSpec();
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title = sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(GfxRenderer& renderer, const StatusBarOverlayPosition position) const {
  const auto sb = SETTINGS.statusBarSpec();
  const bool drawBottom = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

void XtcReaderActivity::pageInsets(int& top, int& right, int& bottom, int& left) const {
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  if (xtc->getBitDepth() == 1) {
    const int statusHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusHeight > 0) {
      const auto placement = SETTINGS.statusBarSpec().xtcMode;
      if (placement == CrossPointSettings::XTC_STATUS_BAR_TOP) top += statusHeight + 4;
      if (placement == CrossPointSettings::XTC_STATUS_BAR_BOTTOM) bottom += statusHeight + 4;
    }
  }
}

// Continuous scroll is on for a 1-bit book read in landscape whose pages are exactly the screen width but taller than
// the room left for them (webtoon strips): they are shown 1:1 as one long strip instead of shrunk to fit. Every other
// book and orientation keeps the fitted page and its double-tap zoom.
bool XtcReaderActivity::scrollLayout(int& top, int& viewHeight) const {
  if (!xtc || xtc->getBitDepth() != 1) return false;
  const auto orientation = renderer.getOrientation();
  if (orientation != GfxRenderer::LandscapeClockwise && orientation != GfxRenderer::LandscapeCounterClockwise)
    return false;
  if (renderer.getScreenWidth() != xtc->getPageWidth()) return false;
  int right, bottom, left;
  pageInsets(top, right, bottom, left);
  viewHeight = renderer.getScreenHeight() - top - bottom;
  return viewHeight > 0 && xtc->getPageHeight() > viewHeight;
}

// Moves the scroll offset by delta rows, first following any jump made by page (chapters, skips, reopening), and keeps
// currentPage on the page holding the top row.
bool XtcReaderActivity::scrollBy(const int64_t delta, const int viewHeight) {
  const int pageHeight = xtc->getPageHeight();
  if (scrollY / pageHeight != currentPage) scrollY = int64_t(currentPage) * pageHeight;
  const int64_t old = scrollY;
  scrollY = xtcscroll::clampY(scrollY + delta, xtc->getPageCount(), pageHeight, viewHeight);
  currentPage = uint32_t(scrollY / pageHeight);
  return scrollY != old;
}

xtcscroll::Strip XtcReaderActivity::scrollStrip(const int viewHeight) const {
  return {xtc->getPageWidth(), xtc->getPageHeight(), xtc->getPageCount(), renderer.getScreenWidth(), viewHeight};
}

// Moves the 2x window (clamped and rounded) and keeps the 1x offset, which progress saves, and currentPage on its top
// row.
bool XtcReaderActivity::setScrollZoom(const int64_t x, const int64_t y, const int viewHeight) {
  const auto strip = scrollStrip(viewHeight);
  const auto zoom = xtcscroll::clampZoom(x, y, strip);
  const bool changed = zoom.x != scrollZoomX || zoom.y != scrollZoomY;
  scrollZoomX = zoom.x;
  scrollZoomY = zoom.y;
  scrollY = xtcscroll::clampY(zoom.y, strip.pageCount, strip.pageHeight, viewHeight);
  currentPage = uint32_t(zoom.y / strip.pageHeight);
  return changed;
}

// Double tap: 2x centred on the tapped point of the 1x strip, or back to 1x centred on the zoom window.
void XtcReaderActivity::toggleScrollZoom(const int x, const int viewRow, const int viewHeight) {
  tapPending = false;  // a held-back single tap must not turn the page after the zoom changes
  const auto strip = scrollStrip(viewHeight);
  if (scrollZoomed) {
    scrollZoomed = false;
    scrollY = xtcscroll::unzoomY(scrollZoomY, strip);
    currentPage = uint32_t(scrollY / strip.pageHeight);
    return;
  }
  scrollBy(0, viewHeight);  // follow any page jump before mapping the tap
  const auto zoom = xtcscroll::zoomAt(x, viewRow, scrollY, strip);
  scrollZoomed = true;
  setScrollZoom(zoom.x, zoom.y, viewHeight);
}

// Decoded page from the two-slot cache, loading it over the slot that does not hold `keep`.
const uint8_t* XtcReaderActivity::scrollPage(const uint32_t page, const uint32_t keep) {
  for (auto& slot : scrollPages)
    if (slot.page == page) return slot.data.get();
  auto& slot = scrollPages[0].page == keep ? scrollPages[1] : scrollPages[0];
  const size_t size = size_t((xtc->getPageWidth() + 7) / 8) * xtc->getPageHeight();
  if (!slot.data) slot.data = makeUniqueNoThrow<uint8_t[]>(size);
  slot.page = UINT32_MAX;
  if (!slot.data) {
    LOG_ERR("XTR", "Failed to allocate scroll page buffer (%lu bytes)", size);
    return nullptr;
  }
  if (xtc->loadPage(page, slot.data.get(), size) != size) {  // a short page would leave stale rows in a reused slot
    LOG_ERR("XTR", "Failed to load page %lu: error=%s", page, xtc::errorToString(xtc->getLastError()));
    return nullptr;
  }
  slot.page = page;
  return slot.data.get();
}

void XtcReaderActivity::renderScroll(const int top, const int viewHeight) {
  const int pageWidth = xtc->getPageWidth(), pageHeight = xtc->getPageHeight();
  scrollBy(0, viewHeight);
  if (scrollZoomed) setScrollZoom(scrollZoomX, scrollZoomY, viewHeight);  // re-clamp if the view height changed
  const int64_t y = scrollZoomed ? scrollZoomY : scrollY;
  const int zoomX = scrollZoomed ? scrollZoomX : 0, shift = scrollZoomed ? 1 : 0;  // 2x: source = zoom + screen / 2
  const int rows = scrollZoomed ? xtcscroll::zoomRows(viewHeight) : viewHeight;
  const uint32_t first = uint32_t(y / pageHeight);
  const bool spans = y % pageHeight + rows > pageHeight;
  const uint8_t* pages[2] = {scrollPage(first, spans ? first + 1 : UINT32_MAX), nullptr};
  if (spans) pages[1] = scrollPage(first + 1, first);
  if (!pages[0] || (spans && !pages[1])) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  if (directFrameBuffer(renderer) && pageWidth == renderer.getDisplayWidthBytes() * 8) {
    const bool rotated180 = renderer.getOrientation() == GfxRenderer::LandscapeClockwise;
    if (scrollZoomed)
      xtcscroll::blitZoomRows(pages, pageHeight, zoomX, y, viewHeight, top, renderer.getFrameBuffer(),
                              renderer.getDisplayWidthBytes(), renderer.getDisplayHeight(), rotated180);
    else
      xtcscroll::blitRows(pages, pageHeight, y, viewHeight, top, renderer.getFrameBuffer(),
                          renderer.getDisplayWidthBytes(), renderer.getDisplayHeight(), rotated180);
  } else {
    for (int r = 0; r < viewHeight; ++r) {
      const auto [page, row] = xtcscroll::sourceRow(y, r >> shift, pageHeight);
      for (int x = 0; x < pageWidth; ++x) {
        if (xtcview::pixel(pages[page - first], pageWidth, pageHeight, 1, zoomX + (x >> shift), row))
          renderer.drawPixel(x, top + r, true);
      }
    }
  }

  renderStatusBarOverlay(renderer, SETTINGS.statusBarSpec().xtcMode == CrossPointSettings::XTC_STATUS_BAR_TOP
                                       ? StatusBarOverlayPosition::Top
                                       : StatusBarOverlayPosition::Bottom);
  renderContentsReturn();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  LOG_DBG("XTR", "Rendered scroll row %lu (page %lu/%lu)", uint32_t(y), currentPage + 1, xtc->getPageCount());
  markPageRendered();
}

void XtcReaderActivity::renderPage() {
  int scrollTop = 0, scrollHeight = 0;
  if (scrollLayout(scrollTop, scrollHeight)) {
    renderScroll(scrollTop, scrollHeight);
    return;
  }
  for (auto& slot : scrollPages) slot = {};
  scrollZoomed = false;

  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = static_cast<size_t>(pageWidth) * ((static_cast<size_t>(pageHeight) + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  auto pageBuffer = makeUniqueNoThrow<uint8_t[]>(pageBufferSize);
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer.get(), pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%lu bitDepth=%u error=%s", currentPage, pageBufferSize,
            bitDepth, xtc::errorToString(xtc->getLastError()));
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();

  int top, right, bottom, left;
  pageInsets(top, right, bottom, left);
  fitView = xtcview::fit(pageWidth, pageHeight, renderer.getScreenWidth(), renderer.getScreenHeight(), top, right,
                         bottom, left);
  if (zoomPage != currentPage) zoomPage = UINT32_MAX;  // leaving the page ends zoom, so returning shows it whole
  const auto view =zoomPage == currentPage ? xtcview::zoom(fitView, zoomX, zoomY) : fitView;
  auto getPixelValue = [&](int x, int y) {
    return xtcview::pixel(pageBuffer.get(), pageWidth, pageHeight, bitDepth, view.sourceX(x), view.sourceY(y));
  };

  if (bitDepth == 2) {
    for (int y = 0; y < view.height; ++y) {
      for (int x = 0; x < view.width; ++x) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(view.x + x, view.y + y, true);
        }
      }
    }

    renderContentsReturn();
    if (pagesUntilFullRefresh <= 1) {
      // Periodic ghost cleanup: scrub via the normal path, then run the
      // settle flavor of the grayscale base pass (DTM planes are equal after
      // the display sync, so only the gentle reinforcement cells fire).
      // Combined-base panels (Paper Mono) instead defer the base so the gray
      // planes below join it in one waveform.
      if (renderer.grayscaleCapabilities().base == HalDisplay::GrayscaleBase::Combined) {
        renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        renderer.preconditionGrayscale();
      }
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }

    renderer.clearScreen(0x00);
    for (int y = 0; y < view.height; ++y) {
      for (int x = 0; x < view.width; ++x) {
        if (getPixelValue(x, y) == 1) {
          renderer.drawPixel(view.x + x, view.y + y, false);
        }
      }
    }
    renderContentsReturn(true);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    for (int y = 0; y < view.height; ++y) {
      for (int x = 0; x < view.width; ++x) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {
          renderer.drawPixel(view.x + x, view.y + y, false);
        }
      }
    }
    renderContentsReturn(true);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();

    renderer.clearScreen();
    for (int y = 0; y < view.height; ++y) {
      for (int x = 0; x < view.width; ++x) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(view.x + x, view.y + y, true);
        }
      }
    }

    renderContentsReturn();
    renderer.cleanupGrayscaleWithFrameBuffer();

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    markPageRendered();
    return;
  } else {
    // The per-pixel drawPixel loop took ~510 ms per page on device; blit1 writes the framebuffer bytes directly
    // when no strip mode or clip rect needs drawPixel's handling.
    static_assert(GfxRenderer::Portrait == 0 && GfxRenderer::LandscapeClockwise == 1 &&
                  GfxRenderer::PortraitInverted == 2 && GfxRenderer::LandscapeCounterClockwise == 3);
    if (!directFrameBuffer(renderer) ||
        !xtcview::blit1(pageBuffer.get(), pageWidth, view, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                        renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), renderer.getOrientation())) {
      for (int y = 0; y < view.height; ++y) {
        for (int x = 0; x < view.width; ++x) {
          if (getPixelValue(x, y)) renderer.drawPixel(view.x + x, view.y + y, true);
        }
      }
    }
  }

  if (SETTINGS.statusBarSpec().xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(renderer, StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(renderer, StatusBarOverlayPosition::Bottom);
  }

  renderContentsReturn();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
  markPageRendered();
}

void XtcReaderActivity::restoreContentsPosition(ContentsPosition position) { jumpToPage(position.page); }

// An explicit page jump starts at the top of that page, even when it is the page already on screen.
void XtcReaderActivity::jumpToPage(const uint32_t page) {
  scrollZoomed = false;
  currentPage = page;
  scrollY = int64_t(page) * xtc->getPageHeight();
}

bool XtcReaderActivity::pageTurn(bool isForward) {
  if (!xtc) return false;
  int scrollTop = 0, scrollHeight = 0;
  if (currentPage < xtc->getPageCount() && scrollLayout(scrollTop, scrollHeight)) {
    // Continuous scroll steps 80% of the view; a step past the last screen opens the end-of-book screen as before.
    // While zoomed it steps 80% of the zoom window and stays zoomed.
    RenderLock lock;
    const int step = xtcscroll::step(scrollZoomed ? xtcscroll::zoomRows(scrollHeight) : scrollHeight);
    if (scrollZoomed ? setScrollZoom(scrollZoomX, scrollZoomY + (isForward ? step : -step), scrollHeight)
                     : scrollBy(isForward ? step : -step, scrollHeight))
      return true;
    if (!isForward) return false;
    scrollZoomed = false;
    currentPage = xtc->getPageCount();
    return true;
  }
  if (isForward) {
    if (currentPage < xtc->getPageCount()) {
      currentPage++;
      return true;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
      return true;
    }
  }
  return false;
}

bool XtcReaderActivity::skipPages(int amount) {
  if (!xtc) return false;
  RenderLock lock;
  int newPage = static_cast<int>(currentPage) + amount;
  if (newPage < 0) newPage = 0;
  if (newPage > static_cast<int>(xtc->getPageCount())) newPage = static_cast<int>(xtc->getPageCount());
  if (newPage != static_cast<int>(currentPage)) {
    jumpToPage(static_cast<uint32_t>(newPage));
    return true;
  }
  return false;
}

bool XtcReaderActivity::isAtEndOfBook() const { return xtc && currentPage >= xtc->getPageCount(); }

bool XtcReaderActivity::saveFinishedPosition() {
  if (!xtc || !xtc->getPageCount()) return false;
  const uint32_t page = xtc->getPageCount();
  uint8_t data[4];
  for (int n = 0; n < 4; ++n) data[n] = static_cast<uint8_t>(page >> (8 * n));
  if (!ProgressFile::writeAtomic(xtc->getCachePath(), data, sizeof(data))) return false;
  currentPage = lastSavedPage = page;
  scrollZoomed = false;
  pageSettings.reset();
  return true;
}

void XtcReaderActivity::onReturnFromEndOfBook() {
  scrollZoomed = false;
  if (xtc && xtc->getPageCount() > 0) {
    currentPage = xtc->getPageCount() - 1;
  } else {
    currentPage = 0;
  }
}

// Scroll mode keeps the row within the page in a sidecar, scroll.bin = {page, row} (LE u32 each), next to
// progress.bin. progress.bin stays the 4-byte page every other reader of it expects; the row is used only while the
// sidecar's page matches it, so a stale sidecar (finish, reset, sync) is ignored.
void XtcReaderActivity::saveProgress() const {
  int scrollTop = 0, scrollHeight = 0;
  if (xtc && currentPage < xtc->getPageCount() && scrollLayout(scrollTop, scrollHeight) &&
      scrollY / xtc->getPageHeight() == currentPage) {
    const uint32_t row = uint32_t(scrollY % xtc->getPageHeight());
    if (currentPage != lastSavedScrollPage || row != lastSavedScrollRow) {
      uint8_t data[8];
      for (int n = 0; n < 4; ++n) data[n] = uint8_t(currentPage >> (8 * n)), data[4 + n] = uint8_t(row >> (8 * n));
      HalFile f;
      if (Storage.openFileForWrite("XTC", xtc->getCachePath() + "/scroll.bin", f) && f.write(data, 8) == 8) {
        lastSavedScrollPage = currentPage;
        lastSavedScrollRow = row;
      }
    }
  }
  if (!xtc || currentPage == lastSavedPage) return;
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  if (!ProgressFile::writeAtomic(xtc->getCachePath(), data, sizeof(data))) {
    LOG_ERR("XTC", "Failed to save progress: page %lu", currentPage);
  } else
    lastSavedPage = currentPage;
}

void XtcReaderActivity::loadProgress() {
  if (!xtc) return;
  HalFile f;
  if (Storage.openFileForRead("XTC", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      if (currentPage > xtc->getPageCount() && xtc->getPageCount() > 0) {
        currentPage = xtc->getPageCount() - 1;
      }
      LOG_DBG("XTC", "Loaded progress: page %lu/%lu", currentPage + 1, xtc->getPageCount());
    }
  }
  scrollY = int64_t(currentPage) * xtc->getPageHeight();
  HalFile scroll;
  if (Storage.openFileForRead("XTC", xtc->getCachePath() + "/scroll.bin", scroll)) {
    uint8_t data[8];
    if (scroll.read(data, 8) == 8) {
      const uint32_t page = data[0] | (data[1] << 8) | (data[2] << 16) | (uint32_t(data[3]) << 24);
      const uint32_t row = data[4] | (data[5] << 8) | (data[6] << 16) | (uint32_t(data[7]) << 24);
      if (page == currentPage && row < xtc->getPageHeight()) scrollY += row;
    }
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = isAtEndOfBook() ? 100 : pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
