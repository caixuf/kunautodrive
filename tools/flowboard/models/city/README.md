# Downtown City assets

These three building assets come from Quaternius Downtown City MegaKit
(Standard/free pack), licensed under CC0 1.0. The original license is kept in
`LICENSE_DOWNTOWN_CITY.txt`.

`models.js` loads the `.gltf` files with their sibling `.bin` and PNG texture
files. `BuildingView` uses real buildings only in the near field, with a
budget of 18/8/0 models for high/medium/low performance tiers. Remaining
slots use the instanced silhouette fallback so the full modular pack is not
loaded into the runtime.
