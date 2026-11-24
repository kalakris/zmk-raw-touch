# Examples

The configuration files the main README walks through. They are the source
of truth for its code blocks: `scripts/check-readme-examples.py` fails CI
if a README block differs from its file (the README's diff blocks are
generated from the `before`/`after` pairs), and the CI firmware build
compiles the "after" files on a MoErgo Go60 against the pinned trees in
[`ci/`](../ci).

The files use the README's generic labels (`trackpad`, `trackpad_split`,
`remote_trackpad_listener`, ...). Rename them to your board's nodes, or map
them with `#define` as `ci/go60_lh.keymap` and `ci/go60_rh.keymap` do.

| File | README section | Compiled by CI |
|---|---|---|
| `trackpad/raw-touch.overlay` | [3. Register the pad and mark the scrolling chain](../README.md#3-register-the-pad-and-mark-the-scrolling-chain) | `go60_lh` |
| `trackpad/before.overlay` | [Converting a relative-mode configuration](../README.md#converting-a-relative-mode-configuration) | no (the relative-mode starting point) |
| `trackpad/dedicated-scroll.overlay` | [Dedicated scrolling pad](../README.md#dedicated-scrolling-pad) | `go60_lh`, on top of the above |
| `split/shared.dtsi` | [Shared definitions](../README.md#shared-definitions) | no (the Go60 board defines these nodes itself) |
| `split/peripheral.overlay` | [Peripheral: send absolute samples](../README.md#peripheral-send-absolute-samples) | `go60_rh` |
| `split/central.overlay` | [Central: process the relayed samples](../README.md#central-process-the-relayed-samples) | `go60_lh` |
| `split/peripheral-before.overlay` | [Converting a relative-mode configuration](../README.md#converting-a-relative-mode-configuration) | no |
| `split/central-before.overlay` | [Converting a relative-mode configuration](../README.md#converting-a-relative-mode-configuration) | no |

The "before" files exist so the README's diffs are generated rather than
hand-maintained; they are not built.
