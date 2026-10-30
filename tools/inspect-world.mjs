import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { pathToFileURL } from 'node:url';

export function parseNBT(buffer) {
  let p = 0;
  const take = n => { if (n < 0 || p + n > buffer.length) throw Error('Truncated NBT'); const b = buffer.subarray(p, p + n); p += n; return b; };
  const byte = () => take(1)[0];
  const int = () => take(4).readInt32BE();
  const str = () => take(take(2).readUInt16BE()).toString('utf8');
  function value(type, depth = 0) {
    if (depth > 64) throw Error('NBT nesting limit');
    switch (type) {
      case 1: return take(1).readInt8();
      case 2: return take(2).readInt16BE();
      case 3: return int();
      case 4: return take(8).readBigInt64BE().toString();
      case 5: return take(4).readFloatBE();
      case 6: return take(8).readDoubleBE();
      case 7: return take(int());
      case 8: return str();
      case 9: { const t = byte(), n = int(); if (n < 0 || n > buffer.length) throw Error('Invalid list'); return Array.from({length:n}, () => value(t, depth + 1)); }
      case 10: { const out = {}; for (;;) { const t = byte(); if (!t) return out; const name = str(); out[name] = value(t, depth + 1); } }
      case 11: return Array.from({length:int()}, int);
      case 12: { const n = int(); return take(n * 8); }
      default: throw Error(`Unknown NBT tag ${type}`);
    }
  }
  const type = byte(); str(); return value(type);
}

export function* chunks(world) {
  for (const name of fs.readdirSync(path.join(world, 'region')).filter(n => n.endsWith('.mca'))) {
    const region = fs.readFileSync(path.join(world, 'region', name));
    for (let i = 0; i < 1024; i++) {
      const offset = region.readUIntBE(i * 4, 3) * 4096;
      if (!offset) continue;
      const length = region.readUInt32BE(offset), type = region[offset + 4];
      const data = region.subarray(offset + 5, offset + 4 + length);
      const root = parseNBT(type === 1 ? zlib.gunzipSync(data) : type === 2 ? zlib.inflateSync(data) : data);
      yield root.Level;
    }
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const world = process.argv[2];
  const level = parseNBT(zlib.gunzipSync(fs.readFileSync(path.join(world, 'level.dat')))).Data;
  console.log(JSON.stringify(level, null, 2));
  const counts = {}, entities = {}; let count = 0;
  for (const chunk of chunks(world)) {
    count++;
    for (const s of chunk.Sections ?? []) for (let i = 0; i < 4096; i++) {
      const id = s.Blocks[i] + (s.Add ? ((s.Add[i >> 1] >> ((i & 1) * 4)) & 15) * 256 : 0);
      const meta = (s.Data[i >> 1] >> ((i & 1) * 4)) & 15;
      const key = `${id}:${meta}`; counts[key] = (counts[key] ?? 0) + 1;
    }
    for (const e of chunk.TileEntities ?? []) entities[e.id] = (entities[e.id] ?? 0) + 1;
  }
  console.log(JSON.stringify({chunks:count, blocks:counts, blockEntities:entities}, null, 2));
}
