// 把「原厂 .ko 要求的符号 CRC」和「我们这次构建导出的 CRC」对表。
// 这本来是唯一只能靠刷机才能判的未知量，搬到 CI 里就能离线判定。
// 用法: node crc_compare.js <crc_stock.json> <Module.symvers 文件...>
// json 形如 {"memset":"dcb764ad", ...}（32-bit CRC，十六进制字符串）
const fs = require("fs");

const wantRaw = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const files = process.argv.slice(3);
if (!files.length) { console.error("需要至少一个 Module.symvers"); process.exit(2); }

// Module.symvers 每行： <crc>\t<symbol>\t<module>\t<export type>\t[namespace]
// CRC 是 0x 开头的十六进制或十进制，6.1 里是 32 位 CRCTABLE
// 同一符号可能在多个 symvers 里出现（common / vendor 各一份）。
// 这里按「任一文件对上都算一致」处理，避免把实际能加载的判成不能加载（假阴性）。
const ours = new Map();
let lines = 0;
for (const f of files) {
  for (const line of fs.readFileSync(f, "utf8").split("\n")) {
    const t = line.split("\t");
    if (t.length < 2 || !t[0]) continue;
    const crc = BigInt(t[0]);
    const sym = t[1];
    const hex = (crc & 0xffffffffn).toString(16).padStart(8, "0");
    if (!ours.has(sym)) ours.set(sym, new Set());
    ours.get(sym).add(hex);
    lines++;
  }
}

const want = Object.keys(wantRaw);
let match = 0;
const mismatch = [], absent = [];
for (const s of want) {
  const set = ours.get(s);
  if (!set) { absent.push(s); continue; }
  if (set.has(wantRaw[s])) { match++; continue; }
  mismatch.push([s, wantRaw[s], [...set].join("|")]);
}

console.log("读取 Module.symvers: " + files.length + " 个文件, " + lines + " 行");
console.log("原厂要求的符号数: " + want.length);
console.log("  CRC 一致     : " + match);
console.log("  CRC 不一致   : " + mismatch.length);
console.log("  我们没导出   : " + absent.length);
for (const [s, a, b] of mismatch.slice(0, 20))
  console.log(`    差异 ${s}  原厂要=${a} 我们给=${b}`);
for (const s of absent.slice(0, 20))
  console.log(`    缺符号 ${s}`);

// 判定：只有全部一致才算 CRC 关卡通过
if (mismatch.length === 0 && absent.length === 0) {
  console.log("VERDICT: CRC_ALL_MATCH —— 这批原厂模块的符号 CRC 与我们内核导出的逐一对上");
  process.exit(0);
}
if (match === 0) {
  console.log("VERDICT: CRC_NONE_MATCH —— 一个都对不上，几乎确定会报 disagrees about version of symbol");
  process.exit(1);
}
console.log(`VERDICT: CRC_PARTIAL —— 命中 ${match}/${want.length}，会有部分原厂模块加载失败`);
process.exit(1);
