# Low-latency input

InputLeap sends every mouse movement to the client as a small message.
On a wired LAN this is cheap. On WiFi, each message costs airtime and a
single delayed TCP segment blocks every later movement (head-of-line
blocking), which shows up as stuttering or "rubber-banding" of the
remote cursor.

This document describes the low-latency path and how to tune it.

## What the server does

### Motion coalescing

The server forwards mouse motion at most `--motion-rate` times per
second (default `250`). Absolute positions use latest-wins semantics:
intermediate positions are dropped because only the newest one matters.
Relative deltas (used when the pointer is locked to a screen, for
example in games) are accumulated instead, so no motion is lost.

The first movement after an idle period is sent immediately. The rate
limit therefore only adds latency while the mouse is moving
continuously, and never more than `1 / rate` seconds (4 ms at the
default). Pending motion is flushed before buttons, keys, the wheel,
clipboard changes and screen switches, so a click can never overtake the
position it belongs to.

Set `--motion-rate 0` to disable the limit. This is appropriate on a
wired network; on WiFi it increases airtime usage and may bring back
stuttering.

### One write per message

Small protocol messages are written to the socket as a single buffer.
Because `TCP_NODELAY` is enabled, each write becomes one TCP segment and
one TLS record. Previously the 4-byte length prefix was written
separately, doubling the packet rate for mouse motion.

### DSCP marking

TCP and UDP sockets are marked with DSCP EF (46). Access points that
implement WMM map this to the voice access category, which is scheduled
ahead of best-effort and bulk traffic. This is best effort: not all
access points honour it.

### Encrypted UDP fast path

When both sides support it, mouse motion is sent over an authenticated
UDP channel instead of TCP:

* The server generates a random 128-bit key per client session and sends
  it over the existing TLS control channel.
* The client sends a heartbeat every 250 ms. The server learns the
  client's UDP endpoint from the heartbeat and starts sending motion
  datagrams to it.
* Every datagram is encrypted and authenticated with AES-128-GCM. A
  sequence number prevents replay and duplicate application.
* If heartbeats stop for one second, the server automatically falls back
  to TCP. When they resume, it switches back.

Buttons, keys, the wheel and clipboard data always use TCP. Only
disposable absolute motion (and accumulated relative motion) uses UDP.
Losing a motion datagram is harmless: the next one carries the newest
position.

The fast path is negotiated through the `FSTP` option during the
handshake. Older clients and older servers never see the new messages
and keep using TCP.

## Options

Server (`input-leaps`):

```
--motion-rate <hz>   maximum motion messages per second (default 250, 0 = unlimited)
--no-udp             disable the encrypted UDP fast path
--udp-port <port>    UDP port for the fast path (default 24801)
```

Client (`input-leapc`):

```
--no-udp             do not request the UDP fast path
```

## Tuning the link

Software cannot compensate for a bad link. For the best results:

* Disable WiFi power saving on both machines.
  * Linux: `sudo iw dev wlan0 set power_save off`
    (persist it with `wifi.powersave = 2` in NetworkManager).
  * Windows: Device Manager, WiFi adapter, Power Management, clear
    "Allow the computer to turn off this device"; in the adapter's
    advanced settings disable power saving / U-APSD.
* Use 5 GHz with a clean channel and a wired backhaul for the access
  point. Repeaters and mesh nodes add latency.
* On Android, use the Shizuku/UHID injection path and set the app's
  battery usage to unrestricted. Accessibility injection adds
  15-25 ms.
* Avoid large clipboard transfers while you need precise input; they
  share the TCP control channel.

## Verifying

From the server, measure the link itself:

```
ping -i 0.2 -c 300 <client-ip>
```

Look at `mdev` and the maximum, not only the average. A wired LAN is
normally below 1 ms with no spikes. WiFi jitter of tens of milliseconds
is what the fast path is designed to hide.

With debug logging enabled, the server reports:

```
NOTE: UDP fast path listening on port 24801
DEBUG1: negotiated UDP fast path session ... on port 24801
```

If the client cannot use UDP (firewall, IPv6-only server address, or an
older client), it simply stays on TCP.
