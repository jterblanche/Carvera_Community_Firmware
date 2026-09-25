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

Also: `is_transfer_owner`, which a file transfer's byte stream uses to tell
its own client's bytes apart from every other connected client's -- true only
for the sender at the given client index's own address, false for an
out-of-range index, and false once that index's slot has been freed (the
transferring client disconnecting mid-transfer).

Also: sending to one client while the WiFi module's send buffer is full,
against a stand-in for the module that refuses a send while 8 packets are
waiting and drains at a set pace. A long reply sent faster than the network
takes it arrives whole on the client that asked and on a second client; a
client that has stopped reading holds things up once, not once per message,
and is still marked gone after the usual run of failures; errors that mean
the client is gone, or a mistake on our side, are not retried; and a send
the module takes only part of carries on from where it stopped.
