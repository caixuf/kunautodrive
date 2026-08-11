# city_center

This is the first-tier downtown map foundation: a six-lane central avenue,
financial street, riverside boulevard, and a depressed underpass. The routes
are intentionally marked `validated: false`; they are static geometry targets
until junction and signal semantics are connected to navigation.

Compile the source after editing:

```bash
python3 tools/map_compiler.py maps/city_center/city_center.kmap \
  -o maps/city_center/map.json
```

This map is not the default demo and does not replace `city_ring`.
