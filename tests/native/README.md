# Native C tests

`mise run test-native` compiles and runs these host C tests of the shared
runtime (Lingo, Director services, saves, pointer, Android host pieces) for
each Director family they cover, plus the games' own tests under
`games/<slug>/tests/`. They need no game media.
