/**
 * png.js — 最小 PNG 编码器（无依赖），Node 内置 zlib。
 * 用于调试脚本把 THREE 几何光栅化成图片直接查看。
 */
import { deflateSync } from 'node:zlib';

export class Raster {
  constructor(w, h) {
    this.w = w; this.h = h;
    this.buf = new Uint8Array(w * h * 3);
  }
  set(x, y, [r, g, b]) {
    x = Math.round(x); y = Math.round(y);
    if (x < 0 || x >= this.w || y < 0 || y >= this.h) return;
    const i = (y * this.w + x) * 3;
    this.buf[i] = r; this.buf[i + 1] = g; this.buf[i + 2] = b;
  }
  fillRect(x0, y0, x1, y1, c) {
    for (let y = Math.round(y0); y <= Math.round(y1); y++)
      for (let x = Math.round(x0); x <= Math.round(x1); x++) this.set(x, y, c);
  }
  /** 粗线段（非抗锯齿，够用） */
  line(x0, y0, x1, y1, c, w = 1) {
    const dx = x1 - x0, dy = y1 - y0;
    const len = Math.hypot(dx, dy) || 1;
    const nx = -dy / len, ny = dx / len;
    for (let t = 0; t <= len; t += 0.5) {
      const px = x0 + dx * (t / len), py = y0 + dy * (t / len);
      for (let ww = -w; ww <= w; ww++) this.set(px + nx * ww, py + ny * ww, c);
    }
  }
  /** 多边形填充（扫描线，水平每行测试交点；凸/凹均可） */
  fillPoly(pts, c) {
    if (!pts.length) return;
    let minY = Infinity, maxY = -Infinity;
    for (const p of pts) { minY = Math.min(minY, p[1]); maxY = Math.max(maxY, p[1]); }
    for (let y = Math.round(minY); y <= Math.round(maxY); y++) {
      const xs = [];
      for (let i = 0; i < pts.length; i++) {
        const a = pts[i], b = pts[(i + 1) % pts.length];
        if ((a[1] <= y && b[1] > y) || (b[1] <= y && a[1] > y)) {
          xs.push(a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
        }
      }
      xs.sort((a, b) => a - b);
      for (let i = 0; i + 1 < xs.length; i += 2) {
        for (let x = Math.round(xs[i]); x <= Math.round(xs[i + 1]); x++) this.set(x, y, c);
      }
    }
  }
  /** 矩形（用于标线实例，带旋转） */
  box(cx, cy, rotY, len, w, c) {
    const cos = Math.cos(rotY), sin = Math.sin(rotY);
    const corners = [
      [-len / 2, -w / 2], [len / 2, -w / 2], [len / 2, w / 2], [-len / 2, w / 2],
    ].map(([lx, lz]) => [cx + lx * cos - lz * sin, cy + lx * sin + lz * cos]);
    this.fillPoly(corners, c);
  }
  encode() {
    const { w, h, buf } = this;
    const raw = Buffer.alloc((w * 3 + 1) * h);
    const b = Buffer.from(buf.buffer, buf.byteOffset, buf.byteLength);
    for (let y = 0; y < h; y++) {
      raw[y * (w * 3 + 1)] = 0;
      b.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
    }
    const chunk = (type, data) => {
      const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
      const body = Buffer.concat([Buffer.from(type), data]);
      const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body));
      return Buffer.concat([len, body, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
    ihdr[8] = 8; ihdr[9] = 2; // 8bit RGB
    return Buffer.concat([
      Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
      chunk('IHDR', ihdr),
      chunk('IDAT', deflateSync(raw, { level: 9 })),
      chunk('IEND', Buffer.alloc(0)),
    ]);
  }
}
/* CRC32（PNG 规范查表法） */
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
