# Changelog

## 0.1.1 - 2026-11-15

- Large - and + keys beside the dial on its card: hold one and it keeps stepping.
- Fixed: after - or + the number jumped back to the old target until the thermostat answered (the wait for Home
  Assistant ended the moment it began; Thermo dial has the same fault).
- Larger - and + keys on a high tile, and a tap that just misses a key no longer opens the dial.
- A change is sent three seconds after the last touch, so several taps or a second drag make one command; the number
  is amber until the thermostat has taken it.
- An item for the top bar: a red flame while it heats, green while it rests, with the temperature now.
- Red while it heats and green while it rests, with a word that says which on the dial's card.
- A key for Celsius or Fahrenheit on the tile too (two cells wide or more), and the tile follows a change of unit at once.
- The limits are 7 to 32 degrees, and the tile grows to 9 x 9.

## 0.1.0 - 2026-11-15

- The first version, made from Thermo dial 0.3.1: no mode key, limits of 7 to 32 degrees, a key for Celsius or Fahrenheit,
  and numbers that arrive in the other unit are read correctly.
