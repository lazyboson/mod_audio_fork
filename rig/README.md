# Integration rig

Docker-compose harness for exercising the module against a controllable
WebSocket peer (DESIGN.md §10, decision 17).

| Piece | Role |
|---|---|
| `mock_ws_server/server.py` | Stdlib-only WS server. Validates the wire protocol, counts audio, writes a JSON report, and injects faults (`MOCK_WS_STALL_AFTER`, `MOCK_WS_DROP_AFTER`). |
| `dialplan/rig.xml` | `rig-tone` extension: answers and plays a continuous tone so forked audio is deterministic. |
| `sipp/uac_tone.xml` | SIPp UAC scenario for the call-volume tier. |
| `smoke.sh` | Starts a call, forks it, asserts the mock server saw hello + audio, tears down, asserts counters return to zero. |
| `docker-compose.yml` | Wires FreeSWITCH (with the module), the mock server, and SIPp together. |

## Status — not yet green

The rig is scaffolding: **the 50-call smoke has not been run end to end yet**, so
M3's "50-call smoke green in CI" exit criterion is not met. What has been
verified is narrower and listed in the root README's Status section. Finishing
the rig is the first task of M5, where the nightly load tier and the soak gate
also land.

## Running the smoke locally

```sh
docker compose -f rig/docker-compose.yml up --build --abort-on-container-exit
```

`smoke.sh` exits non-zero on any protocol error the mock server recorded, on
missing audio, or on a non-zero fork count after teardown.
