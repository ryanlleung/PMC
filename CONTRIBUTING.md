# Contributing

## Branches

- `main` holds the released firmware. It always builds and passes the checks below.
- Each change is made on its own short branch from `main` and merged back through a pull request. The branch is deleted after the merge. There is no long-lived development branch.
- Each firmware release is a tag on `main`, named after the version: `v0.18.0`.

## Versions

The firmware version is `IPC_FW_VERSION` in `firmware/ipc_config.h`. A change to the firmware raises it, and its commit title starts with the new version: `0.18.1: ...`. The simulator bundle (`simulator/ipc-wasm.js`) is rebuilt and committed with every firmware change, so it always matches the source beside it.

## Checks before a merge

| Check | Command |
| --- | --- |
| Control, settings, Modbus and barometer tests | `tools/ctrl-test/run.sh`, `tools/cfg-test/run.sh`, `tools/modbus-test/run.sh`, `tools/baro-test/run.sh` |
| Simulator rebuild and tests | `simulator/wasm/build.sh`, then `node simulator/tests/run.js`, `remote-only.js`, `tcp-board-test.js`, `page.js`, `text-fit.js` and `label-jitter.js` in `simulator/tests/` |
| Frappy driver | `python secop/tests/run.py` and `python secop/tests/run.py --sim` |
| Screen previews and cross-build | `tools/ui-preview/build.sh`, `tools/xbuild/build.sh` |

`tools/README.md` and `simulator/README.md` say what each one needs and checks.

## Documents

A change in behaviour is described in the same pull request: the operation manual for what the operator sees, `docs/text-port-commands.md` and `docs/modbus-register-map.md` for the network, and the screen captures in `docs/screens/` for a changed screen. Documents describe the firmware as it is; the history is in git.
