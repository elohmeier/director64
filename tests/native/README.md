# Portable C contracts

Run `mise run test-native` without source media. The suite checks resumable Lingo,
Director timing and state ownership, geometry, audio bounds, and workshop save and
pointer adapters. Shared tests live here; game tests live beside their adapters.

Cursor contracts run for D5, D6, extended D6 and D8: bitmap/member references,
numeric built-ins, hover precedence, clearing/hiding, movie/dialog lifecycle,
hotspots and explicit mask composition. Native image fixtures also verify cursor
coverage independently of ordinary sprite alpha and reject malformed FDI files.

These are host C tests. Native game journeys and N64 captures are separate gates.
