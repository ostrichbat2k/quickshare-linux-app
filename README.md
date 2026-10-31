# Quick Share for Linux — app

A Linux desktop app built from scratch on top of the Linux platform backend
in [`ostrichbat2k/nearby`](https://github.com/ostrichbat2k/nearby) (a fork
of `google/nearby` with a native Linux implementation of BLE, GATT, and
WiFi LAN mediums, plus Crypto via BoringSSL).

This repo is the **consumer**: it links against that library and provides
the actual user-facing app (discovery UI, send/receive flows, device
pairing, etc.). The library itself stays in the `nearby` fork and is
pulled in here as a submodule pinned to its `release/linux` branch.

## Status

Scaffolding only. The library side (`third_party/nearby`) already has:

- BLE/BlueZ GATT transport (adapter, advertising, client, server)
- A Weave BLE socket layer
- Crypto via BoringSSL
- A WiFi LAN medium via Avahi + POSIX TCP
- A first real end-to-end discover+send file transfer to a phone (CLI-level, no UI)

WiFi Direct is not implemented yet (tracked in the library repo).

Nothing in this app repo is implemented yet. See issues for the current
plan.

## Getting the code

```
git clone --recurse-submodules https://github.com/ostrichbat2k/quickshare-linux-app.git
```

## Updating the library pin

```
cd third_party/nearby
git fetch origin
git checkout origin/release/linux
cd ../..
git add third_party/nearby
git commit -m "Bump nearby library pin"
```

## Workflow

Issue/feature-branch/PR against `main`, same as the library repo uses
`release/linux` as its integration branch. See open issues for current
scope decisions (UI toolkit, packaging, etc.) that are still undecided.
