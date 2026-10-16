#pragma once

#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "ambient/MethodLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

// A question with two separate, large, side-by-side choices for the Network screens: a safe one (Keep, Back) and the
// action (Remove, Install). Built like MethodRemoveDialog (the same interaction table and placement): when a finger
// opened it, the safe choice sits where the finger already is, so a second touch at the same spot cannot act. Keys:
// Up/Down switch the highlighted choice (the safe one to begin with), Confirm chooses it, Back chooses the safe one.
// A tap outside the dialog chooses the safe one. The owner polls handleInput() for the answer.
class NetChoiceDialog {
 public:
  enum class Choice : uint8_t { None, Safe, Action };

  // The strings are copied. message may hold several lines ("\n").
  void show(const char* titleText, const char* headlineText, const char* messageText, const char* safeLabel,
            const char* actionLabel, const bool hasFinger, const int fingerX, const int fingerY) {
    title = titleText ? titleText : "";
    headline = headlineText ? headlineText : "";
    message = messageText ? messageText : "";
    safeText = safeLabel;
    actionText = actionLabel;
    finger = hasFinger;
    fingerPoint[0] = fingerX;
    fingerPoint[1] = fingerY;
    highlighted = SAFE;
    uiReady = false;
    active = true;
  }

  bool isActive() const { return active; }
  void dismiss() { active = false; }

  // One pass of input while the dialog is up (it consumes all of it). Returns the choice once one is made, and the
  // dialog is closed then. `repaint` is set whenever the screen must be drawn again.
  Choice handleInput(MappedInputManager& input, bool& repaint) {
    repaint = false;
    if (!active) return Choice::None;
    const freeink::ui::InputSnapshot snap = touchSnapshotFrom(input);
    if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
      // Hit rects are published by the render task; route only after the first render since show().
      if (!uiReady) return Choice::None;
      const freeink::ui::ActionEvent event = interactions.routePublished(snap);
      if (event && event.action == ACTION_OPTION) return choose(event.value == ACTION ? ACTION : SAFE, repaint);
      if (event && event.action == ACTION_CHROME) return Choice::None;           // the dialog's own padding
      if (snap.touchReleased && snap.touchX >= 0) return choose(SAFE, repaint);  // released outside the dialog
      if (snap.touchPressed) {
        const int16_t idx = interactions.activeIndex();
        if (idx >= 0) {
          const freeink::ui::Interaction& hit = interactions.publishedData()[idx];
          if (hit.action == ACTION_OPTION && highlighted != hit.value) {
            highlighted = hit.value == ACTION ? ACTION : SAFE;
            repaint = true;
          }
        }
      }
      return Choice::None;
    }
    if (input.wasPressed(MappedInputManager::Button::NavPrevious) ||
        input.wasPressed(MappedInputManager::Button::NavNext)) {
      highlighted = highlighted == SAFE ? ACTION : SAFE;
      repaint = true;
      return Choice::None;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) return choose(highlighted, repaint);
    if (input.wasReleased(MappedInputManager::Button::Back)) return choose(SAFE, repaint);
    return Choice::None;
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
  static constexpr int16_t SAFE = 0;
  static constexpr int16_t ACTION = 1;
  static constexpr freeink::ui::ActionId ACTION_OPTION = 1;
  static constexpr freeink::ui::ActionId ACTION_CHROME = 2;
  static constexpr size_t INTERACTION_CAPACITY = 3;  // two choices plus the chrome guard
  static constexpr int BUTTON_HEIGHT = 72;           // above method::MIN_ROW_HEIGHT_PX (64)

  Choice choose(const int16_t option, bool& repaint) {
    active = false;
    repaint = true;
    return option == ACTION ? Choice::Action : Choice::Safe;
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
    props.message = message.empty() ? nullptr : message.c_str();
    props.verticalOptions = false;
    props.inputMask = fui::InputTouch;  // physical keys go through handleInput
    props.titleText.font = fui::GfxRendererTarget::FONT_BODY;
    props.titleText.bold = true;
    props.titleText.align = fui::TextAlign::Center;
    props.titleText.maxLines = 2;
    props.headlineText.font = fui::GfxRendererTarget::FONT_BODY;
    props.headlineText.align = fui::TextAlign::Center;
    props.headlineText.maxLines = 2;
    props.messageText.font = fui::GfxRendererTarget::FONT_SMALL;
    props.messageText.align = fui::TextAlign::Left;
    props.messageText.maxLines = 14;
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
    // Wider than the Method dialog: the install question lists the profile's details.
    const int16_t width = fui::clampI16(screen.width - 2 * margin);
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

    // Option order follows the placement: the safe choice is drawn where place.keep is.
    fui::DialogOption options[2];
    const int safeSlot = place.keepFirst ? 0 : 1;
    options[safeSlot].label = safeText.c_str();
    options[safeSlot].value = SAFE;
    options[safeSlot].state = highlighted == SAFE ? fui::StateFocused : fui::StateNormal;
    options[1 - safeSlot].label = actionText.c_str();
    options[1 - safeSlot].value = ACTION;
    options[1 - safeSlot].state = highlighted == ACTION ? fui::StateFocused : fui::StateNormal;
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
  std::string message;
  std::string safeText;
  std::string actionText;
  int16_t highlighted = SAFE;
  mutable freeink::ui::InteractionBuffer<INTERACTION_CAPACITY> interactions;
  mutable std::atomic<bool> uiReady{false};
};
