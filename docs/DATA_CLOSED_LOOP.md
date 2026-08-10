# Data Closed Loop

KunAutoDrive uses one observability core with two deployment profiles. It does not
maintain separate development-vehicle and production-vehicle collection stacks.

## Profiles

| Profile | Purpose | Output |
|---|---|---|
| `development` (default) | Simulation, visualization, incident reproduction | Dashboard JSON, scene data, samples, registry and IPC bridges |
| `production` | Vehicle health and fleet data collection | Low-rate binary system, topic, health and degrade-event records |

The profiles share message-bus statistics, `health`, `degrade_ladder`, clocks and
event semantics. Sampling and storage policy may differ, but metric definitions
must not differ. This keeps offline evaluation comparable with vehicle data.

## Closed-loop path

```text
node instrumentation
  -> shared topic/health collection
  -> metric and transition records
  -> CRC-protected, rotated binary logs
  -> pem_dump.py filtering/JSONL conversion
  -> incident analysis, replay and evaluation
  -> parameter/model update
  -> deployment
  -> validation with the same metric definitions
```

Every binary record contains monotonic and realtime timestamps, sequence number,
record type, critical flag and CRC. Realtime correlates vehicle records with
external systems; monotonic time provides stable interval analysis. Critical
transitions are flushed and `fsync`ed immediately; a new segment's parent
directory is synced after creation; periodic records flush after 50 records or
one second. The record layout is defined once in
`include/pem_log_layout.def`, used by both the C writer and `pem_dump.py`.

PEM rotates by time or single-file size. Retention is bounded by both a segment
count and a total-directory quota; the oldest closed segments are removed after
opening a new segment, while the active segment is never removed. Default
retention is 96 five-minute segments and 1 GiB. The writer serializes public
operations with an internal lock, but production design remains one
monitor-owned writer to avoid filesystem stalls on a control worker.

## Production configuration

`config/pipeline_car.json` enables production mode:

```json
{"mode":"production","rotate_sec":300,"rotate_mb":100,
 "retain_segments":96,"retain_mb":1024}
```

Optional parameters are `frequency_hz` (0.2-10), `pem_log_path`,
`rotate_sec`, `rotate_mb`, `retain_segments`, `retain_mb`,
`cpu_critical_pct` and `mem_critical_pct`.
Without `pem_log_path`, logs use the cross-platform KunAutoDrive temporary
directory with prefix `kunautodrive_pem`.

Decode logs:

```bash
python3 tools/pem_dump.py /tmp/kunautodrive_pem_*.pem
python3 tools/pem_dump.py --jsonl --type event /tmp/kunautodrive_pem_*.pem
python3 tools/pem_dump.py --type topic --name planning /tmp/kunautodrive_pem_*.pem
```

The production profile deliberately does not subscribe to scene or large
payload topics and does not open dashboard/stats IPC bridges.
