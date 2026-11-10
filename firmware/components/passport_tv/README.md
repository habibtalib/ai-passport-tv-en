# Shared Passport TV player

This component is the only implementation of the TV receiver, full-frame ring,
decoder, audio clock and menu used by FoloTalk and the standalone application.
See the [standalone application](../../../README.md) for setup and
export. The component retains the existing `ft_tv_*` API; `passport_tv.h` also
provides product-neutral aliases.

The host initializes Wi-Fi, BSP display/audio/buttons and NVS, then supplies
DMA stripes, a 10 KiB control/frame workspace and 8–24 KiB extra frame storage.
It must release all other panel/audio owners before starting and wait for
`done` before reclaiming memory. `prepare` optionally retires the host's idle
workspace; `settings` receives volume/brightness. `allocation_failed` is an
optional nonallocating, nonblocking host OOM recorder, restored on release.
A standalone host uses the built-in no-op fallback when no recorder exists.

The `ticket` field is the authenticated HELLO payload: FoloTalk supplies its
signed ticket; standalone supplies its pairing code. Authentication policy stays
in the server entry, not the media decoder. No credentials, account operations,
provisioning or Admin networking belong to this component.

CMake preserves the accepted `-O2` files and targeted receive/ACK IRAM placement.
`PASSPORT_TV_FONT_DIR` selects bitmap resources at build time. The in-tree
default retains FoloTalk's existing layout; the standalone application/export
selects the attributed OFL font. Never publish historical font blobs without
checking their source; the allowlisted exporter replaces them explicitly.
