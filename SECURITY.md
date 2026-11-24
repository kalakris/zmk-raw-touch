# Security

The module parses commands that a host writes to its HID feature report,
over USB and Bluetooth. A way for a host or a nearby device to crash the
keyboard, corrupt its state, or read more than the protocol exposes is a
security problem worth reporting.

## Reporting a vulnerability

Use **Report a vulnerability** on this repository's
[Security tab](https://github.com/kalakris/zmk-raw-touch/security), which
reaches the maintainer privately. Please do not open a public issue for
it. Include the module version, your board and ZMK tree, the transport,
and how to reproduce.

This is a single-maintainer project; expect a reply within a week or so.

## Supported versions

Fixes go into the newest release only. The release job moves the
`stable` tag to it.
