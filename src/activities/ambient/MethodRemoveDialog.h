#pragma once

#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "ambient/MethodLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

// "Remove from this reader?" with two separate, side-by-side choices: Keep and Remove. Built like
// OptionPopup (the same interaction table, hit-tested on the loop task against what the render task drew),
// but laid out by method::placeRemoveDialog so that a long-press puts Keep where the finger already is and
// Remove on the other side: a second touch at the same spot can only choose Keep. Keys: Up/Down switch
// the highlighted choice (Keep to begin with), Confirm chooses it, Back keeps. Tapping outside keeps.
class MethodRemoveDialog {
 public:
  // keepLabel/removeLabel are copied. hasFinger: the long-press position (screen coordinates), when a
  // finger opened the dialog; otherwise the dialog is centred.
  void show(const char* titleStr, const char* headlineStr, const char* keepLabel, const char* removeLabel,
            const bool hasFinger, const int fingerX, const int fingerY, std::function<void()> onRemove) {
    title = titleStr ? titleStr : "";
    headline = headlineStr ? headlineStr : "";
    keepText = keepLabel;
    removeText = removeLabel;
    finger = hasFinger;
    fingerPoint[0] = fingerX;
    fingerPoint[1] = fingerY;
    highlighted = KEEP;
    onRemoveCallback = std::move(onRemove);
    uiReady = false;
    active = true;
  }

  bool isActive() const { return active; }

  // Close without choosing (the screen under the dialog is going away).
  void dismiss() {
    active = false;
    onRemoveCallback = nullptr;
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const freeink::ui::InputSnapshot snap = touchSnapshotFrom(input);
    if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
      // Hit rects are registered on the render task; only route once the first render after show() has
      // published them (the same handshake OptionPopup uses).
      if (uiReady) {
        const freeink::ui::ActionEvent event = interactions.routePublished(snap);
        if (event && event.action == ACTION_OPTION) {
          choose(event.value == REMOVE ? REMOVE : KEEP, requestUpdate);
          return true;
        }
        if (event && event.action == ACTION_CHROME) return true;  // the dialog's own padding keeps it open
        if (snap.touchReleased && snap.touchX >= 0) {
          // Released outside the dialog: keep. Swipe-end releases arrive at -1,-1 and fall through.
          active = false;
          requestUpdate();
          return true;
        }
        if (snap.touchPressed) {
          const int16_t idx = interactions.activeIndex();
          if (idx >= 0) {
            const freeink::ui::Interaction& hit = interactions.publishedData()[idx];
            if (hit.action == ACTION_OPTION && highlighted != hit.value) {
              highlighted = hit.value == REMOVE ? REMOVE : KEEP;
              requestUpdate();
            }
          }
        }
      }
      return true;
    }

    if (input.wasPressed(MappedInputManager::Button::NavPrevious) ||
        input.wasPressed(MappedInputManager::Button::NavNext)) {
      highlighted = highlighted == KEEP ? REMOVE : KEEP;
      requestUpdate();
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      choose(highlighted, requestUpdate);
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
    if (!active) return false;
    const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

 private:
  static constexpr int16_t KEEP = 0;
  static constexpr int16_t REMOVE = 1;
  static constexpr freeink::ui::ActionId ACTION_OPTION = 1;
  static constexpr freeink::ui::ActionId ACTION_CHROME = 2;
  static constexpr size_t INTERACTION_CAPACITY = 3;  // two choices plus the chrome guard
  static constexpr int BUTTON_HEIGHT = 72;           // above method::MIN_ROW_HEIGHT_PX

  void choose(const int16_t option, const std::function<void()>& requestUpdate) {
    active = false;
    if (option == REMOVE && onRemoveCallback) onRemoveCallback();
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
    props.verticalOptions = false;
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
    const method::RemoveDialogPlacement place = method::placeRemoveDialog(spec, finger, fingerPoint[0], fingerPoint[1]);

    // Option order follows the placement: Keep first or second. Its hit rect is the one drawn at place.keep.
    fui::DialogOption options[2];
    const int keepSlot = place.keepFirst ? 0 : 1;
    options[keepSlot].label = keepText.c_str();
    options[keepSlot].value = KEEP;
    options[keepSlot].state = highlighted == KEEP ? fui::StateFocused : fui::StateNormal;
    options[1 - keepSlot].label = removeText.c_str();
    options[1 - keepSlot].value = REMOVE;
    options[1 - keepSlot].state = highlighted == REMOVE ? fui::StateFocused : fui::StateNormal;
    for (auto& option : options) option.action = ACTION_OPTION;
    props.options = options;
    props.optionCount = 2;

    const fui::Rect dialogRect{static_cast<int16_t>(place.dialog.x), static_cast<int16_t>(place.dialog.y),
                               static_cast<int16_t>(place.dialog.w), static_cast<int16_t>(place.dialog.h)};
    // Chrome guard first, options after: route() scans newest-first, so the buttons win inside the dialog.
    frame.hit(dialogRect, ACTION_CHROME, 0, fui::InputTouch);
    fui::optionDialog(frame, dialogRect, props);
    interactions.publish();
    uiReady = true;
  }

  bool active = false;
  bool finger = false;
  int fingerPoint[2] = {0, 0};
  std::string title;
  std::string headline;
  std::string keepText;
  std::string removeText;
  int16_t highlighted = KEEP;
  std::function<void()> onRemoveCallback;
  mutable freeink::ui::InteractionBuffer<INTERACTION_CAPACITY> interactions;
  mutable std::atomic<bool> uiReady{false};
};
