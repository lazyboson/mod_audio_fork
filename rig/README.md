# Integration rig

Docker-compose harness for exercising the module against a controllable
WebSocket peer (DESIGN.md §10, decision 17).

| Piece | Role |
|---|---|
| `mock_ws_server/server.py` | Stdlib-only WS server. Validates the wire protocol, measures audio per connection, streams playback back, writes a JSON report, and injects faults (`MOCK_WS_STALL_AFTER`, `MOCK_WS_DROP_AFTER`). |
| `conf/` | The whole FreeSWITCH configuration the rig runs on. Alpine's `freeswitch` package ships none, so the rig mounts this tree over `/etc/freeswitch`: `mod_audio_fork` plus the six modules the smoke needs, an event socket on 127.0.0.1:8021, and the dialplan. `mod_sofia` and `mod_verto` stay unloaded (DESIGN.md §13). |
| `conf/dialplan/default.xml` | `rig-tone` extension: answers and plays a continuous tone so forked audio is deterministic. |
| `smoke.sh` | Starts calls, forks them, asserts the mock server saw hello + audio of the right rate and loudness and that server audio reached the caller, tears down, asserts counters return to zero. |
| `docker-compose.yml` | Wires FreeSWITCH (with the module), the mock server, and the smoke together. |

The SIPp call generator belongs to the nightly load tier and is not in the rig
yet.

## Status — 50-call smoke green

`CALLS=50`, one `uuid_audio_fork start` per call, 5 s of streaming, then
`stop` + `uuid_kill`. Three consecutive local runs passed; the module reported
50 forks, 8.06 MB sent and 3.2 MB of server audio played, and returned to zero
forks and zero leased slabs after teardown.

`smoke.sh` fails on the first violation of any of these, printing the value it
saw:

- every `uuid_audio_fork start` returns `+OK`, and `forks` equals `CALLS`
- the mock server saw one connection and one `hello` per call, with
  `rate` 16000, `channels` 1, `encoding` `L16`, and the `{"rig":true}` metadata
  the script passed
- no protocol errors (unparseable JSON, audio before hello, odd-length frames)
- per connection: at least the 5 s the script asked for, at least 70% of the
  160 KB that window implies, 32000 B/s ±30%, and a mean absolute sample well
  clear of silence
- `playback_bytes_played` is non-zero while streaming, and the mock sent
  playback on every connection
- after teardown: zero forks and zero leased slabs

The byte envelope is asserted as a rate rather than as a total because the
script starts and stops the forks in serial `fs_cli` calls; that adds a few
hundred milliseconds to every connection's window on a loaded runner, which a
fixed total would either tolerate too loosely or fail on spuriously.

## Running the smoke locally

```sh
docker compose -f rig/docker-compose.yml up --build \
  --abort-on-container-exit --exit-code-from smoke
docker compose -f rig/docker-compose.yml down -v
```

| Knob | Where | Default | Effect |
|---|---|---|---|
| `CALLS` | compose environment | 50 | concurrent calls the smoke originates |
| `STREAM_SECONDS` | `smoke.sh` | 5 | how long each fork streams before the counters are read |
| `MOCK_WS_PLAYBACK_MS` | compose environment | 2000 | after `hello`, the mock streams this many ms of 16 kHz mono L16 tone back in 20 ms frames at real-time pace, then a `rig-end` mark. 0 disables playback. |
| `MOCK_WS_STALL_AFTER` | compose environment | off | stop reading after N audio frames |
| `MOCK_WS_DROP_AFTER` | compose environment | off | close the connection after N audio frames |

`CALLS=1 docker compose ...` runs the single-call path.

## Known gaps the rig found

- The module never sends `{"type":"bye"}`: the frame is queued and the socket
  is closed in the same pump, and the writable callback closes before it
  drains the queue. The mock still counts `bye_count`, but the smoke does not
  assert on it.
- FreeSWITCH exits 139 on shutdown once a fork has run in the process, whether
  or not the fork was stopped first. Loading the module without starting a
  fork, and placing a rig call without forking it, both shut down cleanly. It
  does not affect the smoke's exit code (`--exit-code-from smoke`).
