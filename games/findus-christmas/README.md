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
Text uses a Droid Sans substitute (`[convert] substitute_font`), preserving
source character sizes, faces, colors and formatting runs.

The journey (`host/full_journey.py`) enters all 24 doors and returns.

Square transitions now block the score and Lingo until the reveal completes;
audio continues, and a skip click is consumed. Source-absent cursor members on
day 13 retain the current cursor, and the absent day 23 sound remains silent.
These source cases have no invented replacement media. Completion of every
activity and original-player visual parity remain unverified.
