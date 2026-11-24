# zmk-raw-touch

[![CI](https://github.com/kalakris/zmk-raw-touch/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/kalakris/zmk-raw-touch/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/kalakris/zmk-raw-touch)](https://github.com/kalakris/zmk-raw-touch/releases/latest)
[![License: MIT](https://img.shields.io/github/license/kalakris/zmk-raw-touch)](LICENSE)

<img src="https://raw.githubusercontent.com/kalakris/rawtouch/main/docs/images/demo.gif" align="right" width="180" alt="A finger on a Go60's built-in trackpad scrolls a web page through RawTouch: the page follows the finger, coasts after a flick, and stops when the finger touches down again">

A ZMK module that sends a keyboard trackpad's finger position and contact
state to a host application over USB or Bluetooth. The macOS app
[RawTouch](https://github.com/kalakris/rawtouch) turns them into phased
scroll gestures, the kind a Mac trackpad produces: two-axis scrolling that
follows the finger, coasting after a flick, and touch-to-stop. Wheel
events carry no touch-down, lift-off or momentum phase, so the keyboard
cannot produce these on its own.

The keyboard has two modes and switches between them by itself:

- **Standard mode:** no host software. The module derives pointer
  movement and optional tap-to-click from the pad, and your keymap's
  wheel processors scroll. This works on any computer.
- **RawTouch mode:** while RawTouch runs, it holds a lease on the touch
  stream. The module sends touch samples and suppresses its own wheel
  events, and RawTouch scrolls. Pointer movement and taps stay in the
  firmware.

The module adds its own USB HID interface and Bluetooth HID service; it
does not patch ZMK.

**Requirements**

- A ZMK 0.3 / Zephyr 3.5 tree. MoErgo's `v26.09` is tested; upstream ZMK
  `v0.3` should work but is untested. ZMK `main` (Zephyr 4.1) is not
  supported yet.
- Zephyr's USB device stack (`USB_DEVICE_STACK`), not
  `USB_DEVICE_STACK_NEXT`.
- A Cirque Pinnacle trackpad, driven in absolute mode by
  [`kalakris/cirque-input-module`](https://github.com/kalakris/cirque-input-module)
  `v0.1.0`. It replaces the Pinnacle driver your tree uses now. Another
  driver works if it meets the [driver requirements](#driver-requirements).
- `CONFIG_ZMK_POINTING=y`.
- For RawTouch mode: a Mac with macOS 13 or later and RawTouch 0.1.

On a MoErgo Go60, use the ready-made
[Go60 configuration](https://github.com/kalakris/go60-rawtouch-config)
instead of these steps. For the other repositories, see
[Which repository?](#which-repository)

## Setup

You need a ZMK configuration that already builds for your keyboard. The
devicetree snippets go in your board's `.keymap` or `.overlay` file under
`config/`, and the settings in its `.conf` file.

**Split keyboard?** Only the central half talks to the computer, so the
`zmk,raw-touch-pad` node, the listener changes and the module's settings
go in the central half's files. For a pad wired to the central, follow
steps 1 to 6 as written. For a pad on the peripheral, also read
[Split keyboards](#split-keyboards) before step 3: the driver changes go
on the peripheral, everything else on the central.

### 1. Add the module and the driver to `config/west.yml`

Merge these entries into the existing `remotes` and `projects` lists.
Keep your board's ZMK project and other dependencies:

```yaml
manifest:
  remotes:
    - name: kalakris
      url-base: https://github.com/kalakris
  projects:
    - name: zmk-raw-touch
      remote: kalakris
      revision: v0.1.0
    - name: cirque-input-module
      remote: kalakris
      revision: v0.1.0
```

The driver entry must be named `cirque-input-module`. Some ZMK trees,
MoErgo's among them, already import a project of that name with Pete
Johanson's driver. West keeps the first definition of a name, and your
manifest's own projects come before its imports, so this entry overrides
the imported one. If your `west.yml` already lists a
`cirque-input-module` project, replace that entry instead of adding a
second one: two projects with one name fail.

Both repositories publish numbered release tags and a moving `stable` tag
that follows the newest release. A numbered tag gives a build that
changes only when you edit it; `stable` picks up new releases without
edits. Each module release's notes name the driver tag it was tested
with. Avoid `main`, which carries unreleased work. GitHub Actions fetches
tags afresh on every build; a local west workspace needs
`west update --fetch=always` to see a moved `stable`.

### 2. Move the pad to the new driver's properties

Your board's pad node is written for the driver it used before. The
devicetree tooling rejects properties the new driver does not declare,
so delete those with `/delete-property/` and set their replacements.

**Coming from petejohanson/cirque-input-module** (the driver most ZMK
Cirque boards use, including the stock Go60):

| That driver | This driver | Notes |
|---|---|---|
| `dr-gpios` | `data-ready-gpios` | Same pin and flags. |
| `rotate-90`, `x-invert`, `y-invert` | Delete | Set the same flags on the `zmk,raw-touch-pad` node in step 3. Do not use this driver's `swap-xy`, `invert-x` or `invert-y`. |
| `no-taps`, `no-secondary-tap` | Delete | Absolute mode turns hardware taps off; `tap-click` on the pad node replaces them. |
| `sleep` | `sleep-mode-enable` | |
| `sensitivity` | `sensitivity` | Same values. The default differs: `1x` before, `4x` here, so set it explicitly to keep your pad's feel. |
| `x-axis-z-min`, `y-axis-z-min` | Unchanged | Same defaults, 5 and 4. |
| (none) | `data-mode = "absolute"` | Required. |
| (none) | `idle-packets-count = <3>` | Required. At `0` the pad reports no lift-off: no momentum and no taps. |
| `CONFIG_ZMK_INPUT_PINNACLE_IDLE_SLEEPER` | (none) | Remove it from your `.conf` files; the symbol does not exist here. |

For a board whose pad node is called `trackpad`, the result looks like
this. Use your board's node label and its data-ready pin:

```dts
#include <zephyr/dt-bindings/gpio/gpio.h>

&trackpad {
    /delete-property/ dr-gpios;
    /delete-property/ rotate-90;
    /delete-property/ y-invert;
    /delete-property/ no-secondary-tap;

    data-ready-gpios = <&gpio0 4 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>;
    sensitivity = "1x";
    data-mode = "absolute";
    idle-packets-count = <3>;
};
```

If your pad already uses this driver (Zephyr's `cirque,pinnacle`
binding), the property names stay: set `data-mode` and `idle-packets-count`, and remove
`primary-tap-enable`, `swap-xy`, `invert-x` and `invert-y`. For any
other driver, see [Driver requirements](#driver-requirements).

### 3. Register the pad and mark the scrolling chain

This example pad points normally, supports taps, and scrolls while layer
2 is active. It is mounted with its axes swapped and Y inverted; use your
own orientation and layer number, and bind a key to the layer. Edit your
existing listener rather than adding a second one for the same pad. The
`&trackpad` block repeats the absolute-mode lines from step 2; keep one
copy.

<!-- example: examples/trackpad/raw-touch.overlay -->
```dts
#include <input/processors.dtsi>
#include <raw_touch/processors.dtsi>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <dt-bindings/zmk/input_transform.h>

#define SCROLL_LAYER 2

&trackpad {
    data-mode = "absolute";
    idle-packets-count = <3>;
    /delete-property/ primary-tap-enable;
};

/ {
    raw_touch_pad: raw_touch_pad {
        compatible = "zmk,raw-touch-pad";
        device = <&trackpad>;
        pad-id = <0>;
        rotate-90;
        y-invert;
        tap-click;
    };

    trackpad_listener: trackpad_listener {
        compatible = "zmk,input-listener";
        device = <&trackpad>;
        input-processors = <&zip_xy_scaler 1 4>,
                           <&zip_raw_touch_idle_filter>;

        scroll {
            layers = <SCROLL_LAYER>;
            input-processors = <&zip_raw_touch_scroll>,
                               <&zip_xy_to_scroll_mapper>,
                               <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>,
                               <&zip_scroll_scaler 1 32>,
                               <&zip_raw_touch_idle_filter>;
        };
    };
};
```

What each part does:

1. **The `&trackpad` block** puts the pad in absolute mode with lift-off
   samples.
2. **The `zmk,raw-touch-pad` node** registers the pad with the module.
   Give each pad its own `pad-id` from 0 to 7. `rotate-90` and `y-invert`
   describe the mounting for both the pointer and RawTouch; `tap-click`
   turns on the module's tap detection (see [Taps](docs/taps.md)).
3. **`&zip_raw_touch_scroll`, first in the scroll chain,** tells the
   module and RawTouch that this chain scrolls. The wheel processors
   after it are what scrolls in Standard mode.
4. **The scalers** divide by 4 and 32. Deltas derived from absolute
   positions are about four times larger than relative-mode output:
   4.2 times, measured on a Go60's 35 mm Pinnacle pad. Other pads and
   sensitivities differ, so treat both values as starting points and
   tune. Relative mode also saturates on very fast swipes; derived
   motion does not.
5. **`&zip_raw_touch_idle_filter`, last in every chain,** drops reports
   that carry no change. Each listener needs its own filter instance.

The scroll transform sets the Standard-mode vertical direction. This one
makes a downward drag scroll like rolling a mouse wheel toward you; drop
it for the opposite direction. RawTouch mode ignores these processors
and takes its direction from the pad node and its own settings.

If a hold-tap key activates the scroll layer, add `hold-while-undecided;`
to that hold-tap. Trackpad input does not resolve a hold-tap, so without
it a quick flick inside the tapping term starts before the layer is
active.

To make a pad scroll all the time, without a layer, see
[Dedicated scrolling pad](#dedicated-scrolling-pad).

### 4. Set the Kconfig options

Add to the central half's `.conf` (or the only `.conf` on a unibody
keyboard):

```conf
CONFIG_ZMK_POINTING=y
CONFIG_INPUT_QUEUE_MAX_MSGS=64
CONFIG_INPUT_THREAD_STACK_SIZE=2048
```

`CONFIG_ZMK_POINTING` is required; with it, an enabled
`zmk,raw-touch-pad` node turns the module on, and its USB and Bluetooth
transports default to on. The input queue and stack settings give
absolute-mode pads headroom: ZMK's defaults (16 messages, 512 bytes)
drop events with two pads and long processor chains.

The module uses a second USB HID interface, `HID_1`, beside ZMK's
`HID_0`, and sets `CONFIG_USB_HID_DEVICE_COUNT` to 2. Do not set it
lower; that fails the build. Another module that also uses `HID_1` needs
integration work. The [Kconfig reference](#kconfig) lists the other
options.

### 5. Build and flash

Build with your usual ZMK workflow and flash the firmware. On a split
keyboard, flash both halves from the same build.

On every Bluetooth host the keyboard was paired with, forget the
keyboard, clear its bond on the keyboard (`&bt BT_CLR` on the selected
profile), and pair again. The module adds a Bluetooth HID service, and
macOS keeps a stale copy of the old report map otherwise. USB needs
nothing.

### 6. Check both modes

1. With no host app running, check pointer movement, taps, and wheel
   scrolling on your scroll layer or dedicated pad, over USB and over
   Bluetooth. This is Standard mode. If it fails, the cause is in the
   firmware, not the app.
2. Install RawTouch and follow its
   [Get started](https://github.com/kalakris/rawtouch#get-started) steps
   from step 2. They end with a first RawTouch-mode scroll.
3. Quit RawTouch and check that wheel scrolling returns.

RawTouch mode on an operating system other than macOS needs a host that
implements the [protocol](docs/protocol.md).

## Dedicated scrolling pad

To make a pad scroll without holding a key, put the marker and the wheel
processors in its base chain. For the step 3 example, this overlay
removes the `scroll` child and replaces the pointer chain:

<!-- example: examples/trackpad/dedicated-scroll.overlay -->
```dts
&trackpad_listener {
    /delete-node/ scroll;
    input-processors = <&zip_raw_touch_scroll>,
                       <&zip_xy_to_scroll_mapper>,
                       <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>,
                       <&zip_scroll_scaler 1 32>,
                       <&zip_raw_touch_idle_filter>;
};

&raw_touch_pad {
    /* Every touch on this pad is scroll context, and the module drops
     * taps from scroll-context touches unless this is set. */
    tap-click-while-scrolling;
};
```

Pads are independent: one can always scroll while another points or uses
a scroll layer. Check other overlays of the same listener too; one that
replaces the base chain changes this.

The module drops taps from touches used for scrolling, which on a
dedicated pad is every touch. `tap-click-while-scrolling` keeps them for
one pad. A tap left-clicks unless the chain maps it: MoErgo's stock Go60
keymap turns the left pad's taps into right clicks with a
`zmk,input-processor-code-mapper` from `INPUT_BTN_0` to `INPUT_BTN_1`.
Keep such a mapper before the idle filter. In RawTouch mode the firmware
holds such a tap until RawTouch confirms it, so that a touch that only
stopped a coasting scroll does not click; see [Taps](docs/taps.md).

RawTouch's per-pad **Scroll axes** setting can limit RawTouch-mode
scrolling to one axis. It does not change the Standard-mode wheel chain.

## Split keyboards

The central half sends reports to the computer. A pad on the peripheral
sends its samples through a `zmk,input-split` relay; the central turns
them into pointer, wheel and raw-touch reports. Each raw-touch report
of a relayed pad says whether the split link carrying it is wired or
wireless, and RawTouch shows it next to the pad's measured latency. The
examples call the physical pad `remote_trackpad` and the relay
`trackpad_split`; reuse your board's own nodes where it has them. Split
roles stay as your board defines them.

### Shared definitions

Both halves use the same relay ID (`reg = <0>` here), which is
independent of the raw-touch `pad-id`. The listener is enabled only on
the central.

<!-- example: examples/split/shared.dtsi -->
```dts
/ {
    inputs {
        #address-cells = <1>;
        #size-cells = <0>;

        trackpad_split: trackpad_split@0 {
            compatible = "zmk,input-split";
            reg = <0>;
        };
    };

    remote_trackpad_listener: remote_trackpad_listener {
        compatible = "zmk,input-listener";
        device = <&trackpad_split>;
        status = "disabled";
    };
};
```

### Peripheral: send absolute samples

<!-- example: examples/split/peripheral.overlay -->
```dts
&remote_trackpad {
    data-mode = "absolute";
    idle-packets-count = <3>;
    /delete-property/ primary-tap-enable;
};

/ {
    input_processors {
        zip_raw_touch_split_stamp: zip_raw_touch_split_stamp {
            compatible = "zmk,input-processor-raw-touch-split-stamp";
            #input-processor-cells = <1>;
        };
    };
};

&trackpad_split {
    device = <&remote_trackpad>;
    /* The cell is the relay's own reg (trackpad_split@0 -> 0). */
    input-processors = <&zip_raw_touch_split_stamp 0>;
};

&remote_trackpad_listener {
    status = "disabled";
};
```

The peripheral sends untransformed X/Y/Z samples, including lift-off,
and its listener stays disabled. Do not add a `zmk,raw-touch-pad` node
or any transform here; the central does that. Apply step 2's driver
changes to this half's pad node.

The stamp processor sends each sample with the peripheral's own sample
time, so the split link's delivery jitter does not reach the timestamps
the host derives velocity from. Chain it on the relay, on the peripheral
only; its cell is the relay's `reg`. It also announces the peripheral's
module version before the first touch after boot and after any pause of
500 ms or more. A peripheral with two relayed pads needs two stamp
nodes.

### Central: process the relayed samples

<!-- example: examples/split/central.overlay -->
```dts
#include <input/processors.dtsi>
#include <raw_touch/processors.dtsi>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <dt-bindings/zmk/input_transform.h>

#define SCROLL_LAYER 2

&trackpad_split {
    /delete-property/ device;
};

/ {
    raw_touch_remote: raw_touch_remote {
        compatible = "zmk,raw-touch-pad";
        device = <&trackpad_split>;
        pad-id = <1>; /* Local pad uses 0. */
        rotate-90;
        y-invert;
        tap-click;
    };

    input_processors {
        zip_raw_touch_idle_filter_remote: zip_raw_touch_idle_filter_remote {
            compatible = "zmk,input-processor-raw-touch-idle-filter";
            #input-processor-cells = <0>;
        };
    };
};

&remote_trackpad_listener {
    status = "okay";
    device = <&trackpad_split>;
    input-processors = <&zip_xy_scaler 1 4>,
                       <&zip_raw_touch_idle_filter_remote>;

    scroll {
        layers = <SCROLL_LAYER>;
        input-processors = <&zip_raw_touch_scroll>,
                           <&zip_xy_to_scroll_mapper>,
                           <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>,
                           <&zip_scroll_scaler 1 32>,
                           <&zip_raw_touch_idle_filter_remote>;
    };
};
```

The relay on the central has no `device` property: the split link
supplies its events. The raw-touch node points at **the relay**, not the
physical pad. The remote listener needs its own idle-filter instance; it
can share the scroll marker with a local listener. If the central also
has a local pad, keep its configuration and give the two pads different
IDs.

For a dedicated remote scrolling pad, move its scroll processors into
its base chain as in [Dedicated scrolling pad](#dedicated-scrolling-pad),
keeping `zip_raw_touch_idle_filter_remote`, and add
`tap-click-while-scrolling` to `raw_touch_remote` to keep taps.

Flash both halves from the same build.

### Wireless splits

Over a Bluetooth split link, the peripheral queues each relayed input
event and sends it as its own notification. A stamped sample is four
events. These settings on the **peripheral** half let one sample's four
notifications go out in a single connection event instead of two with
ZMK's default three buffers. They may reduce the backlog on a busy link;
measure before relying on them. They are harmless on a wired split:

```conf
CONFIG_BT_L2CAP_TX_BUF_COUNT=8
CONFIG_BT_CONN_TX_MAX=8
CONFIG_BT_BUF_ACL_TX_COUNT=8
```

The stamps stay accurate either way: they are taken before the queue.
When the queue (`CONFIG_ZMK_INPUT_SPLIT_MSG_QUEUE_SIZE`, 32 events by
default) is full, it drops its oldest event, which can be a lift-off
sample. That is one reason the pad sends three lift-off samples
(`idle-packets-count = <3>`).

### Wired split timing

On the Go60's polled wired split, these central-side settings shorten the
poll cycle from about 22.5 ms to about 5 ms, so the relayed pad's samples
and the peripheral's keys arrive sooner and without bursts:

```conf
CONFIG_ZMK_SPLIT_WIRED_HALF_DUPLEX_RX_COMPLETE_TIMEOUT=3
CONFIG_ZMK_SPLIT_WIRED_HALF_DUPLEX_RX_TIMEOUT=5
```

They are Go60 tuning, not general defaults; see
[Split link timing](docs/split-timing.md) before using them elsewhere.

## Troubleshooting

**USB works; Bluetooth raw scrolling does not.** Forget the keyboard on
the host, clear its bond with `&bt BT_CLR` on that profile, and pair
again. This is needed after first installing the module and after any
change to the HID report map: adding or removing a pad, changing a pad's
`x-max` or `y-max`, or a protocol change that alters a report. macOS can
keep a stale report map while typing and feature-report reads still
work. If keys work after pairing but a pad does not, disconnect the
keyboard in the Mac's Bluetooth settings and connect it again once
before investigating further.

**Pointer works; RawTouch scrolling does not.** Check RawTouch's
Accessibility permission and that **Use RawTouch Scrolling** is on in its
menu. Then check that the active listener chain contains the scroll
marker, and that the driver sends absolute events and release samples.

**Tap-to-click does nothing.** Set `tap-click`, check that the driver
sends release samples (`idle-packets-count = <3>`), and make sure no
processor in the listener consumes `INPUT_BTN_0`. Touches used for
scrolling never tap unless the pad sets `tap-click-while-scrolling`.

**A pad does not respond, in either mode, while keys work.** The cause
is in the firmware or hardware, not the app: check the driver
properties, the data-ready GPIO and the split relay. Rarely, a pad does
not respond after power-on; switch that half off and on again.

**A relayed pad lags or stutters over a wireless split.** Try the
peripheral's [transmit buffer settings](#wireless-splits) and compare.

**Scrolling stops after RawTouch is force-quit.** Relaunch it, or wait
up to 30 seconds for the lease to lapse and Standard mode to return. A
normal quit returns to Standard mode at once.

### Diagnostics

To check that macOS sees the module's HID collection, independent of
RawTouch:

```sh
ioreg -c IOHIDDevice -r -l
```

Look for a collection with `PrimaryUsagePage` 65280 (`0xFF00`) and
`PrimaryUsage` 1. Other devices may use the same values, and this does
not show that samples arrive.

To watch the lease, build the central half with ZMK's
[USB logging](https://zmk.dev/docs/development/usb-logging) and
`CONFIG_ZMK_RAW_TOUCH_LOG_LEVEL_INF=y`, connect it over USB, and open its
serial console as that page describes; the log level alone prints
nothing. Then start RawTouch. An acquired USB lease logs
`Raw touch lease acquired by USB (timeout 30s)`; releases, disconnects,
endpoint switches and expiry are logged too.

When reporting a problem, include the board, the ZMK/Zephyr, module and
driver revisions, the transport, and whether Standard mode works.

## Converting a relative-mode configuration

If your pad already points and scrolls in relative mode, these are the
changes. The "before" files are the same pads in relative mode: the
driver recognizes taps in hardware, a `tap_to_click` mapper turns its
`INPUT_BTN_TOUCH` into ZMK's left button, and both listener chains apply
the mounting transform. Lines starting with `-` are removed; lines
starting with `+` are added.

<details>
<summary>A pad on the central, or on a unibody keyboard</summary>

<!-- example: examples/trackpad/before.overlay -->
```dts
#include <input/processors.dtsi>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <dt-bindings/zmk/input_transform.h>

#define SCROLL_LAYER 2

&trackpad {
    data-mode = "relative";
    primary-tap-enable;
};

/ {
    input_processors {
        tap_to_click: tap_to_click {
            compatible = "zmk,input-processor-code-mapper";
            #input-processor-cells = <0>;
            type = <INPUT_EV_KEY>;
            map = <INPUT_BTN_TOUCH INPUT_BTN_0>;
        };
    };

    trackpad_listener: trackpad_listener {
        compatible = "zmk,input-listener";
        device = <&trackpad>;
        input-processors = <&tap_to_click>,
                           <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
                           <&zip_xy_scaler 1 1>;

        scroll {
            layers = <SCROLL_LAYER>;
            input-processors = <&tap_to_click>,
                               <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
                               <&zip_xy_to_scroll_mapper>,
                               <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>,
                               <&zip_scroll_scaler 1 8>;
        };
    };
};
```

<!-- example-diff: trackpad.overlay examples/trackpad/before.overlay examples/trackpad/raw-touch.overlay -->
```diff
--- trackpad.overlay (before)
+++ trackpad.overlay (with raw touch)
@@ -1,38 +1,39 @@
 #include <input/processors.dtsi>
+#include <raw_touch/processors.dtsi>
 #include <zephyr/dt-bindings/input/input-event-codes.h>
 #include <dt-bindings/zmk/input_transform.h>

 #define SCROLL_LAYER 2

 &trackpad {
-    data-mode = "relative";
-    primary-tap-enable;
+    data-mode = "absolute";
+    idle-packets-count = <3>;
+    /delete-property/ primary-tap-enable;
 };

 / {
-    input_processors {
-        tap_to_click: tap_to_click {
-            compatible = "zmk,input-processor-code-mapper";
-            #input-processor-cells = <0>;
-            type = <INPUT_EV_KEY>;
-            map = <INPUT_BTN_TOUCH INPUT_BTN_0>;
-        };
+    raw_touch_pad: raw_touch_pad {
+        compatible = "zmk,raw-touch-pad";
+        device = <&trackpad>;
+        pad-id = <0>;
+        rotate-90;
+        y-invert;
+        tap-click;
     };

     trackpad_listener: trackpad_listener {
         compatible = "zmk,input-listener";
         device = <&trackpad>;
-        input-processors = <&tap_to_click>,
-                           <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
-                           <&zip_xy_scaler 1 1>;
+        input-processors = <&zip_xy_scaler 1 4>,
+                           <&zip_raw_touch_idle_filter>;

         scroll {
             layers = <SCROLL_LAYER>;
-            input-processors = <&tap_to_click>,
-                               <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
+            input-processors = <&zip_raw_touch_scroll>,
                                <&zip_xy_to_scroll_mapper>,
                                <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>,
-                               <&zip_scroll_scaler 1 8>;
+                               <&zip_scroll_scaler 1 32>,
+                               <&zip_raw_touch_idle_filter>;
         };
     };
 };
```

</details>

<details>
<summary>A pad on a split peripheral</summary>

Peripheral before:

<!-- example: examples/split/peripheral-before.overlay -->
```dts
&remote_trackpad {
    data-mode = "relative";
    primary-tap-enable;
};

&trackpad_split {
    device = <&remote_trackpad>;
};

&remote_trackpad_listener {
    status = "disabled";
};
```

<!-- example-diff: peripheral.overlay examples/split/peripheral-before.overlay examples/split/peripheral.overlay -->
```diff
--- peripheral.overlay (before)
+++ peripheral.overlay (with raw touch)
@@ -1,10 +1,22 @@
 &remote_trackpad {
-    data-mode = "relative";
-    primary-tap-enable;
+    data-mode = "absolute";
+    idle-packets-count = <3>;
+    /delete-property/ primary-tap-enable;
+};
+
+/ {
+    input_processors {
+        zip_raw_touch_split_stamp: zip_raw_touch_split_stamp {
+            compatible = "zmk,input-processor-raw-touch-split-stamp";
+            #input-processor-cells = <1>;
+        };
+    };
 };

 &trackpad_split {
     device = <&remote_trackpad>;
+    /* The cell is the relay's own reg (trackpad_split@0 -> 0). */
+    input-processors = <&zip_raw_touch_split_stamp 0>;
 };

 &remote_trackpad_listener {
```

Central before:

<!-- example: examples/split/central-before.overlay -->
```dts
#include <input/processors.dtsi>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <dt-bindings/zmk/input_transform.h>

#define SCROLL_LAYER 2

&trackpad_split {
    /delete-property/ device;
};

/ {
    input_processors {
        remote_tap_to_click: remote_tap_to_click {
            compatible = "zmk,input-processor-code-mapper";
            #input-processor-cells = <0>;
            type = <INPUT_EV_KEY>;
            map = <INPUT_BTN_TOUCH INPUT_BTN_0>;
        };
    };
};

&remote_trackpad_listener {
    status = "okay";
    device = <&trackpad_split>;
    input-processors = <&remote_tap_to_click>,
                       <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
                       <&zip_xy_scaler 1 1>;

    scroll {
        layers = <SCROLL_LAYER>;
        input-processors = <&remote_tap_to_click>,
                           <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
                           <&zip_xy_to_scroll_mapper>,
                           <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>,
                           <&zip_scroll_scaler 1 8>;
    };
};
```

<!-- example-diff: central.overlay examples/split/central-before.overlay examples/split/central.overlay -->
```diff
--- central.overlay (before)
+++ central.overlay (with raw touch)
@@ -1,4 +1,5 @@
 #include <input/processors.dtsi>
+#include <raw_touch/processors.dtsi>
 #include <zephyr/dt-bindings/input/input-event-codes.h>
 #include <dt-bindings/zmk/input_transform.h>

@@ -9,12 +10,19 @@
 };

 / {
+    raw_touch_remote: raw_touch_remote {
+        compatible = "zmk,raw-touch-pad";
+        device = <&trackpad_split>;
+        pad-id = <1>; /* Local pad uses 0. */
+        rotate-90;
+        y-invert;
+        tap-click;
+    };
+
     input_processors {
-        remote_tap_to_click: remote_tap_to_click {
-            compatible = "zmk,input-processor-code-mapper";
+        zip_raw_touch_idle_filter_remote: zip_raw_touch_idle_filter_remote {
+            compatible = "zmk,input-processor-raw-touch-idle-filter";
             #input-processor-cells = <0>;
-            type = <INPUT_EV_KEY>;
-            map = <INPUT_BTN_TOUCH INPUT_BTN_0>;
         };
     };
 };
@@ -22,16 +30,15 @@
 &remote_trackpad_listener {
     status = "okay";
     device = <&trackpad_split>;
-    input-processors = <&remote_tap_to_click>,
-                       <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
-                       <&zip_xy_scaler 1 1>;
+    input-processors = <&zip_xy_scaler 1 4>,
+                       <&zip_raw_touch_idle_filter_remote>;

     scroll {
         layers = <SCROLL_LAYER>;
-        input-processors = <&remote_tap_to_click>,
-                           <&zip_xy_transform (INPUT_TRANSFORM_XY_SWAP | INPUT_TRANSFORM_Y_INVERT)>,
+        input-processors = <&zip_raw_touch_scroll>,
                            <&zip_xy_to_scroll_mapper>,
                            <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>,
-                           <&zip_scroll_scaler 1 8>;
+                           <&zip_scroll_scaler 1 32>,
+                           <&zip_raw_touch_idle_filter_remote>;
     };
 };
```

</details>

In both cases the listener transforms move to the raw-touch node as
`rotate-90` and `y-invert`, `tap-click` replaces `primary-tap-enable` and
the `tap_to_click` mapper, and the scalers are divided by about 4:
multiply a relative-mode pointer scaler by about 1/4 (`1 1` becomes
`1 4`) and a wheel divisor by about 4 (`1 8` becomes `1 32`). The factor
is 4.2 on a Go60's 35 mm Pinnacle pad; other pads and sensitivities may
differ, so tune from there. Expect a faster pointer and a busier wheel
until you rescale. Remove any
processor that discards `INPUT_BTN_0`, such as a `zip_button_behaviors`
instance mapping it to `&none`: the module's taps arrive as that button.

## Driver requirements

The module works with any driver that produces this input:

- `INPUT_ABS_X`, `INPUT_ABS_Y` and `INPUT_ABS_Z` events, with the sync
  flag on the final Z event.
- Samples while a finger stays down, including while it is still.
- A release sample with X, Y and Z all zero. The module detects lift-off
  this way; a driver that reports contact differently needs adapting.
- Untransformed coordinates. Declare the mounting on the pad node.

Property names vary between drivers. Keep your board's bus and GPIO
assignments, delete the properties the new driver does not declare, and
set the ones it needs.

## Configuration reference

### Pad properties

Each `zmk,raw-touch-pad` node describes one pad. The
[binding](dts/bindings/input/zmk,raw-touch-pad.yaml) has the full
definitions.

| Property | Default | Meaning |
|---|---|---|
| `device` | required | The absolute-input device, or a split relay on the central. |
| `pad-id` | required | Unique ID from 0 to 7. A duplicate or out-of-range ID fails the build, and so do more than 8 pads. |
| `x-max` / `y-max` | `2047` / `1535` | Maximum raw coordinates, 1 to 32767; set these for your sensor. |
| `resolution` | `38` | Sensor counts/mm, 0 to 255; `0` means unknown. |
| `rotate-90` | absent | Swap the X and Y axes for the pointer, and tell the host. |
| `x-invert` / `y-invert` | absent | Invert that pointer axis after the swap, and tell the host. |
| `tap-click` | absent | Firmware tap-to-click. |
| `tap-click-while-scrolling` | absent | Also report taps for touches used for scrolling (dedicated scrolling pads); held for host confirmation in RawTouch mode. Without `tap-click`, the build fails. |
| `tap-max-ms` | `180` | Longest touch counted as a tap; at most 5000. |
| `tap-max-movement` | `30` | Farthest a tap may move from touch-down on either raw axis, in counts; at most 32767. |

Geometry defaults describe a Cirque Pinnacle; override them for other
hardware. Values outside these ranges fail the build. Samples carry raw
coordinates; the orientation flags say how to read them.

`rotate-90` only swaps the axes, which on its own mirrors the pad. A
90° rotation is the swap plus one invert, as in the example's `rotate-90`
with `y-invert`; invert the other axis to rotate the other way.

The HID report map declares one pad's coordinate ranges, so changing
`x-max` or `y-max` changes the report map, and Bluetooth hosts need a
[fresh pairing](#troubleshooting).

Pad IDs need only be unique within one keyboard. RawTouch keeps
keyboards apart and can apply
[settings per keyboard](https://github.com/kalakris/rawtouch/blob/main/docs/configuration.md#per-keyboard-overrides).

### Kconfig

Names below drop the `CONFIG_` prefix used in a `.conf` file.

| Symbol | Default | Purpose |
|---|---|---|
| `ZMK_RAW_TOUCH` | `y` when a pad node and `ZMK_POINTING` exist | Enable the module. |
| `ZMK_RAW_TOUCH_USB` | `y` on the central with USB | Raw reports over the second USB HID interface. |
| `ZMK_RAW_TOUCH_BLE` | `y` on the central with Bluetooth | Raw reports over the second Bluetooth HID service. |
| `ZMK_RAW_TOUCH_USB_QUEUE_SIZE` | `4` | Buffered USB samples; 2 to 255. |
| `ZMK_RAW_TOUCH_BLE_QUEUE_SIZE` | `8` | Buffered Bluetooth samples; 2 to 255. |
| `ZMK_RAW_TOUCH_LOG_LEVEL_*` | inherited | Module log level. |

The marker, idle-filter and stamp processors turn on by themselves when
their nodes are used. Queue sizes count samples across all pads. Larger
queues absorb bursts but hold older input longer. Turning off only
`ZMK_RAW_TOUCH_BLE` keeps Standard-mode pointing and scrolling over
Bluetooth and raw reports over USB; use it if a host ignores the second
Bluetooth HID service.

## Compatibility

Tested on a MoErgo Go60 with two Cirque Pinnacle pads, with RawTouch on
macOS 26 over USB and Bluetooth, with the halves wired and wireless.

| Component | Tested revision |
|---|---|
| `moergo-sc/zmk` (ZMK 0.3 / Zephyr 3.5) | Release `v26.09`, in the [Go60 configuration](https://github.com/kalakris/go60-rawtouch-config) and in this repository's CI |
| [`kalakris/cirque-input-module`](https://github.com/kalakris/cirque-input-module) | `v0.1.0`: Zephyr's Pinnacle driver packaged for Zephyr 3.5, with small patches for status handling, edge sensitivity and calibration at start-up |

CI builds the examples for both Go60 halves in their stock roles: the
left half as central with the local pad, the right half as peripheral
relaying its pad.

Not yet tested: other keyboards and trackpads, upstream ZMK `v0.3`,
battery life, and deep sleep and wake. ZMK `main` (Zephyr 4.1) and
`USB_DEVICE_STACK_NEXT` need a port. Each pad reports one finger:
Pinnacle pads detect a single contact. Multi-touch pads such as the
Azoteq TPS43 are in scope but would need firmware and host work, and
possibly protocol extensions.

Test reports are welcome: open an
[issue](https://github.com/kalakris/zmk-raw-touch/issues) with your
hardware, firmware and OS versions, connection type, and what worked or
failed.

## Versioning

Releases are tagged `v0.y.z`. The module is pre-1.0, so a minor release
may change interfaces. The moving tag `stable` points at the newest
release: the release job moves it to each released tag that has no
pre-release suffix and is the highest version so far.
[`include/zmk/raw_touch/version.h`](include/zmk/raw_touch/version.h) holds
the number: the firmware reports it to the host, and CI rejects a `v*` tag
that disagrees with it.

The protocol version (4) is the only compatibility contract with a host;
the module version is diagnostic. RawTouch's
[compatibility table](https://github.com/kalakris/rawtouch#firmware-compatibility)
lists which app versions speak which protocol.

A change to the HID report map, including adding or removing a pad,
needs a fresh Bluetooth pairing on hosts that cache the map, macOS among
them. Release notes say when an update does this.

## Which repository?

| You have | Start here |
|---|---|
| A MoErgo Go60 | [go60-rawtouch-config](https://github.com/kalakris/go60-rawtouch-config): prebuilt firmware, or a configuration to fork |
| Another ZMK keyboard with a Cirque Pinnacle trackpad | [zmk-raw-touch](https://github.com/kalakris/zmk-raw-touch) (this repository): the firmware module and its setup guide |
| Firmware with the module, and a Mac | [RawTouch](https://github.com/kalakris/rawtouch): the macOS app |
| A question about the trackpad driver | [cirque-input-module](https://github.com/kalakris/cirque-input-module): the absolute-mode Pinnacle driver the module is tested with |

## Implementation and contributions

The sources compile into ZMK's `app` target to use its private headers.
The USB and Bluetooth code registers a separate vendor collection with
transports derived from ZMK's HID implementation; no ZMK file is
modified.

Useful contributions include tested board configurations, ports to newer
ZMK and Zephyr trees, and host implementations for other operating
systems; see [CONTRIBUTING](CONTRIBUTING.md). Wire-format changes need
coordinated firmware and host work, and can require Bluetooth
re-pairing.

## Credits and license

[MIT](LICENSE), including the [protocol specification](docs/protocol.md).
The transport code derives from ZMK. The module approach follows
[`zzeneg/zmk-raw-hid`](https://github.com/zzeneg/zmk-raw-hid) and
[`badjeff/zmk-hid-io`](https://github.com/badjeff/zmk-hid-io).
The pad integration builds on Zephyr's Pinnacle driver and Pete
Johanson's Cirque work. RawTouch credits its
[LinearMouse](https://github.com/linearmouse/linearmouse) origins
separately.
