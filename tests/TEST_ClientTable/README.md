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
