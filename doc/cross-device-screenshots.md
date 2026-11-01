# Cross-device screenshots

Input Leap can capture the screen of whichever machine currently has the
pointer and deliver the resulting PNG to the server. You no longer have to
switch to a client, take a screenshot locally, and then copy the file back.

## Triggering a screenshot

Press the native full-screen screenshot shortcut while the pointer is on the
screen you want:

| Platform | Shortcut |
| --- | --- |
| Linux / X11 | `PrintScreen` |
| Windows | `PrintScreen` or `Win+Shift+S` |
| macOS | `Command+Shift+3` |

Only the active screen is captured. If the pointer is on a client, the client
captures its screen and streams the PNG back over the existing control
connection. If the pointer is on the server, Input Leap captures locally when
it receives the shortcut; on X11 the server normally releases the keyboard
while the pointer is on its own screen, so the desktop's screenshot tool
handles `PrintScreen` there instead.

Region selection and window capture are not implemented; every shortcut
captures the whole desktop of that machine.

## Where files are stored

Screenshots are written on the server, never on the client:

| Platform | Directory |
| --- | --- |
| Linux / BSD | `$XDG_CACHE_HOME/InputLeap/screenshots` (default `~/.cache/InputLeap/screenshots`) |
| macOS | `~/Library/Caches/InputLeap/screenshots` |
| Windows | `%LOCALAPPDATA%\InputLeap\screenshots` |

Files are named `<screen-name>-YYYYMMDD-HHMMSS.png`, with a numeric suffix if
two screenshots land in the same second. The server writes a `NOTE` log line
with the full path after each capture.

## Retention

Screenshots are treated as temporary artifacts. After every save the server
prunes the directory so that it keeps only:

- the newest 50 files,
- at most 512 MiB in total,
- files younger than 7 days.

Older files are removed automatically, so a long session cannot fill the
disk.

## Requirements and limitations

- PNG encoding is built in; no extra image library is needed.
- On macOS the application needs the **Screen Recording** permission
  (System Settings → Privacy & Security → Screen Recording). Without it the
  capture fails and the server logs the reason.
- Wayland does not provide a synchronous full-desktop capture API. Wayland
  clients do not advertise screenshot support, so `PrintScreen` still reaches
  the compositor's own screenshot tool.
- On X11 the server does not grab the keyboard while the pointer is on its own
  screen, so a screenshot taken there is handled by the desktop environment
  and stays in its usual location instead of the cache below.
- Clients running an older Input Leap build do not understand the request.
  The server forwards the key to them, so their local screenshot tool keeps
  working, but the image stays on that machine.
- The mouse pointer is not drawn into the image.
