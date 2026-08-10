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
transitions are flushed and synced immediately. Periodic records are batched.

## Production configuration

`config/pipeline_car.json` enables production mode:

```json
{"mode":"production"}
```

Optional parameters are `frequency_hz` (0.2-10), `pem_log_path`,
`rotate_sec`, `rotate_mb`, `cpu_critical_pct` and `mem_critical_pct`.
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
