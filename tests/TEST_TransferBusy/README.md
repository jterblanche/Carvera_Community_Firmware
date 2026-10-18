# TEST_TransferBusy

Host-side tests for `src/libs/TransferBusy.cpp`: the reply a WiFi client gets
when it sends something while another client's file transfer holds the
machine, which of its reads need that reply, and how often it is sent.

Like `TEST_ClientTable`, this is not a gcode fixture: the module is plain
C++ with no `Kernel` or `mbed` dependency, so it can be compiled and driven
natively.

## Running

```shell
./tests/TEST_TransferBusy/run.sh
```

Any C++17 host compiler will do; override with `CXX=g++-14 ./run.sh` if the
default `c++` is not what you want. The script builds with `-Wall -Wextra
-Werror`. It exits non-zero if any check fails.

This is not wired into CI. Run it by hand when changing this module.

## What is covered

The reply decodes as one text frame carrying exactly the busy message, and is
not built into a buffer too small for it. A read holding only status queries,
keep-alives, diagnose requests, heartbeats, relays, presence replies or
job-start readies needs no reply of its own, and neither does a read with no
frame header in it; a hello, a command, a stop, a file transfer start or any
other frame does, including one mixed in after automatic frames or a relay,
one whose type or text is cut off by the end of the read, and one whose CRC
happens to contain the header bytes. The first read from each client that is
not sent transfer notices is answered whatever it holds; a client that is
sent them is answered only for a read that needs a reply. After that,
automatic traffic gets nothing more, while other frames are answered no more
often than every 250 ms, correctly across a wrap of the microsecond counter.
Clients are tracked separately, clearing starts over, and a client beyond the
tracked number reuses the oldest entry.

The transfer notice the other identified controllers are sent during a
transfer decodes as one 0x68 event frame of 11 bytes, kind 9 followed by the
direction (0 download, 1 upload), and is not built into a buffer too small
for it. A notice is due at once when a transfer begins, then once a second
measured from the last one sent, a new transfer starts over with its own
direction, and the pace holds across a wrap of the microsecond counter.
