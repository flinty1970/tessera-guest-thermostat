# Guest thermostat

A thermostat for a guest: turn the heating up or down within limits you set, and nothing else. There is no key for the
mode and no way to turn the heating on or off from the screen.

- The tile: the name, the target large, the temperature now, and **-** and **+** keys.
- A tap on the tile opens a card with a dial to drag, like Home Assistant's thermostat card.
- The limits: 7 to 32 degrees Celsius unless you change them.
- A key on the card, and on a tile two cells wide or more, changes what the screen shows between Celsius and
  Fahrenheit. The thermostat is not changed.
- Red while it heats, green while it rests; the card says which in words.

## Set up

1. Add the plugin to a screen in Tessera. The screen builds once.
2. In Layout, place the **Guest thermostat** tile and choose the `climate.*` entity in the tile's inspector.
3. Make sure the screen may perform Home Assistant actions (Tessera turns this on for a screen it added itself).

## Good to know

- The dial and the **-** and **+** keys change the number on the screen only. Three seconds after the last touch
  Home Assistant gets one `climate.set_temperature`; until the thermostat has taken it the number is amber.
- The number you chose stays on the screen for up to 20 seconds while a slow thermostat (one behind a cloud) catches
  up; if it never does, the number goes back to the real one.
- Home Assistant has a switch on the screen's device, **Thermostat shows Fahrenheit**. Turn it off in an automation
  when a guest leaves and the next guest starts in Celsius.
- A thermostat whose numbers arrive in the other unit (a Celsius thermostat seen through a Fahrenheit Home Assistant)
  is read correctly: a number that can only be the other unit is converted.
- The limits and the thermostat's own unit are in `plugin.yaml` (`min_temp`, `max_temp`, `unit`).

## Thanks

The tile, the card and the dial come from [Thermo dial](https://github.com/alexcoscia/tessera-thermo-dial) by Alex
Coscia (MIT). This plugin takes away the mode key and adds the limits and the choice of unit.

## Tried on

- Drawn by the plugin bench. On a real screen: a Guition JC4880P443.
