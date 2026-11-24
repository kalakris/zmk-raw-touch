# Split link timing

How a pad on a split peripheral reaches the computer, and the two
settings the README suggests for it. The settings themselves are in the
README under [Wireless splits](../README.md#wireless-splits) and
[Wired split timing](../README.md#wired-split-timing).

## Sample timestamps

A Pinnacle pad reports about every 10 ms. On the central, each sample
gets a timestamp, and RawTouch derives finger velocity from those
timestamps. A relayed sample would otherwise be stamped when it arrives
on the central, after the split hop, so the link's delivery jitter would
show up as velocity noise.

The `zmk,input-processor-raw-touch-split-stamp` processor on the
peripheral's relay sends the peripheral's own sample time with each
sample, as one extra input event. The central uses it in place of its
own clock with no configuration, and nothing changes in what the
computer receives. With the stamp, delivery jitter only adds latency,
which RawTouch measures per pad and connection.

## Wireless split links

Over a Bluetooth split, the peripheral puts each relayed input event in
a queue (`CONFIG_ZMK_INPUT_SPLIT_MSG_QUEUE_SIZE`, 32 events by default)
and the split service's work queue sends each as its own notification.
A stamped sample is four events (X, Y, stamp, Z). The stamp is taken
before the queue, so a slow link adds latency, not timestamp error.
With the default three transmit buffers, a sample's fourth notification
waits for the next connection event; eight buffers let it go out in the
same one. That may reduce the backlog on a busy link; measure it on
yours:

```conf
CONFIG_BT_L2CAP_TX_BUF_COUNT=8
CONFIG_BT_CONN_TX_MAX=8
CONFIG_BT_BUF_ACL_TX_COUNT=8
```

They are harmless on a wired split. When the queue is full, it drops
its oldest event to make room. A dropped lift-off sample is covered by
the pad's three lift-off samples (`idle-packets-count = <3>`).

## Wired split links

The Go60's wired split link is half-duplex and polled: the peripheral
queues input until the central asks for it. At ZMK's default timeouts a
poll cycle measured about 22.5 ms on a Go60, so the relayed pad's 10 ms
samples arrive in bursts of two or three, and the peripheral's key
events wait for the next poll too. These central-side settings
shortened the cycle to about 5 ms:

```conf
CONFIG_ZMK_SPLIT_WIRED_HALF_DUPLEX_RX_COMPLETE_TIMEOUT=3
CONFIG_ZMK_SPLIT_WIRED_HALF_DUPLEX_RX_TIMEOUT=5
```

Changing only these needs a reflash of the central alone. Treat them as
Go60 tuning, not as defaults for every split keyboard: more frequent
polling may cost battery on both halves, which has not been measured.
Check typing as well as the trackpad after changing them.
