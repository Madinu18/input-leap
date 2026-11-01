# Cross-device file transfer

Input Leap can move files between machines by dragging them onto a glowing
line at the edge of the screen. Transfers work in both directions: from the
server to a client and from a client back to the server.

## Using the lines

1. Start dragging a file in your file manager.
2. An accent-coloured line appears on every screen edge that faces another
   Input Leap machine.
3. Move the pointer close to a line. The line grows and glows.
4. Drop the file directly on the line.

The line disappears and a small progress window shows the file name, the
transferred size and a progress bar on both machines. Files are written to
the **Downloads** folder of the receiving machine.

The line is only mapped while the mouse button is held, so it does not
interfere with normal clicks. A normal screen-edge drag (the existing
drag-and-drop behaviour) still works as before. Line transfer does not require
the `--enable-drag-drop` option.

## Destination and naming

Received files are stored in the platform Downloads directory:

| Platform | Directory |
| --- | --- |
| Linux / BSD | `$XDG_DOWNLOAD_DIR` from `user-dirs.dirs`, or `~/Downloads` |
| macOS | `~/Downloads` |
| Windows | the user's `Downloads` known folder |

The file name is reduced to its base name and control characters are removed.
If a file with the same name already exists, Input Leap appends ` (1)`,
` (2)`, and so on instead of overwriting it.

Data is written to a temporary `.part` file first and renamed only after the
whole file has arrived and its size has been verified. A failed or cancelled
transfer leaves no partial file behind.

## Limits and platform notes

- The maximum size of a single file is 4 GiB (the size is carried in the
  protocol as a 32-bit integer).
- One outgoing and one incoming transfer can run at a time per machine.
  Additional drops are rejected with a progress-window message.
- On X11 the lines are override-redirect windows shaped into a thin visible
  strip with a wider invisible drop area. They use the XDND protocol, so they
  work with file managers that provide `text/uri-list`.
- On Windows the lines are layered topmost windows with an OLE drop target.
- On macOS the lines are borderless `NSWindow` instances that accept
  `NSFilenamesPboardType` drags.
- Wayland clients do not currently provide an overlay window, so the line is
  unavailable there. The platform's own drag-and-drop remains unchanged.
- Only regular files can be transferred. Directories and special files are
  skipped.
