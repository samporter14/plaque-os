#include "MessagesActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <PlaqueBoard.h>

#include <cstdio>
#include <cstring>

#include "MessageViewActivity.h"
#include "RegisterActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "ambient/AmbientSettings.h"
#include "ambient/RegisterChoice.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
using namespace messagerows;
namespace {
constexpr StrId LABELS[] = {
    StrId::STR_MESSAGE_VIEW,          StrId::STR_MESSAGE_CHECK,    StrId::STR_REGISTER_VIEW,
    StrId::STR_REGISTER_CHECK,        StrId::STR_MESSAGE_URL,      StrId::STR_MESSAGE_DEVICE_ID,
    StrId::STR_MESSAGE_TOKEN,         StrId::STR_MESSAGE_INTERVAL, StrId::STR_MESSAGE_SLEEP_SCREEN,
    StrId::STR_REGISTER_SLEEP_SCREEN, StrId::STR_MESSAGE_POLLING,  StrId::STR_MESSAGE_LOCAL_HTTP};
}  // namespace

MessagesActivity::MessagesActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("Messages", renderer, input), layout(rowsFor(plaque::offersRegister(plaque::board().caps))) {}

void MessagesActivity::onEnter() {
  for (int i = 0; i < ITEM_COUNT; ++i) rows[i].label = I18N.get(LABELS[i]);
  UiListActivity::onEnter();
}

const char* MessagesActivity::headerTitle() const { return tr(STR_MESSAGES); }

void MessagesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const auto& config = AMBIENT_SETTINGS;
  rows[URL].subtitle = config.baseUrl[0] ? config.baseUrl : tr(STR_NOT_SET);
  rows[DEVICE_ID].value = config.deviceId[0] ? config.deviceId : tr(STR_NOT_SET);
  rows[TOKEN].value = config.deviceToken[0] ? tr(STR_MESSAGE_SECRET_SET) : tr(STR_NOT_SET);
  snprintf(interval, sizeof(interval), tr(STR_MESSAGE_MINUTES_FORMAT), config.pollMinutes);
  rows[INTERVAL].value = interval;
  GUI.setCheckboxRow(rows[SLEEP_SCREEN], config.useAsSleepScreen);
  GUI.setCheckboxRow(rows[REGISTER_SLEEP], config.useRegisterScreen);
  // One line under the row when switching Register on switched "Check while sleeping" on with it.
  if (pollingNote && config.useRegisterScreen) {
    snprintf(pollingText, sizeof(pollingText), tr(STR_REGISTER_POLLING_ON), config.pollMinutes);
    rows[REGISTER_SLEEP].subtitle = pollingText;
  } else {
    pollingNote = false;
    rows[REGISTER_SLEEP].subtitle = nullptr;
  }
  GUI.setCheckboxRow(rows[POLLING], config.scheduledPolling);
  GUI.setCheckboxRow(rows[LOCAL_HTTP], config.allowLocalHttp);
  fui::ListProps props;
  // Only the rows this board shows, in order; a row's action value is its position, which activateIndex() maps back.
  for (int position = 0; position < layout.count; ++position) {
    shown[position] = rows[layout.items[position]];
    shown[position].actionValue = position;
  }
  props.items = shown;
  props.count = layout.count;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void MessagesActivity::editField(const int index) {
  const auto& config = AMBIENT_SETTINGS;
  const char* initial = index == URL ? config.baseUrl : index == DEVICE_ID ? config.deviceId : "";
  const size_t length = index == URL         ? sizeof(config.baseUrl) - 1
                        : index == DEVICE_ID ? sizeof(config.deviceId) - 1
                                             : sizeof(config.deviceToken) - 1;
  const auto inputType = index == URL ? InputType::Url : index == TOKEN ? InputType::Password : InputType::Text;
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(LABELS[index]), initial,
                                                           length, inputType);
  if (!keyboard) {
    LOG_ERR("AMBIENT", "OOM: configuration keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, index](const ActivityResult& result) {
    const auto* text = std::get_if<KeyboardResult>(&result.data);
    if (result.isCancelled || !text) return;
    auto& settings = AMBIENT_SETTINGS;
    {
      RenderLock lock;
      char* field = index == URL ? settings.baseUrl : index == DEVICE_ID ? settings.deviceId : settings.deviceToken;
      const size_t size = index == URL         ? sizeof(settings.baseUrl)
                          : index == DEVICE_ID ? sizeof(settings.deviceId)
                                               : sizeof(settings.deviceToken);
      snprintf(field, size, "%s", text->text.c_str());
    }
    if (!settings.saveToFile()) LOG_ERR("AMBIENT", "Failed to save configuration");
    requestUpdate();
  });
}

void MessagesActivity::activateIndex(const int position) {
  app.clearTapFlash();
  if (position < 0 || position >= layout.count) return;
  const int index = layout.items[position];
  if (index == VIEW || index == CHECK) {
    auto activity = makeUniqueNoThrow<MessageViewActivity>(renderer, mappedInput, index == CHECK);
    if (!activity) {
      LOG_ERR("AMBIENT", "OOM: message viewer");
      return;
    }
    startActivityForResult(std::move(activity), nullptr);
    return;
  }
  if (index == REGISTER_VIEW || index == REGISTER_CHECK) {
    auto activity = makeUniqueNoThrow<RegisterActivity>(renderer, mappedInput, index == REGISTER_CHECK);
    if (!activity) {
      LOG_ERR("AMBIENT", "OOM: register view");
      return;
    }
    startActivityForResult(std::move(activity), nullptr);
    return;
  }
  if (index == URL || index == DEVICE_ID || index == TOKEN) {
    editField(index);
    return;
  }
  if (index == INTERVAL) {
    auto activity = makeUniqueNoThrow<IntervalSelectionActivity>(
        renderer, mappedInput, "MessageInterval", StrId::STR_MESSAGE_INTERVAL, AMBIENT_SETTINGS.pollMinutes, 5, 1440, 5,
        60, StrId::STR_MESSAGE_MINUTES_FORMAT);
    if (!activity) {
      LOG_ERR("AMBIENT", "OOM: polling interval");
      return;
    }
    startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
      const auto* value = std::get_if<IntervalResult>(&result.data);
      if (!result.isCancelled && value) {
        {
          RenderLock lock;
          AMBIENT_SETTINGS.pollMinutes = value->value;
        }
        if (!AMBIENT_SETTINGS.saveToFile()) LOG_ERR("AMBIENT", "Failed to save interval");
        requestUpdate();
      }
    });
    return;
  }
  {
    RenderLock lock;
    switch (index) {
      // One sleep screen at a time: the note, or the instrument's schedule. Register with polling off would never
      // refresh, so switching it on switches polling on (reg::toggleRegister), and the row says so.
      case SLEEP_SCREEN: {
        const reg::SleepChoice now =
            reg::toggleMessage({AMBIENT_SETTINGS.useAsSleepScreen, AMBIENT_SETTINGS.useRegisterScreen,
                                AMBIENT_SETTINGS.scheduledPolling, AMBIENT_SETTINGS.pollMinutes});
        AMBIENT_SETTINGS.useAsSleepScreen = now.message;
        AMBIENT_SETTINGS.useRegisterScreen = now.registerScreen;
        break;
      }
      case REGISTER_SLEEP: {
        const reg::Toggled now =
            reg::toggleRegister({AMBIENT_SETTINGS.useAsSleepScreen, AMBIENT_SETTINGS.useRegisterScreen,
                                 AMBIENT_SETTINGS.scheduledPolling, AMBIENT_SETTINGS.pollMinutes});
        AMBIENT_SETTINGS.useAsSleepScreen = now.choice.message;
        AMBIENT_SETTINGS.useRegisterScreen = now.choice.registerScreen;
        AMBIENT_SETTINGS.scheduledPolling = now.choice.polling;
        AMBIENT_SETTINGS.pollMinutes = now.choice.pollMinutes;
        pollingNote = now.pollingTurnedOn;
        break;
      }
      case POLLING:
        AMBIENT_SETTINGS.scheduledPolling = !AMBIENT_SETTINGS.scheduledPolling;
        break;
      case LOCAL_HTTP:
        AMBIENT_SETTINGS.allowLocalHttp = !AMBIENT_SETTINGS.allowLocalHttp;
        break;
      default:
        return;
    }
  }
  if (!AMBIENT_SETTINGS.saveToFile()) LOG_ERR("AMBIENT", "Failed to save configuration");
  requestUpdate();
}
