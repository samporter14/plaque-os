#pragma once

#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "ambient/MethodLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

// The step timer's prompts, drawn over the reader's page: a menu of two or three large rows (what Confirm opens on a
// page with a timer, or while one runs), and a pair of side-by-side choices for the questions that matter (stop this
// timer? replace it?).
//
// It is built like MethodRemoveDialog, whose placement it reuses: the pair is laid out by method::placeRemoveDialog, so
// a prompt opened by a finger puts the first, safe choice where the finger already is and the other on the far side,
// and a second touch at the same spot can only pick the safe one. It differs in one way MethodRemoveDialog cannot: both
// choices run something (the menu's choices are all actions, and a pair's safe choice may too), and rows are 72 px,
// above the 64 px the bench needs.
//
// Keys: Up/Down (or Left/Right) move the highlight, Confirm chooses it, Back closes without choosing. The first choice
// is highlighted to begin with. Touch: a tap on a choice chooses it; a tap outside closes. The loop task reads the hit
// table the render task published (the same handshake MethodRemoveDialog and OptionPopup use).
class StepTimerDialog {
 public:
  struct Choice {
    std::string label;
    std::function<void()> action;  // may be empty: the choice just closes the dialog
  };

  // A list of rows, centred. Nothing about it depends on a finger.
  void showMenu(const char* headlineStr, std::vector<Choice> rows) {
    RenderLock lock(RenderLock::Mode::Blocking);
    title.clear();
    headline = headlineStr ? headlineStr : "";
    choices = std::move(rows);
    if (choices.size() > MAX_CHOICES) choices.resize(MAX_CHOICES);
    vertical = true;
    finger = false;
    open();
  }

  // Two side-by-side choices. `first` is the safe one: highlighted, and under the finger when `hasFinger`.
  void showPrompt(const char* titleStr, const char* headlineStr, Choice first, Choice second, const bool hasFinger,
                  const int fingerX, const int fingerY) {
    RenderLock lock(RenderLock::Mode::Blocking);
    title = titleStr ? titleStr : "";
    headline = headlineStr ? headlineStr : "";
    choices.clear();
    choices.push_back(std::move(first));
    choices.push_back(std::move(second));
    vertical = false;
    finger = hasFinger;
    fingerPoint[0] = fingerX;
    fingerPoint[1] = fingerY;
    open();
  }

  bool isActive() const { return active.load(); }

  // Close without choosing (the screen under the dialog is going away).
  void dismiss() {
    RenderLock lock(RenderLock::Mode::Blocking);
    active = false;
  }
  // The same for a caller that already holds the RenderLock (the lock is not recursive: dismiss() would wait for it
  // forever).
  void dismissLocked() { active = false; }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active.load()) return false;
    const int count = static_cast<int>(choices.size());

    const freeink::ui::InputSnapshot snap = touchSnapshotFrom(input);
    if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
      // Hit rects are registered on the render task; only route once the first render after show has published them.
      if (uiReady) {
        const freeink::ui::ActionEvent event = interactions.routePublished(snap);
        if (event && event.action == ACTION_OPTION) {
          choose(event.value, requestUpdate);
          return true;
        }
        if (event && event.action == ACTION_CHROME) return true;  // the dialog's own padding keeps it open
        if (snap.touchReleased && snap.touchX >= 0) {
          // Released outside the dialog: close. Swipe-end releases arrive at -1,-1 and fall through.
          active = false;
          requestUpdate();
          return true;
        }
        if (snap.touchPressed) {
          const int16_t idx = interactions.activeIndex();
          if (idx >= 0) {
            const freeink::ui::Interaction& hit = interactions.publishedData()[idx];
            if (hit.action == ACTION_OPTION && highlighted.load() != hit.value) {
              highlighted = hit.value;
              requestUpdate();
            }
          }
        }
      }
      return true;
    }

    if (count > 0 && (input.wasPressed(MappedInputManager::Button::NavPrevious))) {
      highlighted = (highlighted.load() - 1 + count) % count;
      requestUpdate();
      return true;
    }
    if (count > 0 && input.wasPressed(MappedInputManager::Button::NavNext)) {
      highlighted = (highlighted.load() + 1) % count;
      requestUpdate();
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      choose(highlighted.load(), requestUpdate);
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      active = false;
      requestUpdate();
      return true;
    }
    return true;
  }

  // Draws the dialog over the current frame. True while the dialog owns the screen.
  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active.load()) return false;
    const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

 private:
  static constexpr size_t MAX_CHOICES = 3;
  static constexpr freeink::ui::ActionId ACTION_OPTION = 1;
  static constexpr freeink::ui::ActionId ACTION_CHROME = 2;
  static constexpr size_t INTERACTION_CAPACITY = MAX_CHOICES + 1;  // the choices plus the chrome guard
  static constexpr int BUTTON_HEIGHT = 72;                         // above method::MIN_ROW_HEIGHT_PX

  void open() {
    highlighted = 0;
    uiReady = false;
    active = !choices.empty();
  }

  // The dialog closes first, so an action may open the next one (a menu row that asks "stop it?").
  void choose(const int index, const std::function<void()>& requestUpdate) {
    std::function<void()> action;
    if (index >= 0 && index < static_cast<int>(choices.size())) action = choices[static_cast<size_t>(index)].action;
    active = false;
    if (action) action();
    requestUpdate();
  }

  void render(const GfxRenderer& renderer) const {
    namespace fui = freeink::ui;
    fui::GfxRendererTarget target = makeUiTarget(renderer);
    const fui::ThemeTokens& theme = refreshSharedUiThemeTokens(target);
    const fui::DeviceContext device = target.deviceContext();
    const fui::InputSnapshot noInput{};
    interactions.beginPublishCycle();
    fui::Frame<INTERACTION_CAPACITY> frame(target, device, noInput, interactions);
    const auto& metrics = UITheme::getInstance().getMetrics();

    fui::OptionDialogProps props;
    props.title = title.empty() ? nullptr : title.c_str();
    props.headline = headline.empty() ? nullptr : headline.c_str();
    props.verticalOptions = vertical;
    props.inputMask = fui::InputTouch;  // physical keys go through handleInput
    props.titleText.font = fui::GfxRendererTarget::FONT_BODY;
    props.titleText.bold = true;
    props.titleText.align = fui::TextAlign::Center;
    props.titleText.maxLines = 2;
    props.headlineText.font = fui::GfxRendererTarget::FONT_BODY;
    props.headlineText.align = fui::TextAlign::Center;
    props.headlineText.maxLines = 3;
    props.buttonText.font = fui::GfxRendererTarget::FONT_BODY;
    const int16_t inner = static_cast<int16_t>(metrics.optionPopupInnerPadding);
    props.padding = fui::Insets{inner, inner, inner, inner};
    props.gap = static_cast<int16_t>(metrics.optionPopupItemSpacing);
    props.buttonHeight = static_cast<int16_t>(std::max(BUTTON_HEIGHT, method::MIN_ROW_HEIGHT_PX));
    if (theme.listSelectionStyle == fui::SelectionStyle::InvertFill && theme.listRowRadius > 0) {
      props.buttonStyles = fui::defaultButtonStyles();
      props.buttonStyles.focused = props.buttonStyles.selected;
      fui::setStyleRadius(props.buttonStyles, theme.listRowRadius);
    }
    props.styles = fui::defaultPopupStyles();
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
    props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
    props.styles.selected = props.styles.normal;
    props.styles.focused = props.styles.normal;
    props.styles.active = props.styles.normal;
    props.styles.disabled = props.styles.normal;

    const fui::Rect screen = device.screen();
    const int margin = metrics.optionPopupDialogSideMargin > 0 ? metrics.optionPopupDialogSideMargin : 16;
    const int16_t width = fui::clampI16(std::min<int>(screen.width * 5 / 6, screen.width - 2 * margin));
    const int16_t height = fui::clampI16(fui::optionDialogHeight(target, props, width), 0, screen.height);

    fui::DialogOption options[MAX_CHOICES];
    const int count = static_cast<int>(std::min(choices.size(), MAX_CHOICES));
    fui::Rect dialogRect = fui::centeredRect(screen, fui::Size{width, height});
    int firstSlot = 0;  // the slot, left to right, the first choice is drawn in
    if (!vertical) {
      // The pair: the first choice goes where the finger is (method::placeRemoveDialog calls it "Keep").
      method::RemoveDialogSpec spec;
      spec.screenW = screen.width;
      spec.screenH = screen.height;
      spec.dialogW = width;
      spec.dialogH = height;
      spec.padLeft = props.padding.left;
      spec.padRight = props.padding.right;
      spec.padBottom = props.padding.bottom;
      spec.buttonH = props.buttonHeight;
      spec.gap = props.gap;
      spec.margin = margin;
      const method::RemoveDialogPlacement place =
          method::placeRemoveDialog(spec, finger, fingerPoint[0], fingerPoint[1]);
      firstSlot = place.keepFirst ? 0 : 1;
      dialogRect = fui::Rect{static_cast<int16_t>(place.dialog.x), static_cast<int16_t>(place.dialog.y),
                             static_cast<int16_t>(place.dialog.w), static_cast<int16_t>(place.dialog.h)};
    }
    for (int i = 0; i < count; ++i) {
      const int slot = vertical ? i : (i == 0 ? firstSlot : 1 - firstSlot);
      options[slot].label = choices[static_cast<size_t>(i)].label.c_str();
      options[slot].value = static_cast<int16_t>(i);
      options[slot].action = ACTION_OPTION;
      options[slot].state = highlighted.load() == i ? fui::StateFocused : fui::StateNormal;
    }
    props.options = options;
    props.optionCount = static_cast<decltype(props.optionCount)>(count);

    // Chrome guard first, choices after: route() scans newest-first, so the buttons win inside the dialog.
    frame.hit(dialogRect, ACTION_CHROME, 0, fui::InputTouch);
    fui::optionDialog(frame, dialogRect, props);
    interactions.publish();
    uiReady = true;
  }

  std::atomic<bool> active{false};  // written by the loop task, read by the render task
  bool vertical = false;
  bool finger = false;
  int fingerPoint[2] = {0, 0};
  std::string title;
  std::string headline;
  std::vector<Choice> choices;
  std::atomic<int> highlighted{0};
  mutable freeink::ui::InteractionBuffer<INTERACTION_CAPACITY> interactions;
  mutable std::atomic<bool> uiReady{false};
};
