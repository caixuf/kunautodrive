# city_ring

`city_ring` is the reusable map contract extracted from the validated
city-road baseline. The current map intentionally exposes only the verified
`mainline` route; junction and turn routes are added only after independent
closed-loop validation.

- `map.json` contains map identity, authoritative geometry source, and edge
  connections.
- `routes.json` contains reusable route definitions.
- `source_scenario` remains the compatibility geometry source until the loader
  migration is complete.

The default `scripts/demo.sh` entry point is unchanged.
