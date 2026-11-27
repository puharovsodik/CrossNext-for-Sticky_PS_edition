#pragma once

#include <Xtc.h>

#include <memory>
#include <string>

#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "XtcPageView.h"
#include "XtcScroll.h"

class XtcReaderActivity final : public ReaderActivity {
  std::shared_ptr<Xtc> xtc;
  uint32_t currentPage = 0;
  mutable uint32_t lastSavedPage = UINT32_MAX;
  std::unique_ptr<ReaderToolbarUi> pageSettings;
  ReaderToolbarUi::Model overlayModel;
  int settingsTab = 0, settingsPage = 0;
  void renderPageSettings();

  // Double-tap zoom: a 2x window into the page, panned by swipes. Any page change ends it.
  uint32_t zoomPage = UINT32_MAX;
  int zoomX = 0, zoomY = 0;    // top-left source pixel of the zoomed window
  xtcview::View fitView;       // unzoomed layout of the last rendered page; maps taps to source pixels
  // Every single tap waits this long for a second one, so page-turn taps are delayed by it.
  static constexpr unsigned long DOUBLE_TAP_MS = 300;
  bool tapPending = false;     // a single tap held back until the double-tap window closes
  unsigned long pendingTapAt = 0, pendingTapHeldMs = 0;
  int pendingTapX = 0;
  bool handleZoomInput();

  // Continuous scroll (see scrollLayout): scrollY is the global source row on the top screen row.
  int64_t scrollY = 0;
  mutable uint32_t lastSavedScrollPage = UINT32_MAX, lastSavedScrollRow = 0;
  bool dragging = false;  // a finger is down; scroll input tracks its vertical travel itself
  int dragStartX = 0, dragStartY = 0, dragLastX = 0, dragLastY = 0;
  struct ScrollPage {
    uint32_t page = UINT32_MAX;
    std::unique_ptr<uint8_t[]> data;
  };
  ScrollPage scrollPages[2];  // a screen spans at most two pages, since pages are taller than it
  // Double-tap 2x zoom inside the strip (xtcscroll::Zoom): left source column and top global source row.
  bool scrollZoomed = false;
  int scrollZoomX = 0;
  int64_t scrollZoomY = 0;
  xtcscroll::Strip scrollStrip(int viewHeight) const;
  bool setScrollZoom(int64_t x, int64_t y, int viewHeight);
  void toggleScrollZoom(int x, int viewRow, int viewHeight);
  void pageInsets(int& top, int& right, int& bottom, int& left) const;
  bool scrollLayout(int& top, int& viewHeight) const;
  bool scrollBy(int64_t delta, int viewHeight);
  const uint8_t* scrollPage(uint32_t page, uint32_t keep);
  void renderScroll(int top, int viewHeight);
  bool handleScrollInput(int top, int viewHeight);
  void jumpToPage(uint32_t page);

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage();
  void openChapterSelection();
  void restoreContentsPosition(ContentsPosition position) override;
  ContentsPosition contentsPagePosition() const override { return {0, currentPage}; }
  void renderStatusBarOverlay(GfxRenderer& renderer, StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();

  bool loadBook() override;
  bool saveFinishedPosition() override;
  std::string getBookTitle() const override { return xtc ? xtc->getTitle() : ""; }
  std::string getBookAuthor() const override { return xtc ? xtc->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return xtc ? xtc->getThumbBmpPath() : ""; }
  bool handleFormatInput() override;
  void renderBook() override;
  void applyInitialOrientation() override;

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("XtcReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~XtcReaderActivity() override = default;

  bool pageTurn(bool isForward) override;
  void openTextSettings() override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  ScreenshotInfo getScreenshotInfo() const override;
};
