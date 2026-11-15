#pragma once
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
  // A number from Home Assistant in the thermostat's own unit: one that can only be the other unit is converted.
  double normalize(double value) const;
  // A temperature (tenths, the thermostat's unit) as the glass shows it now: "18.5\u00b0C" or "65\u00b0F".
  std::string show(int tenths, char decimal) const;

 private:
  DialTile *tiles_[MAX_TILES]{};
  switch_::Switch *fahrenheit_{};
  int lo_ = 70, hi_ = 250;
  bool native_f_ = false, bar_known_ = false;
  int bar_mood_ = 0, bar_current_ = 0;
};

}  // namespace esphome::guest_thermostat
