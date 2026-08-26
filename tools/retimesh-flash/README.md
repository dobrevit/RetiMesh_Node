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
```

Every downloaded part is verified against the SHA-256 in `release.json`
before anything is written to flash.
