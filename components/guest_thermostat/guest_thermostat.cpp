#include "guest_thermostat.h"
#include "thermo_logic.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace esphome::guest_thermostat {

static const char *const TAG = "guest_thermostat";
using tessera::Font;
namespace ui = tessera::ui;

constexpr uint32_t DEBOUNCE_MS = 3000;  // after the last tap on - or +, or the last drag of the dial: time to change one's mind
constexpr uint32_t CONFIRM_MS = 20000;  // how long Home Assistant has to report the target we sent (a cloud thermostat is slow)
constexpr int NEAR = 3;                 // tenths: a reported target this close to ours is ours (a unit conversion rounds)
constexpr int MAX_MODES = 8;
constexpr int MODE_LEN = 16;
constexpr int NO_VALUE = INT_MIN;
// Tessera's icon set, by codepoint (components/smart_display/tile_icon_names.h): fire, snowflake, power, sleep (zzz).
constexpr uint32_t ICON_FIRE = 0xF0238, ICON_SNOWFLAKE = 0xF0717, ICON_POWER = 0xF0425, ICON_SLEEP = 0xF04B2;

// The icon of a glyph; 0 for none.
static uint32_t glyph_code(Glyph glyph) {
  switch (glyph) {
    case Glyph::FIRE: return ICON_FIRE;
    case Glyph::SNOWFLAKE: return ICON_SNOWFLAKE;
    case Glyph::POWER: return ICON_POWER;
    case Glyph::SLEEP: return ICON_SLEEP;
    default: return 0;
  }
}

// What the climate entity says, as a tile and its card both read it.
struct Reading {
  Range range;
  Mood mood = Mood::UNAVAILABLE;
  int reported = NO_VALUE, current = NO_VALUE;
  char state_word[MODE_LEN]{};
  char modes[MAX_MODES][MODE_LEN]{};
  int modes_n = 0;
  bool own_seen = false;

  static int number_of(JsonVariantConst v, const GuestThermostat *plugin) {
    return v.is<float>() || v.is<int>() ? tenths(plugin->normalize(v.as<double>())) : NO_VALUE;
  }

  // Every number is brought to the thermostat's own unit first (it can arrive in the other one), the range is cut to
  // what a guest may set, and a target within NEAR of a step is that step.
  void read(JsonObjectConst data, const GuestThermostat *plugin) {
    const std::string state = data["state"] | "";
    JsonObjectConst a = data["attributes"];
    const double lo = a["min_temp"] | 7.0, hi = a["max_temp"] | 35.0;
    const bool foreign = plugin->normalize(lo) != lo || plugin->normalize(hi) != hi;
    if (plugin->ignore_other_unit()) {
      if (foreign && own_seen) return;   // the late second source: what the own source said stands
      if (!foreign) own_seen = true;
    }
    const double native_step = plugin->native_fahrenheit() ? 1.0 : 0.5;
    range = make_range(plugin->normalize(lo), plugin->normalize(hi), foreign ? native_step : (a["target_temp_step"] | native_step));
    range.lo = std::max(range.lo, plugin->lo());
    range.hi = std::min(range.hi, plugin->hi());
    if (range.hi - range.lo < range.step) {
      range.lo = plugin->lo();
      range.hi = plugin->hi();
    }
    // The steps count from a whole step, so 7 to 25 in halves is 7.0, 7.5 ... 25.0.
    reported = number_of(a["temperature"], plugin);
    if (reported != NO_VALUE) {
      const int snapped = (int) std::lround((double) reported / range.step) * range.step;
      if (std::abs(snapped - reported) <= NEAR) reported = snapped;
    }
    current = number_of(a["current_temperature"], plugin);
    mood = mood_of(state, a["hvac_action"] | "");
    std::snprintf(state_word, sizeof state_word, "%s", state.c_str());
    modes_n = 0;
    for (JsonVariantConst m : a["hvac_modes"].as<JsonArrayConst>()) {
      const char *mode = m.as<const char *>();
      if (modes_n >= MAX_MODES || !mode) continue;
      std::snprintf(modes[modes_n++], MODE_LEN, "%s", mode);
    }
  }
};

static std::string mood_word(const tessera::Plugin *plugin, Mood mood) {
  switch (mood) {
    case Mood::HEATING: return plugin->text("heating");
    case Mood::COOLING: return plugin->text("cooling");
    case Mood::OFF: return plugin->text("off");
    case Mood::UNAVAILABLE: return plugin->text("unavailable");
    default: return plugin->text("idle");
  }
}

// Red while it heats, the screen's blue while it cools, green while it rests; off and offline keep the usual colours.
static tessera::Tone mood_tone(Mood mood) {
  switch (mood) {
    case Mood::HEATING: return tessera::Tone::ALERT;
    case Mood::COOLING: return tessera::Tone::ACCENT;
    case Mood::IDLE: return tessera::Tone::GOOD;
    default: return tessera::Tone::NORMAL;
  }
}

static char decimal_of(const tessera::Plugin *plugin) {
  const char *separator = plugin->text("decimal");
  return *separator ? *separator : '.';
}

// ---- The tile: a gauge of the room's temperature, what the heating does and its target, and a key for the unit. Nothing
// is set here: a tap anywhere but on that key opens the card with the dial ----
class DialTile : public tessera::Tile {
 public:
  explicit DialTile(GuestThermostat *plugin) : plugin_(plugin) {}
  ~DialTile() override { plugin_->detach(this); }   // LVGL's objects go with the card; nothing else is ours

  void create(const tessera::TileContext &c) override {
    std::snprintf(entity_, sizeof entity_, "%s", c.entity);
    w_ = c.width;
    h_ = c.height;
    tile_ = c.tile;
    decimal_ = decimal_of(plugin_);
    const int gap = ui::px(4);
    const int body_h = ui::line_height(Font::BODY), value_h = ui::line_height(Font::VALUE);
    const int row_h = std::max(body_h, ui::line_height(Font::ICON_SMALL));

    name_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    ui::set_text(name_, c.name);
    lv_obj_set_width(name_, w_);
    lv_obj_set_pos(name_, 0, 0);
    int top = body_h + gap;
    // Under the name: what the heating does and its target ("Heating · Target 21.0°C"), where there is room for it.
    if (h_ >= top + row_h + gap + value_h) {
      status_icon_ = ui::label(c.parent, Font::ICON_SMALL, theme::INK);
      status_ = ui::label(c.parent, Font::BODY, theme::INK);
      status_y_ = top;
      status_h_ = row_h;
      top += row_h + gap;
    }
    // At the bottom: "Tap to adjust", and at its right the key for the unit on a tile two cells wide.
    int bottom = h_;
    const int key_h = ui::mm(9);
    if (h_ - top >= value_h + gap + key_h) {
      bottom = h_ - key_h - gap;
      int hint_w = w_;
      if (c.columns >= 2) {
        key_w_ = std::min(w_ / 3, ui::mm(20));
        key_h_ = key_h;
        key_x_ = w_ - key_w_;
        key_y_ = h_ - key_h_;
        key_ = ui::block(c.parent, theme::TRACK);
        lv_obj_set_size(key_, key_w_, key_h_);
        lv_obj_set_pos(key_, key_x_, key_y_);
        key_label_ = ui::label(key_, Font::BODY, theme::INK);
        lv_obj_set_width(key_label_, key_w_);
        lv_obj_set_style_text_align(key_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(key_label_, 0, (key_h_ - body_h) / 2);
        hint_w = w_ - key_w_ - gap;
      }
      hint_ = ui::label(c.parent, Font::BODY, theme::MUTED);
      ui::set_text(hint_, plugin_->text("tap_adjust"));
      lv_obj_set_width(hint_, hint_w);
      lv_obj_set_style_text_align(hint_, key_ ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(hint_, 0, h_ - key_h + (key_h - body_h) / 2);
    }
    // Between them the gauge: a ring as on the dial that fills to the room's temperature. Where the number fits in the
    // ring it stands there; on a lower tile the ring stands at the left with the flame in it and the number beside it
    // (as the gauge of a sensor); a tile with no room for a ring has the flame and the number.
    const std::string widest = "88.8\xC2\xB0" "C";
    const int room = bottom - top, d = std::min(w_, room);
    const int ring = std::max(ui::px(5), d / 10), pad = ui::px(10);
    const int tw = ui::text_width(widest, Font::VALUE) + ui::px(4), tw2 = ui::text_width(widest, Font::BODY_LARGE) + ui::px(4);
    Font number_font = Font::VALUE;
    int number_x = 0, number_w = w_, arc_x = (w_ - d) / 2, icon_x = 0, icon_w = 0;
    bool with_ring = false, icon_in_ring = false, number_centred = true;
    Font icon_font = Font::ICON_SMALL;
    if (d >= tw + 2 * ring + pad) {
      with_ring = true;                                   // the number in the ring
    } else if (d >= ui::mm(8) && w_ >= d + pad + tw) {
      with_ring = icon_in_ring = true;                    // the ring, and the number beside it
      number_centred = false;
      arc_x = (w_ - (d + pad + tw)) / 2;
      number_x = arc_x + d + pad;
      number_w = tw;
      if (ui::line_height(Font::ICON) + 2 * ring + ui::px(6) <= d) icon_font = Font::ICON;
      icon_x = arc_x;
      icon_w = d;
    } else if (d >= tw2 + 2 * ring + pad) {
      with_ring = true;                                   // a smaller number in the ring
      number_font = Font::BODY_LARGE;
    } else {
      const int iw = ui::text_width(ui::icon(0xF0238), Font::ICON_SMALL) + ui::px(6);
      if (w_ >= iw + tw) {                                // no ring: the flame, and the number
        number_centred = false;
        icon_x = (w_ - (iw + tw)) / 2;
        icon_w = iw;
        number_x = icon_x + iw;
        number_w = tw;
      }
    }
    const int arc_y = top + (room - d) / 2;
    if (with_ring) {
      arc_ = lv_arc_create(c.parent);
      lv_obj_remove_style_all(arc_);
      lv_obj_set_size(arc_, d, d);
      lv_obj_set_pos(arc_, arc_x, arc_y);
      lv_arc_set_rotation(arc_, 135);
      lv_arc_set_bg_angles(arc_, 0, 270);
      lv_arc_set_range(arc_, 0, 100);
      for (lv_part_t part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
        lv_obj_set_style_arc_width(arc_, ring, part);
        lv_obj_set_style_arc_rounded(arc_, true, part);
      }
      lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);   // a gauge only: the finger goes through to the tile
    }
    if (icon_w > 0) {
      gauge_icon_ = ui::label(c.parent, icon_font, theme::INK);
      lv_obj_set_width(gauge_icon_, icon_w);
      lv_obj_set_style_text_align(gauge_icon_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(gauge_icon_, icon_x, top + std::max(0, (room - ui::line_height(icon_font)) / 2));
    }
    center_ = ui::label(c.parent, number_font, theme::INK);
    lv_obj_set_width(center_, number_w);
    lv_obj_set_style_text_align(center_, number_centred ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(center_, number_x, top + std::max(0, (room - ui::line_height(number_font)) / 2));
    plugin_->attach(this);
    apply();
  }

  void on_state(JsonObjectConst data) override {
    reading_.read(data, plugin_);
    raw_ = reading_.reported;
    if (raw_ != NO_VALUE) reading_.reported = plugin_->trust(entity_, raw_);
    plugin_->note(static_cast<int>(reading_.mood), reading_.current);
    apply();
  }

  void on_theme() override {
    painted_ = false;
    apply();
  }

  // A tap on the key changes the unit the glass shows; a tap anywhere else opens the dial.
  void on_tap() override {
    const tessera::Point at = tessera::tap_point();
    const int slack = ui::mm(2);
    if (key_ && at.x >= key_x_ - slack && at.y >= key_y_ - slack) {
      plugin_->toggle_unit();
      apply();
      return;
    }
    if (reading_.mood == Mood::UNAVAILABLE) return;
    tessera::open_card(plugin_->plugin_id(), "dial", entity_, tile_);
  }

  // Called by the plugin every 250 ms. The unit was changed (the key on the card, or the switch in Home Assistant), or
  // a target was chosen on the dial (or the wait after it is over): the tile follows at once.
  void tick(uint32_t) {
    bool changed = plugin_->shows_fahrenheit() != unit_f_;
    if (raw_ != NO_VALUE && plugin_->trust(entity_, raw_) != reading_.reported) {
      reading_.reported = plugin_->trust(entity_, raw_);
      changed = true;
    }
    if (changed) apply();
  }

 private:
  // A label's text, set only when it differs: a tile that is told the same again does not flicker.
  static void put(lv_obj_t *label, std::string &was, const std::string &text) {
    if (!label || was == text) return;
    was = text;
    ui::set_text(label, text);
  }

  // Everything that shows, from the state as it is now. Only what changed is painted.
  void apply() {
    const bool offline = reading_.mood == Mood::UNAVAILABLE;
    unit_f_ = plugin_->shows_fahrenheit();
    const tessera::Tone tone = offline ? tessera::Tone::NORMAL : reading_.mood == Mood::HEATING ? tessera::Tone::ALERT
                                                                 : mood_tone(reading_.mood);
    const int number = reading_.current != NO_VALUE ? reading_.current : reading_.reported;
    put(center_, center_text_, !offline && number != NO_VALUE ? plugin_->show(number, decimal_) : std::string("-"));
    std::string line = mood_word(plugin_, reading_.mood);
    if (!offline && reading_.reported != NO_VALUE)
      line += " \xC2\xB7 " + tessera::fill(plugin_->text("target"), "n", plugin_->show(reading_.reported, decimal_));
    const uint32_t code = offline ? 0 : reading_.mood == Mood::IDLE ? 0xF0238 : glyph_code(glyph_of(reading_.mood));
    const std::string icon = code ? ui::icon(code) : "";
    // A narrow tile has no room for both: the target stays, and the flame's colour says what the heating does.
    if (!offline && reading_.reported != NO_VALUE && !icon.empty() &&
        ui::text_width(line, Font::BODY) > w_ - ui::text_width(icon, Font::ICON_SMALL) - ui::px(8))
      line = tessera::fill(plugin_->text("target"), "n", plugin_->show(reading_.reported, decimal_));
    const bool moved = icon != icon_text_;
    put(status_icon_, icon_text_, icon);
    put(status_, status_text_, line);
    put(gauge_icon_, gauge_icon_text_, icon);
    if (status_ && (moved || !painted_)) {
      const int iw = icon.empty() ? 0 : ui::text_width(icon, Font::ICON_SMALL) + ui::px(6);
      lv_obj_set_width(status_icon_, std::max(1, iw));
      lv_obj_set_pos(status_icon_, 0, status_y_ + (status_h_ - ui::line_height(Font::ICON_SMALL)) / 2);
      lv_obj_set_width(status_, std::max(1, w_ - iw));
      lv_obj_set_pos(status_, iw, status_y_ + (status_h_ - ui::line_height(Font::BODY)) / 2);
    }
    put(key_label_, key_text_, unit_f_ ? "\xC2\xB0" "C" : "\xC2\xB0" "F");   // what a tap changes to
    // The gauge fills from the lowest to the highest a guest may set, to the temperature of the room.
    int fill = 0;
    if (!offline && reading_.current != NO_VALUE && reading_.range.hi > reading_.range.lo)
      fill = std::clamp((reading_.current - reading_.range.lo) * 100 / (reading_.range.hi - reading_.range.lo), 2, 100);
    if (arc_ && (fill != fill_ || !painted_)) lv_arc_set_value(arc_, fill);
    fill_ = fill;
    if (tone != tone_ || offline != offline_ || !painted_) {
      ui::set_tone(center_, tone, offline ? theme::MUTED : theme::INK);
      if (status_) {
        ui::set_tone(status_, tone, theme::MUTED);
        ui::set_tone(status_icon_, tone, theme::MUTED);
      }
      if (gauge_icon_) ui::set_tone(gauge_icon_, tone, theme::MUTED);
      if (arc_) {
        lv_obj_set_style_arc_color(arc_, ui::color(theme::TRACK), LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc_, offline ? ui::color(theme::TRACK) : ui::tone_color(tone, theme::MUTED), LV_PART_INDICATOR);
      }
      ui::set_color(name_, theme::MUTED);
      if (hint_) ui::set_color(hint_, theme::MUTED);
      if (key_label_) ui::set_color(key_label_, theme::INK);
      tone_ = tone;
      offline_ = offline;
    }
    painted_ = true;
  }

  GuestThermostat *plugin_;
  char entity_[96]{};
  Reading reading_;
  int w_ = 0, h_ = 0, tile_ = -1;
  int status_y_ = 0, status_h_ = 0, key_x_ = 0, key_y_ = 0, key_w_ = 0, key_h_ = 0;
  char decimal_ = '.';
  int raw_ = NO_VALUE, fill_ = -1;
  bool unit_f_ = false, painted_ = false, offline_ = false;
  tessera::Tone tone_ = tessera::Tone::NORMAL;
  std::string center_text_, status_text_, icon_text_, key_text_, gauge_icon_text_;
  lv_obj_t *name_{}, *center_{}, *status_{}, *status_icon_{}, *arc_{}, *key_{}, *key_label_{}, *hint_{}, *gauge_icon_{};
};

// ---- The card: the dial, like Home Assistant's thermostat card. A finger drags the ring; the card keeps its drags ----
class DialCard : public tessera::Card {
 public:
  explicit DialCard(GuestThermostat *plugin) : plugin_(plugin) {}
  // Closed while a change still waits: it is sent now, not lost.
  ~DialCard() override {
    if (debounce_on_) send(shown_);
  }

  void open(const tessera::CardContext &c) override {
    std::snprintf(entity_, sizeof entity_, "%s", c.entity);
    decimal_ = decimal_of(plugin_);
    // The ring stays this far from the card's edge; the knob reaches a little beyond the ring.
    const int margin = ui::mm(3) + ui::px(6);
    const int key_w = ui::mm(24), key_h = ui::mm(10), gap = ui::px(8);
    d_ = std::max(1, std::min(c.width, c.height) - 2 * margin);
    const bool beside = (c.width - d_) / 2 >= key_w + gap;
    if (!beside) d_ = std::max(1, std::min(c.width - 2 * margin, c.height - key_h - gap - 2 * margin));
    x_ = (c.width - d_) / 2;
    y_ = beside ? (c.height - d_) / 2 : std::max(0, (c.height - key_h - gap - d_) / 2);
    unit_w_ = key_w;
    unit_h_ = key_h;
    unit_x_ = c.width - key_w;
    unit_y_ = c.height - key_h;
    // The ring's width: thick, but never more than a sixth of the dial.
    ring_ = std::min(std::max(ui::mm(5), d_ / 9), d_ / 6);
    arc_ = lv_arc_create(c.parent);
    lv_obj_remove_style_all(arc_);
    lv_obj_set_size(arc_, d_, d_);
    lv_obj_set_pos(arc_, x_, y_);
    lv_arc_set_rotation(arc_, 135);
    lv_arc_set_bg_angles(arc_, 0, 270);
    lv_arc_set_range(arc_, 0, 56);
    for (lv_part_t part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
      lv_obj_set_style_arc_width(arc_, ring_, part);
      lv_obj_set_style_arc_rounded(arc_, true, part);
    }
    lv_obj_set_style_bg_opa(arc_, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(arc_, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc_, ui::px(2), LV_PART_KNOB);
    lv_obj_add_event_cb(arc_, &DialCard::arc_event, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(arc_, &DialCard::arc_event, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(arc_, &DialCard::arc_event, LV_EVENT_PRESS_LOST, this);
    make_middle(c.parent);
    // The key that shows the other unit: Celsius unless a guest (or Home Assistant) chose Fahrenheit.
    unit_block_ = ui::block(c.parent, theme::TRACK);
    lv_obj_set_pos(unit_block_, unit_x_, unit_y_);
    lv_obj_set_size(unit_block_, unit_w_, unit_h_);
    unit_label_ = ui::label(unit_block_, Font::BODY_LARGE, theme::INK);
    lv_obj_set_width(unit_label_, unit_w_);
    lv_obj_set_style_text_align(unit_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(unit_label_, 0, (unit_h_ - ui::line_height(Font::BODY_LARGE)) / 2);
    // What the thermostat does, in its colour, opposite the unit's key: red Heating, green Idle.
    status_block_ = ui::block(c.parent, theme::TRACK);
    lv_obj_set_pos(status_block_, 0, unit_y_);
    lv_obj_set_size(status_block_, unit_w_, unit_h_);
    status_icon_ = ui::label(status_block_, Font::ICON_SMALL, theme::INK);
    status_label_ = ui::label(status_block_, Font::BODY, theme::INK);
    if (c.width < 2 * unit_w_ + gap) lv_obj_add_flag(status_block_, LV_OBJ_FLAG_HIDDEN);
    // - and + beside the dial where the card is wide: a touch steps once, a finger held on one keeps stepping.
    if (beside) {
      const int step_h = std::max(key_h, std::min(ui::mm(30), unit_y_ - 2 * gap));
      const int step_y = std::max(0, (unit_y_ - gap - step_h) / 2);
      for (int i = 0; i < 2; ++i) {
        step_[i] = ui::block(c.parent, theme::TRACK);
        lv_obj_set_size(step_[i], key_w, step_h);
        lv_obj_set_pos(step_[i], i ? c.width - key_w : 0, step_y);
        lv_obj_add_flag(step_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(step_[i], &DialCard::step_event, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(step_[i], &DialCard::step_event, LV_EVENT_LONG_PRESSED_REPEAT, this);
        step_label_[i] = ui::label(step_[i], Font::VALUE, theme::INK);
        lv_obj_set_width(step_label_[i], key_w);
        lv_obj_set_style_text_align(step_label_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(step_label_[i], 0, (step_h - ui::line_height(Font::VALUE)) / 2);
        ui::set_text(step_label_[i], i ? "+" : "-");
      }
    }
  }

  // A tap on the unit's key changes what the glass shows, here and on the tiles; the thermostat is not touched.
  void on_tap() override {
    const tessera::Point at = tessera::tap_point();
    const int slack = ui::mm(1);
    if (at.x < unit_x_ - slack || at.y < unit_y_ - slack || at.x >= unit_x_ + unit_w_ + slack || at.y >= unit_y_ + unit_h_ + slack) return;
    plugin_->toggle_unit();
    apply();
  }

  void on_state(JsonObjectConst data) override {
    reading_.read(data, plugin_);
    raw_ = reading_.reported;
    if (raw_ != NO_VALUE) reading_.reported = plugin_->trust(entity_, raw_);
    if (pending_active_ && reading_.reported != NO_VALUE && std::abs(reading_.reported - pending_) <= NEAR) settle();   // Home Assistant brought the target we chose
    if (!dragging_ && !pending_active_) shown_ = reading_.reported;
    if (!dragging_) apply();                                            // under a finger the dial does not move
  }

  void on_tick(uint32_t) override {
    // The dial was let go DEBOUNCE_MS ago and not touched since: now Home Assistant hears of it, once.
    if (debounce_on_ && !dragging_ && expired(millis(), debounce_since_, DEBOUNCE_MS)) {
      debounce_on_ = false;
      send(shown_);
      apply();
    }
    if (confirm_on_ && expired(millis(), confirm_since_, CONFIRM_MS)) {
      settle();                          // no answer yet: the wait ends, the chosen target stays until HOLD_MS
      shown_ = reading_.reported;
      if (!dragging_) apply();
    }
    // A late report was set aside for a while: when that while is over, what Home Assistant says counts again.
    if (!pending_active_ && !debounce_on_ && raw_ != NO_VALUE && plugin_->trust(entity_, raw_) != reading_.reported) {
      reading_.reported = plugin_->trust(entity_, raw_);
      shown_ = reading_.reported;
      if (!dragging_) apply();
    }
  }

  void on_theme() override { apply(); }

 private:
  // What stands in the middle of the ring, from the top: the target, the temperature now, the state with its icon.
  // The first of these that fits inside the ring (each line within the chord of the inner circle where it stands):
  // everything with the large digits, then smaller faces, then without the state, then without the temperature now.
  void make_middle(lv_obj_t *parent) {
    struct Pass { Font face, now_face; bool now, state; };
    static const Pass passes[] = {
        {Font::DIGITS, Font::BODY_LARGE, true, true}, {Font::VALUE, Font::BODY_LARGE, true, true},
        {Font::VALUE, Font::BODY, true, true},        {Font::VALUE, Font::BODY_LARGE, true, false},
        {Font::VALUE, Font::BODY, true, false},       {Font::VALUE, Font::BODY, false, false}};
    const int gap = ui::px(4);
    const int hs = std::max(ui::line_height(Font::BODY), ui::line_height(Font::ICON_SMALL));
    const int cy = y_ + d_ / 2;
    int widest = 0;   // the widest state word, with its icon
    for (const char *key : {"heating", "cooling", "idle", "off", "unavailable"})
      widest = std::max(widest, ui::text_width(plugin_->text(key), Font::BODY));
    widest += ui::px(6) + ui::text_width(ui::icon(ICON_FIRE), Font::ICON_SMALL);
    const std::string sample = tessera::fill(plugin_->text("now"), "n", "88.8\xC2\xB0" "C");
    for (const Pass &pass : passes) {
      const int hd = ui::line_height(pass.face), hn = ui::line_height(pass.now_face);
      const int total = hd + (pass.now ? gap + hn : 0) + (pass.state ? gap + hs : 0);
      int y = cy - total / 2;
      bool fits = inside(y, hd, ui::text_width("88.8\xC2\xB0" "C", pass.face));
      y += hd + gap;
      if (pass.now) {
        fits = fits && inside(y, hn, ui::text_width(sample, pass.now_face));
        y += hn + gap;
      }
      if (pass.state) fits = fits && inside(y, hs, widest);
      if (fits || &pass == &passes[5]) {
        face_ = pass.face;
        now_face_ = pass.now_face;
        show_now_ = pass.now;
        show_state_ = pass.state;
        break;
      }
    }
    const int hd = ui::line_height(face_), hn = ui::line_height(now_face_);
    const int total = hd + (show_now_ ? gap + hn : 0) + (show_state_ ? gap + hs : 0);
    int y = cy - total / 2;
    center_ = ui::label(parent, face_, theme::INK);
    centre(center_, y);
    y += hd + gap;
    if (show_now_) {
      now_ = ui::label(parent, now_face_, theme::MUTED);
      centre(now_, y);
      y += hn + gap;
    }
    if (show_state_) {
      row_y_ = y;
      row_h_ = hs;
      glyph_ = ui::label(parent, Font::ICON_SMALL, theme::INK);
      state_ = ui::label(parent, Font::BODY, theme::MUTED);
    }
  }

  // Whether a line of `width` pixels, from y `top` and `h` high, stays inside the ring's inner circle: its widest chord
  // is at the line's edge farthest from the centre.
  bool inside(int top, int h, int width) const {
    const int cy = y_ + d_ / 2;
    const int radius = d_ / 2 - ring_ - ui::px(3);
    const int far = std::max(std::abs(top - cy), std::abs(top + h - cy));
    if (far >= radius) return false;
    const int half = (int) std::sqrt((double) radius * radius - (double) far * far);
    return width <= 2 * half;
  }

  // A label across the ring's width, its text centred.
  void centre(lv_obj_t *label, int y) {
    lv_obj_set_width(label, d_);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, x_, y);
  }

  // Everything that shows, from the state as it is now.
  void apply() {
    const bool offline = reading_.mood == Mood::UNAVAILABLE;
    const bool has_value = shown_ != NO_VALUE && !offline;
    ui::set_text(center_, has_value ? plugin_->show(shown_, decimal_) : std::string("-"));
    ui::set_tone(center_, pending_active_ && !offline ? tessera::Tone::BUSY : tessera::Tone::NORMAL, offline ? theme::MUTED : theme::INK);
    if (now_) {
      ui::set_text(now_, reading_.current != NO_VALUE && !offline
                             ? tessera::fill(plugin_->text("now"), "n", plugin_->show(reading_.current, decimal_))
                             : "");
      ui::set_color(now_, theme::MUTED);
    }
    if (state_) place_state();
    // The key names what a tap changes to.
    ui::set_text(unit_label_, plugin_->text(plugin_->shows_fahrenheit() ? "show_c" : "show_f"));
    ui::set_color(unit_label_, theme::INK);
    for (lv_obj_t *label : step_label_)
      if (label) ui::set_color(label, offline ? theme::MUTED : theme::INK);
    const tessera::Tone status_tone = offline ? tessera::Tone::NORMAL : mood_tone(reading_.mood);
    ui::set_fill(status_block_, status_tone, theme::TRACK);
    // Its icon (a flame, the resting zzz, a snowflake, the power sign) and its word, the two centred in the block.
    const std::string status_word = mood_word(plugin_, reading_.mood);
    const uint32_t status_code = offline ? 0 : glyph_code(glyph_of(reading_.mood));
    const std::string status_icon = status_code ? ui::icon(status_code) : "";
    const theme::Role status_ink = status_tone != tessera::Tone::NORMAL ? theme::ON_ACCENT : theme::INK;
    ui::set_text(status_icon_, status_icon);
    ui::set_text(status_label_, status_word);
    ui::set_color(status_icon_, status_ink);
    ui::set_color(status_label_, status_ink);
    const int siw = status_icon.empty() ? 0 : ui::text_width(status_icon, Font::ICON_SMALL);
    const int sww = ui::text_width(status_word, Font::BODY), sgap = siw ? ui::px(6) : 0;
    const int sx = std::max(0, (unit_w_ - (siw + sgap + sww)) / 2);
    lv_obj_set_width(status_icon_, std::max(1, siw));
    lv_obj_set_pos(status_icon_, sx, (unit_h_ - ui::line_height(Font::ICON_SMALL)) / 2);
    lv_obj_set_width(status_label_, sww + ui::px(2));
    lv_obj_set_pos(status_label_, sx + siw + sgap, (unit_h_ - ui::line_height(Font::BODY)) / 2);
    lv_arc_set_range(arc_, 0, steps(reading_.range));
    lv_arc_set_value(arc_, has_value ? to_step(shown_, reading_.range) : 0);
    const bool quiet = offline || reading_.mood == Mood::OFF;
    lv_obj_set_style_arc_color(arc_, ui::color(theme::TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_, quiet ? ui::color(theme::TRACK) : ui::tone_color(mood_tone(reading_.mood), theme::MUTED),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(arc_, quiet ? ui::color(theme::MUTED) : ui::color(theme::INK), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc_, offline ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_KNOB);
    if (offline) {
      lv_obj_add_state(arc_, LV_STATE_DISABLED);
      lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
    } else {
      lv_obj_remove_state(arc_, LV_STATE_DISABLED);
      lv_obj_add_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
    }
  }

  // The state's icon and word as one row, centred in the ring.
  void place_state() {
    const std::string word = mood_word(plugin_, reading_.mood);
    const Glyph glyph = glyph_of(reading_.mood);
    const uint32_t code = glyph_code(glyph);
    const std::string icon = code ? ui::icon(code) : "";
    ui::set_text(glyph_, icon);
    ui::set_text(state_, word);
    ui::set_color(glyph_, theme::INK);
    ui::set_color(state_, theme::MUTED);
    const int gap = icon.empty() ? 0 : ui::px(6);
    const int iw = icon.empty() ? 0 : ui::text_width(icon, Font::ICON_SMALL);
    const int tw = ui::text_width(word, Font::BODY);
    const int x = x_ + std::max(0, (d_ - (iw + gap + tw)) / 2);
    lv_obj_set_width(glyph_, std::max(1, iw));
    lv_obj_set_pos(glyph_, x, row_y_ + (row_h_ - ui::line_height(Font::ICON_SMALL)) / 2);
    lv_obj_set_width(state_, tw + ui::px(2));
    lv_obj_set_pos(state_, x + iw + gap, row_y_ + (row_h_ - ui::line_height(Font::BODY)) / 2);
  }

  void send(int value) {
    if (value == NO_VALUE || value == reading_.reported) {   // already there: nothing to tell Home Assistant
      settle();
      return;
    }
    pending_ = value;
    pending_active_ = true;
    if (raw_ != NO_VALUE) plugin_->chose(entity_, raw_, value);
    reading_.reported = value;   // believed until Home Assistant names a third
    JsonDocument fields;
    fields["temperature"] = value / 10.0;   // tenths of a degree, sent as a number
    tessera::action("climate.set_temperature", entity_, fields);
    confirm_on_ = true;
    confirm_since_ = millis();
  }

  void settle() {
    pending_active_ = false;
    confirm_on_ = false;
  }

  // One step down or up, as the tile's keys: only the number moves, and Home Assistant hears DEBOUNCE_MS after the
  // last step. A held key repeats four times a second.
  void bump(int direction) {
    if (reading_.mood == Mood::UNAVAILABLE) return;
    const int base = shown_ != NO_VALUE ? shown_ : reading_.reported;
    if (base == NO_VALUE) return;
    shown_ = from_step(to_step(base, reading_.range) + direction, reading_.range);
    pending_ = shown_;
    pending_active_ = true;
    debounce_on_ = true;
    debounce_since_ = millis();
    apply();
  }

  static void step_event(lv_event_t *e) {
    auto *self = static_cast<DialCard *>(lv_event_get_user_data(e));
    const uint32_t now = millis();
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED_REPEAT && (uint32_t)(now - self->stepped_at_) < 250) return;
    self->stepped_at_ = now;
    self->bump(static_cast<lv_obj_t *>(lv_event_get_target(e)) == self->step_[1] ? 1 : -1);
  }

  static void arc_event(lv_event_t *e) {
    auto *self = static_cast<DialCard *>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
      self->dragging_ = true;
      self->shown_ = from_step(lv_arc_get_value(self->arc_), self->reading_.range);
      ui::set_text(self->center_, self->plugin_->show(self->shown_, self->decimal_));
    } else if (self->dragging_) {             // RELEASED or PRESS_LOST: the finger left the ring
      self->dragging_ = false;
      self->pending_ = self->shown_;
      self->pending_active_ = true;
      self->debounce_on_ = true;
      self->debounce_since_ = millis();
      self->apply();
    }
  }

  GuestThermostat *plugin_;
  char entity_[96]{};
  Reading reading_;
  int d_ = 0, x_ = 0, y_ = 0, ring_ = 0, row_y_ = 0, row_h_ = 0;
  Font face_ = Font::VALUE, now_face_ = Font::BODY_LARGE;
  bool show_now_ = false, show_state_ = false;
  char decimal_ = '.';
  int shown_ = NO_VALUE, pending_ = NO_VALUE, raw_ = NO_VALUE;
  bool pending_active_ = false, dragging_ = false, confirm_on_ = false, debounce_on_ = false;
  uint32_t confirm_since_ = 0, debounce_since_ = 0;
  lv_obj_t *arc_{}, *center_{}, *now_{}, *state_{}, *glyph_{}, *unit_block_{}, *unit_label_{}, *status_block_{}, *status_label_{}, *status_icon_{};
  lv_obj_t *step_[2]{}, *step_label_[2]{};
  uint32_t stepped_at_ = 0;
  int unit_x_ = 0, unit_y_ = 0, unit_w_ = 0, unit_h_ = 0;
};

void GuestThermostat::setup() {
  add_tile("dial", [this]() { return new DialTile(this); });
  add_card("dial", [this]() { return new DialCard(this); });
  // The top bar: a flame in red while it heats, a snowflake while it cools, else the resting icon in green (grey when
  // off), with the temperature now beside it in the unit the glass shows. Known once a tile of this plugin has heard
  // from its thermostat.
  add_bar_item("heating", [this]() {
    tessera::BarItem item;
    if (!bar_known_) return item;
    const Mood mood = static_cast<Mood>(bar_mood_);
    if (mood == Mood::UNAVAILABLE) return item;
    item.shown = true;
    item.icon = 0xF0238;   // a flame always: red while it heats, green while it rests
    item.tone = mood == Mood::HEATING ? tessera::Tone::ALERT : tessera::Tone::GOOD;
    if (bar_current_ != NO_VALUE) item.text = show(bar_current_, decimal_of(this));
    return item;
  });
}

double GuestThermostat::normalize(double value) const {
  // A room thermostat is 5 to 35 in Celsius and 41 to 95 in Fahrenheit: the two never meet, so 38 tells them apart.
  if (!native_f_ && value > 38.0) return (value - 32.0) * 5.0 / 9.0;
  if (native_f_ && value < 38.0) return value * 9.0 / 5.0 + 32.0;
  return value;
}

std::string GuestThermostat::show(int tenths_native, char decimal) const {
  const bool f = shows_fahrenheit();
  if (f == native_f_) return format(tenths_native, decimal) + "\xC2\xB0" + (f ? "F" : "C");
  if (f) {   // Celsius shown as whole Fahrenheit
    char text[16];
    std::snprintf(text, sizeof text, "%d\xC2\xB0" "F", (int) std::lround(tenths_native / 10.0 * 9.0 / 5.0 + 32.0));
    return text;
  }
  return format(tenths((tenths_native / 10.0 - 32.0) * 5.0 / 9.0), decimal) + "\xC2\xB0" "C";
}

void GuestThermostat::toggle_unit() {
  if (fahrenheit_ == nullptr) return;
  if (fahrenheit_->state) fahrenheit_->turn_off();
  else fahrenheit_->turn_on();
  tessera::refresh();
}

void GuestThermostat::on_interval(uint32_t now_ms) {
  for (DialTile *tile : tiles_)
    if (tile) tile->tick(now_ms);
}

bool GuestThermostat::attach(DialTile *tile) {
  for (DialTile *&slot : tiles_) {
    if (!slot) {
      slot = tile;
      return true;
    }
  }
  ESP_LOGW(TAG, "more than %d dials on this screen: the others send at once", MAX_TILES);
  return false;
}

void GuestThermostat::detach(DialTile *tile) {
  for (DialTile *&slot : tiles_)
    if (slot == tile) slot = nullptr;
}

}  // namespace esphome::guest_thermostat
