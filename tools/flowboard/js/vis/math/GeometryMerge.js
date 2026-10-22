/**
 * GeometryMerge.js — BufferGeometry 合并工具
 * 把多个小几何体合并成一个大 BufferGeometry，减少 draw call。
 */

/**
 * 合并多个 BufferGeometry。
 *
 * @param {BufferGeometry[]} geometries
 * @param {string[]} [attributes] 要写出的属性名，缺省 `['position','normal','uv']`
 *   （= 旧行为，既有 4 个调用点不受影响）。
 *   需要这个参数是因为：源几何**缺某个属性时本函数会补零** —— 法线补零会渲染成
 *   纯黑，uv 补零会让贴图只采样同一个 texel。车辆 glTF 里大量无贴图件（body/
 *   cabin/hood…，材质没有 map）本来就没有 uv，硬补零毫无意义；调用方按材质是否
 *   用贴图来决定属性表，就能安全地只并 position+normal。
 *   ⚠️ 不同属性表的几何**不能混并**，调用方需把它们分到不同的桶。
 */
export function mergeGeometries(geometries, attributes) {
  const attrs = attributes && attributes.length ? attributes : ['position', 'normal', 'uv'];
  const valid = geometries.filter(g => g && g.attributes.position && g.attributes.position.count > 0);
  if (valid.length === 0) return new THREE.BufferGeometry();
  if (valid.length === 1) return valid[0].clone();

  let totalVerts = 0, totalIdx = 0;
  for (const g of valid) {
    totalVerts += g.attributes.position.count;
    if (g.index) { totalIdx += g.index.count; }
    else { totalIdx += g.attributes.position.count; }
  }

  const arrays = {};
  for (const name of attrs) {
    const itemSize = name === 'uv' ? 2 : 3;
    arrays[name] = new Float32Array(totalVerts * itemSize);
  }
  const indices = new Uint32Array(totalIdx);

  let vOff = 0, iOff = 0, idxBase = 0;
  for (const g of valid) {
    const vCount = g.attributes.position.count;
    /* 🔴 必须逐元素经访问器拷贝，不能 `dst.set(src.array, offset)`。
     * glTF 常用 **InterleavedBufferAttribute**（多个属性共用一条交错缓冲），
     * 此时 `src.array` 是整条交错缓冲、长度远大于该属性本身 —— 直接 set 会
     * `RangeError: offset is out of bounds`（2026-10-23 实测：车辆 glTF 的
     * normal/uv 就是交错属性，合并整批车时全部抛这个错）。`getX/getY/getZ`
     * 对 BufferAttribute 与 InterleavedBufferAttribute 都正确。 */
    for (const name of attrs) {
      const src = g.attributes[name];
      if (!src) continue;                        // 缺则保持 0（调用方已保证该属性表下不会缺）
      const arr = arrays[name];
      const comps = name === 'uv' ? 2 : 3;
      const n = Math.min(vCount, src.count != null ? src.count : vCount);
      for (let i = 0; i < n; i++) {
        const o = (vOff + i) * comps;
        arr[o] = src.getX(i);
        if (comps > 1) arr[o + 1] = src.getY(i);
        if (comps > 2) arr[o + 2] = src.getZ(i);
      }
    }

    if (g.index) {
      const idx = g.index.array;
      for (let i = 0; i < idx.length; i++) indices[iOff + i] = idx[i] + idxBase;
      iOff += idx.length;
    } else {
      const count = g.attributes.position.count;
      for (let i = 0; i < count; i++) indices[iOff + i] = i;
      iOff += count;
    }
    idxBase += g.attributes.position.count;
    vOff += g.attributes.position.count;
  }

  const merged = new THREE.BufferGeometry();
  for (const name of attrs) {
    merged.setAttribute(name, new THREE.BufferAttribute(arrays[name], name === 'uv' ? 2 : 3));
  }
  merged.setIndex(new THREE.BufferAttribute(indices, 1));
  return merged;
}

/** 创建一个平移后的 geometry 副本（用于护栏/路灯等重复元素） */
export function translateGeo(geo, x, y, z) {
  const g = geo.clone();
  g.translate(x, y, z);
  return g;
}
