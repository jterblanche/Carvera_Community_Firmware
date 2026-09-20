# TEST_FrameResync

Host-side tests for `src/libs/FrameResync.cpp`, the wrapper around
`makera::FrameDecoder` that bounds how long `WifiProvider` will hunt for a
frame header before giving up on that run of bytes and trying again on the
next one, and that decides when to show the "please update" diagnostic.

Like `TEST_MakeraFrameParser` and `TEST_ClientTable`, this is not a gcode
fixture: the wrapper is plain C++ with no `Kernel` or `mbed` dependency, so
it can be compiled and driven natively.

## Running

```shell
./tests/TEST_FrameResync/run.sh
```

Any C++17 host compiler will do; override with `CXX=g++-14 ./run.sh` if the
default `c++` is not what you want. The script builds with `-Wall -Wextra
-Werror`. It exits non-zero if any check fails.

This is not wired into CI. Run it by hand when changing the wrapper.

## Why this exists

A controller probes a machine with plain text (`protocols/detector.py`
sends `echo echo\n` three times) before it knows which protocol the
firmware speaks. Against firmware built without this wrapper
(`WifiProvider::receive_wifi_data()` on `master` before this branch), that
probe permanently wedges the connection: the header-error counter that
triggers the "no valid frame found" diagnostic was never reset once it
tripped, so every byte of every frame afterwards -- valid or not -- retripped
it immediately, and the function `return`ed out of the byte loop on that
same trip, discarding whatever else was already sitting in the read buffer
(including a second query that had arrived in the same read as the first).
The connection never recovered. This was never caught by
`TEST_MakeraFrameParser` because that suite drives `FrameDecoder` directly,
never through the header-error bookkeeping that lived inline in
`WifiProvider.cpp` -- itself untestable on the host, since `WifiProvider` is
`Kernel`/`mbed`-coupled. Pulling that bookkeeping out into `FrameResync.cpp`
is what makes it testable at all.

## What is covered

The exact probe-then-frame sequence from the report, reproduced byte for
byte where practical: a run of junk below the threshold, then a
valid frame; a run right at the threshold boundary (19 bytes, then 20); the
three-probe sequence as three separate reads; junk split across many
one-byte reads; a valid frame itself split across reads right after junk;
enough junk to trip the threshold twice in the same connection (notified on
the first trip only, not the second); `reset()` re-arming the
once-per-connection notification for a reused connection slot; and a byte
that looks like the start of a header (matches the first header byte) but
isn't followed by the second, which must not by itself count as extra
failures against the byte that follows it.

Not covered here, and not host-testable without pulling `WifiProvider`
apart much further than this fix warrants: that `receive_wifi_data()`
actually calls `consume_notify_pending()` and shows the diagnostic at the
right place, that it dispatches a decoded command via `command_waiting`,
and the interaction with per-chunk byte offsets (`pending_wifi_*`). Those
stay verified by the firmware build and, eventually, a real controller
connection.
