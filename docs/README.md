# RetiMesh Node documentation

RetiMesh Node turns a LilyGO T3-S3 (or a similar ESP32-S3 + LoRa board) into a
standalone **Reticulum transport node**: it routes for the mesh over LoRa, and
anyone who joins its Wi-Fi is on the mesh with Sideband — no host computer.

| Start here | |
|---|---|
| [Getting started](getting-started.md) | flash, join, connect Sideband, first message |
| [Architecture](architecture.md) | what runs where, packet flow, storage |
| [Configuration](configuration.md) | every setting, default and build flag |
| [Reticulum integration](reticulum.md) | interfaces, transport modes, announces, identity, rnsd/RNode interop |
| [Hardware](hardware.md) | supported boards, pins, adding a board |
| [HTTP API](api.md) | `/api/status`, `/api/board`, `/api/settings/*` |
| [Examples](examples/) | rnsd config, Python listeners, remote node setup, curl |
| [Troubleshooting](troubleshooting.md) | the things that actually go wrong |
| [Development](development.md) | build, CI, releases, debugging, contributing |

Versions: this documentation describes **v0.0.3**. The web flasher at
<https://dobrevit.github.io/RetiMesh_Node/> always carries the latest
published release.
