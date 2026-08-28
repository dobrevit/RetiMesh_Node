# retimesh-flash

Terminal installer for [RetiMesh Node](https://github.com/dobrevit/RetiMesh_Node)
firmware — the `rnodeconf --autoinstall` equivalent.

```sh
pipx run --spec "git+https://github.com/dobrevit/RetiMesh_Node#subdirectory=tools/retimesh-flash" retimesh-flash install
```

or, installed:

```sh
pipx install "git+https://github.com/dobrevit/RetiMesh_Node#subdirectory=tools/retimesh-flash"
retimesh-flash list                       # boards in the latest release
retimesh-flash ports                      # serial ports that look like an ESP32
retimesh-flash install                    # interactive: pick board + port
retimesh-flash install --board t3s3 --port /dev/ttyACM0 --version v1.0.0
retimesh-flash install --mode app         # firmware only, keeps settings + web app
retimesh-flash install --mode fs          # web app only
retimesh-flash install --file bundle.zip  # offline, from a downloaded release archive
retimesh-flash devices                    # every node that answers: serial consoles and /api/status
retimesh-flash bootloader --port /dev/ttyACM0   # restart a running node into its ROM downloader
retimesh-flash bootloader --ip 10.42.0.1        # ...over HTTP (admin password with --password)
retimesh-flash install --serial 7C:DF:A1:12:34:56   # pick the port by USB serial number
```

`install` asks a running node for its bootloader first (the maintenance
console on the port, `BOOTLOADER CONFIRM`), so no BOOT button; esptool's own
DTR/RTS reset is the fallback and `--no-handoff` skips the request. Afterwards
it waits for the application to answer `VERSION` again. `retimesh_flash.device`
is the module behind all of this, shared with the PlatformIO upload hook and
the HIL scripts.

Every downloaded part is verified against the SHA-256 in `release.json`
before anything is written to flash.
