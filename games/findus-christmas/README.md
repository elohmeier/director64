# Findus wartet auf Weihnachten

Experimental Director 7 port of the German Christmas calendar pinned in
`game.toml`: launcher, calendar and all 24 activities.

```sh
uv run --locked director64 android build --game findus-christmas
uv run --locked director64 web --game findus-christmas --serve
uv run --locked director64 journey --game findus-christmas
```

Click a door to open it, then again to enter; the buttons at the bottom of
each activity restart it or return to the calendar. `[player]` fixes the
date to Christmas Eve 2000 so every door is open, and saves `ByggFil.txt`.
Text uses a Droid Sans substitute (`[convert] substitute_font`).

The journey (`host/full_journey.py`) enters all 24 doors and returns.

Known gaps:

- Mixed fonts in a text member use the first run's font.
- Day 13's missing cursor members keep the current cursor; day 23's missing
  sound plays silence.
- Puppet square transitions are timed reveals while the score continues.
