# Windows troubleshooting

## Scroll Lock moves the cursor to the primary screen

Older InputLeap releases treated the Scroll Lock toggle as a
"lock cursor to screen" switch. Windows applications (for example
spreadsheets) and some keyboards/KVMs can set the Scroll Lock state
without the user pressing the key. When that happened, the server
locked the cursor to the primary screen and refused to switch.

This legacy behaviour is now opt-in. By default Scroll Lock no longer
locks the cursor and no longer prevents switching. To restore it, start
the server with:

```
input-leaps --scroll-lock-locks-cursor ...
```

You can also configure an explicit hotkey:

```
ScrollLock = lockCursorToScreen(toggle)
```

If the Scroll Lock LED still turns on by itself, that state is set by
Windows or the keyboard; it no longer affects InputLeap.

## Clipboard does not survive a screen switch

Several cases are fixed:

* A clipboard grab from a screen that is not currently active is
  accepted even if its sequence number is older. Previously the grab
  and the following data were discarded, so the copy was lost.
* The last clipboard contents are kept while a grab is in progress.
  If the transfer is interrupted by a quick screen switch, the
  clipboard is not left empty.
* When switching screens, the new screen is forced to receive the
  current clipboard unless it is the clipboard owner. Previously a
  stale `dirty` flag could make the transfer a no-op.

If a very large clipboard is still being transferred at the moment of
the switch, the first paste can use the previous contents. Wait for the
transfer to finish or paste again.

## The Windows service does not start

The GUI talks to the `InputLeap` service over IPC. It does not run the
service itself. If the service was stopped (after a crash or a manual
stop), pressing Start used to send the command to a socket that was not
connected, so nothing happened.

Now:

* The GUI checks whether the `InputLeap` service is running before
  sending the Start command.
* If it is not running, the GUI tries to start it. Starting a service
  normally needs administrator rights, so Windows may show a UAC prompt
  (`sc start InputLeap`).
* IPC commands are queued until the connection is established, so a
  Start command is not lost while the service is starting.
* The service is installed with recovery actions: Windows restarts it
  after 5, 10 and 30 seconds if it terminates unexpectedly (reset after
  one day).

If UAC is declined, start the service manually:

```
sc start InputLeap
```

or open `services.msc` and start **InputLeap**.
