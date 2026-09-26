# TEST_Hello

Host-side tests for `src/libs/Hello.cpp`, the wire encode/decode for the
identify-handshake messages (hello, hello ack, client-list reply).

Like `TEST_ClientTable`, this is not a gcode fixture: the module is plain
C++ with no `Kernel` or `mbed` dependency, so it can be compiled and driven
natively.

## Running

```shell
./tests/TEST_Hello/run.sh
```

Any C++17 host compiler will do; override with `CXX=g++-14 ./run.sh` if the
default `c++` is not what you want. The script builds with `-Wall -Wextra
-Werror`. It exits non-zero if any check fails.

This is not wired into CI. Run it by hand when changing these encoders or
decoders.

## What is covered

Parsing a well-formed hello, including a zero-length name; rejecting a
payload too short for its own name length, a name length over the 31-byte
limit, and an unrecognised protocol version; ignoring trailing bytes past
the link field (a future version's appended fields); the link byte mapping
to WiFi or USB; building a hello ack and reading back its three fields;
building a client-list reply from a table with a mix of identified and
unidentified WiFi and USB clients, checking only the identified ones are
listed, in WiFi-then-USB order; an empty table producing a reply with a
zero count and no entries; and `has_control`: false for every entry when
nobody holds control, true only for the entry matching the current holder
among several clients, true for the USB entry when the USB client holds
control, and following control as it moves from one client to another.

Also: which clients must identify first (none while nobody present has
identified, however many unidentified clients there are; an unidentified
WiFi or USB client once another client has identified, until it identifies
itself or the identified client leaves); that only a hello and a status
query are taken from such a client, and no other realtime byte, command,
file transfer or handshake message; and that only a hello ack and the empty
status frame, checked byte for byte, may be sent to it, and no status with
content, command reply, diagnostic or plain text.
