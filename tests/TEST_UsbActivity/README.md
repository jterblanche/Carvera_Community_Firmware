# TEST_UsbActivity

Host-side tests for `src/libs/UsbActivity.h`, which keeps the time the USB
link last received anything and decides when the USB session has gone quiet
for long enough to end, and when a temporary baud rate reverts.

Like `TEST_ClientTable`, this is plain C++ with no `Kernel` or `mbed`
dependency, so it is compiled and run natively.

## Running

```shell
./tests/TEST_UsbActivity/run.sh
```

Any C++17 host compiler will do; override with `CXX=g++-14 ./run.sh` if the
default `c++` is not what you want. It exits non-zero if any check fails.

This is not wired into CI.

## What is covered

A quiet USB link still ends its session after 10 seconds; an upload or
download that runs longer than that keeps the session, and a controller that
has said hello still gets its output while WiFi controllers that have said
hello are connected; switching receive handling back on restamps the time,
so both checks count from the end of the transfer; a temporary baud rate is
not reverted during a transfer; both checks across a wrap of the
microsecond counter; and a byte stamped after the idle work read the clock,
so later than its reading, counts as no quiet time, with or without a wrap
between the two.
