# Integration rig

Docker-compose harness for exercising the module against a controllable
WebSocket peer (DESIGN.md §10, decision 17).

| Piece | Role |
|---|---|
| `mock_ws_server/server.py` | Stdlib-only WS server. Validates the wire protocol, measures audio per connection, streams playback back, writes a JSON report, and injects faults (`MOCK_WS_STALL_AFTER`, `MOCK_WS_DROP_AFTER`). |
| `conf/` | The whole FreeSWITCH configuration the rig runs on. Alpine's `freeswitch` package ships none, so the rig mounts this tree over `/etc/freeswitch`: `mod_audio_fork` plus the six modules the smoke needs, an event socket on 127.0.0.1:8021, and the dialplan. `mod_sofia` and `mod_verto` stay unloaded (DESIGN.md §13). |
| `conf/dialplan/default.xml` | `rig-tone` extension: answers and plays a continuous tone so forked audio is deterministic. |
| `smoke.sh` | Starts calls, forks them, asserts the mock server saw hello + audio of the right rate and loudness and that server audio reached the caller, tears down, asserts the graceful bye and that counters return to zero. |
| `freeswitch-entrypoint.sh` | Substitutes the container-local test-CA path into the repo's `conf/audio_fork.conf.xml` before starting FreeSWITCH, then execs it. |
| `docker-compose.yml` | Wires FreeSWITCH (with the module), the mock servers, and the smoke together. |

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
- after `uuid_audio_fork stop` and before any `uuid_kill`, the mock counted one
  `{"type":"bye"}` per call
- after teardown: zero forks and zero leased slabs

The byte envelope is asserted as a rate rather than as a total because the
script starts and stops the forks in serial `fs_cli` calls; that adds a few
hundred milliseconds to every connection's window on a loaded runner, which a
fixed total would either tolerate too loosely or fail on spuriously.

## Running the smoke locally

```sh
./rig/run_smoke.sh
```

`run_smoke.sh` is the only entry point, used by CI too. It brings the compose
stack up, waits for the smoke to finish, waits for FreeSWITCH to exit (the
smoke's last act is `fsctl shutdown`), asserts both exit codes are 0, writes
the full rig logs to `rig-logs.txt`, and tears the stack down with `down -v`.

It names the compose project and the built image after the worktree
(`afrig-<checkout>`), so two checkouts of this repo on one host do not share
containers or overwrite each other's image; `COMPOSE_PROJECT_NAME` and
`AUDIOFORK_IMAGE` override that.

| Knob | Where | Default | Effect |
|---|---|---|---|
| `CALLS` | compose environment | 50 | concurrent calls the smoke originates |
| `STREAM_SECONDS` | `smoke.sh` | 5 | how long each fork streams before the counters are read |
| `MOCK_WS_PLAYBACK_MS` | compose environment | 2000 | after `hello`, the mock streams this many ms of 16 kHz mono L16 tone back in 20 ms frames at real-time pace, then a `rig-end` mark. 0 disables playback. |
| `MOCK_WS_STALL_AFTER` | compose environment | off | stop reading after N audio frames |
| `MOCK_WS_DROP_AFTER` | compose environment | off | close the connection after N audio frames |

`CALLS=1 docker compose ...` runs the single-call path.

## The TLS leg

`mock-wss` is the same `server.py` serving TLS on 9443. The last block of
`smoke.sh` forks one call to `wss://mock-wss:9443/` and asserts the TLS mock
saw exactly one `hello` and non-zero audio; the blocks before it are unchanged
and still run over plaintext.

Its credentials are `net/tests/tls/`, bind-mounted straight in, so
`net/tests/tls/regen.sh` stays the repo's only certificate generator and the
unit suite and the rig cannot drift apart. That is why the test server
certificate carries `DNS:mock-wss` in its SAN.

FreeSWITCH does not expand `$${vars}` inside a `param` value, so the CA path
cannot be injected from the rig's `freeswitch.xml`. Rather than fork the
shipped config, the rig mounts it as `/srv/audio_fork.conf.xml.in` and
`freeswitch-entrypoint.sh` rewrites the one empty `tls-ca-file` value. That
script fails loudly if the param ever stops matching, so the rig cannot
silently fall back to the OS trust store and then pass for the wrong reason.

## Known gaps the rig found

- FreeSWITCH 1.10.12 on musl SIGSEGVs at shutdown once any session in the
  process has carried an `SMBF_WRITE_REPLACE` media bug — the playback path's
  bug, but equally stock `uuid_displace` with `mod_audio_fork` not loaded at
  all. The fault is in `switch_core_session_thread_pool_worker` destroying its
  own memory pool, so the rig sets `session-thread-pool=false`, which gives
  every session its own thread and removes the faulting path. `run_smoke.sh`
  asserts the FreeSWITCH exit code so a regression cannot pass unnoticed.

## Load tier

`rig/load/run_load.sh` is the nightly tier (DESIGN.md §10, decision 17): SIPp
drives `TOTAL_CALLS` through `mod_sofia` at `CONCURRENT` concurrency, the
`load` dialplan context forks every call, and the run ends on the leak
assertions. It uses `docker-compose.load.yml` on top of the base file, which
adds SIPp and toxiproxy and puts the PR smoke behind a profile.

The baseline is taken **at idle after a warm-up wave**, and the final sample is
also at idle. Comparing an idle sample against one taken under traffic measures
concurrency, not a leak: at 100 concurrent forks RSS is ~1.1 GB and 328 fds
against ~130 MB and 28 at rest.

Sampling runs inside the FreeSWITCH container (`rig/load/sampler.sh`). One
`docker compose exec` per value costs tens of seconds once the box is loaded,
which dates every row minutes after the counters in it were read and makes a
prompt drain look like a stall.

| Knob | Default | Effect |
|---|---|---|
| `CONCURRENT` | 1000 | calls held open at once |
| `TOTAL_CALLS` | 10000 | calls the run churns through |
| `WARMUP_CALLS` | 200 | calls driven before the baseline is taken |
| `CALL_RATE` | 50 | calls per second SIPp offers |
| `HOLD_SECONDS` | 60 | rounded up to whole 7 s pcap plays |
| `MAX_FAILED_CALLS` | 0 | SIPp failed calls tolerated |
| `RSS_TOLERANCE_PERCENT` | 5 | band around the idle RSS baseline |
| `FD_TOLERANCE` | 10 | band around the idle fd baseline |
| `SAMPLE_SECONDS` | 5 | sampler interval |

It fails on the first violation, printing the value it saw: SIPp failed calls
over `MAX_FAILED_CALLS`, mock hellos over the run not equal to `TOTAL_CALLS`,
any mock protocol error, a peak `sent_bytes` of 0, non-zero `forks` or
`pool_leased_slabs` after the drain, RSS or fd outside the bands, or a
FreeSWITCH exit code other than 0 after `fsctl shutdown`. `rig-logs/` gets
`load-samples.csv`, `load-summary.txt`, the SIPp stat CSVs and the compose logs.

Reduced scale for a laptop:

```sh
CONCURRENT=100 TOTAL_CALLS=1000 WARMUP_CALLS=300 CALL_RATE=20 HOLD_SECONDS=14 \
  ./rig/load/run_load.sh
```
