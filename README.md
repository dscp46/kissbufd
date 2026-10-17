# kissbufd

ACKMODE-based transmit buffer management for KISS TNCs that have no
RTS/CTS flow control, such as the NinoTNC. kissbufd exposes a pty that
`kissattach` (or any KISS program) can use, and keeps the TNC's transmit
buffer full without ever overflowing it.

```
kissattach / any KISS client  <-pty->  kissbufd  <-serial->  ACKMODE TNC
```

Any TNC implementing the KISS ACKMODE extension (`0x0C`) works.

## How it works

- Every data frame goes to the TNC as an ACKMODE frame with a unique 16-bit
  wire ID. The TNC returns the ID when the frame has left over the air.
- In-flight bytes are counted as **raw KISS bytes** (FENDs, type byte, ID,
  escapes, payload). A frame is released only if it fits under
  `fill_percent` (default 95) of `buffer_size`. Example: a 273-byte frame
  leaves, freeing ~278 bytes; an 800-byte frame waits until enough has left.
- Frames larger than the fill limit are dropped as giants.
- Plain KISS frames from the host get an internal ID; their ACKs are consumed.
- ACKMODE frames from the host are mapped through a translation table; the ACK
  is relayed back with the **host's** ID.
- Wire IDs come from a counter, skip any ID still in flight, and skip IDs
  containing `0xC0`/`0xDB`, so they never need escaping. IDs are network byte
  order: the first byte on the wire is the most significant.
- Other KISS commands pass through unchanged, in arrival order relative to data.
- Frames from the TNC are relayed unchanged; ACKs for unknown IDs are dropped.
- No ACK within `ack_timeout` (default 60 s): the frame is considered lost,
  logged as `MINOR ALARM`, and nothing is sent to the host.
- Serial link lost (USB unplug, I/O error): on-TNC frames are considered lost,
  the host-side queue is held, and the device is reopened every
  `reopen_interval` seconds.
- Host-side queue full (`queue_limit`): new frames are dropped.

## Configuration

Every setting can be given in a config file (`-c file`) or on the command
line; the command line wins. See `kissbufd.conf(5)` and the annotated
`kissbufd.conf.sample`.

| Key | Option | Meaning | Default |
|---|---|---|---|
| `device` | `-s` | TNC serial device | required |
| `buffer_size` | `-B` | TNC buffer, raw KISS bytes (64..1048576) | required |
| `baud` | `-b` | serial speed | 57600 |
| `serial_mode` | `-m` | framing, e.g. `8N1`, `7E1`, `8N2` | 8N1 |
| `dtr` | `-R` (= no) | assert DTR/RTS on open | yes |
| `fill_percent` | `-F` | fill limit, % of `buffer_size` | 95 |
| `pty_link` | `-p` | symlink to the pty slave | none |
| `ack_timeout` | `-t` | seconds before a frame counts as lost | 60 |
| `reopen_interval` | `-r` | serial reopen interval, seconds | 2 |
| `queue_limit` | `-Q` | host-side queue limit, bytes | 262144 |
| `daemon` | `-D` / `-f` | detach / stay in foreground | no |
| `syslog` | `-S` | log to syslog | no |
| `pidfile` | `-P` | pid file | none |
| `verbose` | `-v`, `-vv` | 0..2 | 0 |

`-n` checks the configuration and exits. `SIGUSR1` logs statistics.

## Build and install

```sh
make                 # GNU make or BSD make
make check           # unit tests
make integration     # end-to-end test against a simulated TNC on a pty
make install         # binary, man pages, docs, sample config
make install-service # service file for this OS (see below)
```

Paths: `PREFIX` (`/usr/local`), `SYSCONFDIR` (`$PREFIX/etc`), `RUNDIR`
(`/var/run`), `DOCDIR`, `MANDIR`, `SYSTEMDUNITDIR`, `RCDIR`, `DESTDIR`.
For OpenBSD and NetBSD conventions use `SYSCONFDIR=/etc`.

Portable C99 + POSIX: `poll()`, `posix_openpt()`, `termios`, `getopt`.
Building, testing and packaging need only a C compiler and make.
`KB_TEST_BUF=32768 ./tests/sim_tnc ./kissbufd` runs the end-to-end test
with another TNC buffer size (default 8192).

## Services

| System | File | Instances |
|---|---|---|
| Linux (systemd) | `kissbufd@.service` | `systemctl enable --now kissbufd@tnc0` reads `/etc/kissbufd/tnc0.conf` |
| FreeBSD | `rc.d/kissbufd` | `sysrc kissbufd_enable=YES`; `kissbufd_profiles="tnc0 tnc1"` for several |
| OpenBSD | `/etc/rc.d/kissbufd` | `rcctl enable kissbufd`; symlink `kissbufd_tnc1` + `rcctl set ... flags` for more |
| NetBSD | `/etc/rc.d/kissbufd` | `kissbufd=YES` in rc.conf; copy the script for more |
| macOS | `kissbufd.plist` | `launchctl bootstrap system /Library/LaunchDaemons/kissbufd.plist` |

Sources are in `services/`; `make install-service` picks the right one.

## Debian package

```sh
dpkg-buildpackage -us -uc -b
```

Set your name in `debian/control` and `debian/changelog` before publishing.
The package installs the binary, `kissbufd(8)`, `kissbufd.conf(5)`, the
systemd template unit, and `/usr/share/doc/kissbufd/kissbufd.conf.sample`:

```sh
cp /usr/share/doc/kissbufd/kissbufd.conf.sample /etc/kissbufd/tnc0.conf
systemctl enable --now kissbufd@tnc0
kissattach /run/kissbufd/tnc0 radio
```

## Code style

- Function definitions and declarations on one line.
- Opening braces on the line of their statement or function.
- A space after `(`, none before `)`; casts and empty `()` stay tight:
  `if ( n > 0) {`, `foo( a, b);`, `(uint8_t)x`.
- Tabs for indentation, no line wrapping at 80 columns.

`make style` applies this with clang-format (`.clang-format`) and the small
`tools/cstyle` filter, built from `tools/cstyle.c`, for what clang-format
cannot express.

## Limitations

- A TNC reset that does not drop the serial link is not detectable; frames
  lost that way are reclaimed by the ACK timeout.
- When no program has the pty open, frames received from the TNC accumulate
  in the kernel pty buffer and are then dropped.

## License

MIT, see `LICENSE`.
