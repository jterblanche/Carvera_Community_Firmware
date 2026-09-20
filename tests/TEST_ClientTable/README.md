# TEST_ClientTable

Host-side tests for `src/libs/ClientTable.cpp`, the table of connected
clients (WiFi TCP sockets plus the one USB link) shared by `WifiProvider` and
`SerialConsole`.

Like `TEST_MakeraFrameParser`, this is not a gcode fixture: the table is
plain C++ with no `Kernel` or `mbed` dependency, so it can be compiled and
driven natively.

## Running

```shell
./tests/TEST_ClientTable/run.sh
```

Any C++17 host compiler will do; override with `CXX=g++-14 ./run.sh` if the
default `c++` is not what you want. The script builds with `-Wall -Wextra
-Werror`. It exits non-zero if any check fails.

This is not wired into CI. Run it by hand when changing the table.

## What is covered

Adding WiFi clients up to the 3-client cap and refusing a fourth without
disturbing the three already admitted, idempotent re-adds of an address
already present, freeing and refilling a slot on removal, the USB entry
being a single present/absent slot independent of the WiFi cap, and the
shared singleton accessor.

Also: recording identity and clamping an over-length name; the hello window
(not old before it starts or before it elapses, old once it has, never old
once identified); a WiFi client's window starting the moment it is admitted
versus a USB entry's window only starting once something is actually
received on it; clearing a USB entry's identity without removing the slot;
who counts as "present" for the old-client rule (an idle USB entry does not,
a talking one does); a lone old client versus a second one making it
not-alone; excluding a given client (and, for USB, optionally itself) when
checking whether an old client is already in the mix; and looking a client
up by id, scoped to identified clients only.
