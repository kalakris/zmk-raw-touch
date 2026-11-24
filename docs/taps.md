# Taps

How the module turns touches into clicks, and why it has to. The
[README](../README.md#setup) has the configuration; this page explains
it.

## Why the module detects taps

In relative mode, the Pinnacle hardware recognizes taps and reports them
as button presses. The driver forwards those reports; it does not detect
taps itself. Its `primary-tap-enable` property applies only in relative
mode.

The module needs absolute mode, for finger positions and lift-off
samples. In absolute mode the Pinnacle driver turns hardware tap
recognition off and reports X, Y and touch strength instead, with no
software tap detector. Leaving `primary-tap-enable` set therefore does
not keep tap-to-click. This is how the supported Pinnacle driver works,
not a rule for every absolute-mode driver.

With `tap-click` on the `zmk,raw-touch-pad` node, the module supplies
the detector: a touch is a tap when it lifts within `tap-max-ms` and
never moves more than `tap-max-movement` counts from where it touched
down. The module emits `INPUT_BTN_0` into the pad's listener chain, so a
`tap_to_click` mapper is not needed, and a processor that discards that
button, such as a `zip_button_behaviors` instance mapping it to `&none`,
swallows the tap. Taps behave the same in Standard and RawTouch modes.

## Taps and scrolling

The module knows whether a touch was handled by a chain marked with
`&zip_raw_touch_scroll`. By default such a touch never taps, so a flick
on a scroll layer does not click.

On a dedicated scrolling pad every touch is a scroll touch, so
`tap-click-while-scrolling` lets that pad's taps through. The listener
chain decides what they do: a tap is a left click unless a
`zmk,input-processor-code-mapper` maps `INPUT_BTN_0` to another button,
as MoErgo's stock Go60 keymap does to make its left pad right-click.

## Tap confirmation in RawTouch mode

In Standard mode, a tap is emitted at lift-off. In RawTouch mode, a
short touch on a scrolling pad may only have stopped the host's coasting
scroll, and the firmware cannot tell that from a tap. So for a pad with
`tap-click-while-scrolling`, while a host holds the lease, the firmware
holds a qualifying tap for up to 250 ms and emits it only when the host
confirms it; an unconfirmed tap is never emitted. Confirming is
therefore required of any host that holds the lease on a keyboard with
such a pad.

RawTouch confirms a tap when the touch did not stop a coast, stayed in
scroll context, lasted at most 300 ms and moved at most 60 counts on
either raw axis. Those limits are fixed in the app and are wider than
the firmware defaults (180 ms, 30 counts), so with the defaults the
firmware's own limits decide. Raising `tap-max-ms` or
`tap-max-movement` past the app's limits allows taps in Standard mode
that RawTouch mode rejects. The wire format is under
[Tap confirm](protocol.md#tap-confirm).
