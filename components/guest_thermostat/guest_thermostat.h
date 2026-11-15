#pragma once
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/components/switch/switch.h"
#include <cmath>
#include <string>

namespace esphome::guest_thermostat {

class DialTile;

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tile type in setup(), and drives the
// waiting of its tiles (the pause after - and +, the wait for Home Assistant) from on_interval, as the plugin rules
// ask: a tile owns no timer of its own.
class GuestThermostat : public Component, public tessera::Plugin {
 public:
  static constexpr int MAX_TILES = 16;
  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void on_interval(uint32_t now_ms) override;
  // A tile made / gone. attach() is false when MAX_TILES are already there: that tile then sends at once.
  bool attach(DialTile *tile);
  void detach(DialTile *tile);

  // The limits a guest may set, in the thermostat's own unit, and that unit (plugin.yaml).
  void set_limits(float lo, float hi) {
    lo_ = (int) std::lround(lo * 10.0f);
    hi_ = (int) std::lround(hi * 10.0f);
  }
  void set_native_fahrenheit(bool f) { native_f_ = f; }
  // A report in the other unit comes from a second source of the same thermostat (Home Assistant's own cloud
  // integration beside a mirrored entity, minutes behind): it is left unread once a report in the own unit was seen.
  void set_ignore_other_unit(bool v) { ignore_other_unit_ = v; }
  bool ignore_other_unit() const { return ignore_other_unit_; }
  // The switch "Thermostat shows Fahrenheit": what the glass shows, whatever the thermostat's own unit.
  void set_fahrenheit_switch(switch_::Switch *s) { fahrenheit_ = s; }
  int lo() const { return lo_; }
  int hi() const { return hi_; }
  bool native_fahrenheit() const { return native_f_; }
  bool shows_fahrenheit() const { return fahrenheit_ != nullptr ? fahrenheit_->state : native_f_; }
  void toggle_unit();
  // What a tile last heard from its thermostat, for the item in the top bar: what it does (a Mood, as a number) and
  // the temperature now (tenths, the thermostat's unit; INT_MIN for none).
  void note(int mood, int current) {
    bar_mood_ = mood;
    bar_current_ = current;
    bar_known_ = true;
  }
  // A target was chosen here (tenths; `was` is what the thermostat said before). For HOLD_MS a report that still names
  // an old target is a late one and counts as the chosen target: a thermostat that Home Assistant hears of from two
  // sides (its cloud, minutes behind, and a second Home Assistant, at once) says the old and the new in turns.
  void chose(const char *entity, int was, int now_value) {
    if (!hold_on_ || std::strcmp(entity, hold_entity_) != 0) hold_olds_ = 0;
    else if (hold_olds_ < 4) hold_old_[hold_olds_++] = hold_new_;   // the target chosen just before is an old one too
    std::snprintf(hold_entity_, sizeof hold_entity_, "%s", entity);
    if (hold_olds_ < 4) hold_old_[hold_olds_++] = was;
    hold_new_ = now_value;
    hold_since_ = millis();
    hold_on_ = true;
  }
  // What to believe of a reported target (tenths): the chosen one while a late report names an old one.
  int trust(const char *entity, int reported) {
    if (!hold_on_ || std::strcmp(entity, hold_entity_) != 0) return reported;
    if ((uint32_t)(millis() - hold_since_) >= HOLD_MS) {
      hold_on_ = false;
      return reported;
    }
    if (std::abs(reported - hold_new_) <= 3) return reported;
    for (int i = 0; i < hold_olds_; ++i)
      if (std::abs(reported - hold_old_[i]) <= 3) return hold_new_;
    hold_on_ = false;   // a third target: somebody chose it elsewhere
    return reported;
  }
  // A number from Home Assistant in the thermostat's own unit: one that can only be the other unit is converted.
  double normalize(double value) const;
  // A temperature (tenths, the thermostat's unit) as the glass shows it now: "18.5\u00b0C" or "65\u00b0F".
  std::string show(int tenths, char decimal) const;

 private:
  DialTile *tiles_[MAX_TILES]{};
  switch_::Switch *fahrenheit_{};
  int lo_ = 70, hi_ = 250;
  bool native_f_ = false, bar_known_ = false, ignore_other_unit_ = false;
  int bar_mood_ = 0, bar_current_ = 0;
  static constexpr uint32_t HOLD_MS = 240000;
  char hold_entity_[96]{};
  int hold_old_[4]{}, hold_olds_ = 0, hold_new_ = 0;
  uint32_t hold_since_ = 0;
  bool hold_on_ = false;
};

}  // namespace esphome::guest_thermostat
