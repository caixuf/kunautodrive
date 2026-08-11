# city_ring

`city_ring` is the reusable map contract extracted from the validated
city-road baseline. The current map intentionally exposes only the verified
`mainline` route; junction and turn routes are added only after independent
closed-loop validation.

- `map.json` contains the authoritative road geometry, elevation profiles,
  road properties, static landmarks and road connections.
- `routes.json` contains reusable route definitions.

The default `scripts/demo.sh` entry point is unchanged.
