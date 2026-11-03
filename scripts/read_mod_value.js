// 从内核 .ko 的符号表里把某个模块参数的**初始值**读出来。
// 用途：证明「源码里改的默认值真的进了编译产物」——dist/ 下的 .ko 是 stripped，
// 必须喂 unstripped/ 那一份（含 .symtab）。
// 用法: node read_mod_value.js <path.ko> [符号名...]
const fs = require("fs");

function parse(buf) {
  if (buf.slice(0, 4).toString("hex") !== "7f454c46") throw new Error("不是 ELF 文件");
  if (buf[5] !== 1) throw new Error("只支持 32-bit 标志位的 ELF64 小端");
  const u16 = (o) => buf.readUInt16LE(o);
  const u32 = (o) => buf.readUInt32LE(o);
  const u64 = (o) => Number(buf.readBigUInt64LE(o));

  const e_shoff = u64(0x28);
  const e_shentsize = u16(0x3a);
  // 偏移别搞错：0x38 是 e_phnum，e_shnum 在 0x3c，e_shstrndx 在 0x3e
  let e_shnum = u16(0x3c);
  const e_shstrndx = u16(0x3e);

  const secs = [];
  // e_shnum==0 表示段数超出 16 位，真实数量在 shdr[0].sh_size（扩展编号规则）
  if (e_shnum === 0) e_shnum = u32(e_shoff + 0x28);
  for (let i = 0; i < e_shnum; i++) {
    const o = e_shoff + i * e_shentsize;
    secs.push({
      nameOff: u32(o), type: u32(o + 4), off: u64(o + 0x18),
      size: u64(o + 0x20), link: u32(o + 0x28), entsize: u64(o + 0x38), name: "",
    });
  }
  const shstr = secs[e_shstrndx];
  // 必须带上边界：越界读出来的 buf[e] 是 undefined，undefined !== 0 永远为真，
  // 不判界就会死循环挂住（本地测出来过一次，构建机上同样会挂）。
  const cstr = (base, limit) => {
    if (base < 0 || base >= buf.length) return "?";
    let e = base;
    const end = Math.min(limit === undefined ? buf.length : limit, buf.length);
    while (e < end && buf[e] !== 0) e++;
    return buf.slice(base, e).toString();
  };
  for (const s of secs) s.name = cstr(shstr.off + s.nameOff, shstr.off + shstr.size);

  const sym = secs.find((s) => s.name === ".symtab");
  const strv = secs.find((s) => s.name === ".strtab");
  if (!sym || !strv) return { secs, symbols: null };

  const out = [];
  if (!sym.entsize) return { secs, symbols: [] };
  const n = Math.floor(sym.size / sym.entsize);
  for (let i = 0; i < n; i++) {
    const o = sym.off + i * sym.entsize;
    const nameOff = u32(o);
    const shndx = u16(o + 6);   // ELF64 Sym: st_shndx 在偏移 6（不是段表的 0x3e）
    const value = u64(o + 8);
    const size = u64(o + 0x10);
    out.push({ name: cstr(strv.off + nameOff, strv.off + strv.size), value, size, shndx });
  }
  return { secs, symbols: out };
}

module.exports = { parse };

if (require.main === module) {
  const file = process.argv[2];
  const want = process.argv.slice(3);
  const names = want.length ? want : ["use_vutil_threshold", "boost_kill", "ohm_action_ctrl"];
  if (!file) { console.error("用法: node read_mod_value.js <file.ko> [符号...]"); process.exit(2); }
  const buf = fs.readFileSync(file);
  const { secs, symbols } = parse(buf);
  console.log("段表: " + secs.map((s) => s.name).filter(Boolean).slice(0, 8).join(", ") + " ...");
  if (!symbols) {
    console.log("!! 这个 .ko 没有 .symtab（stripped）—— 要读初值请用 unstripped 版");
    process.exit(0);
  }
  let hit = 0;
  for (const s of symbols) {
    if (!names.includes(s.name)) continue;
    hit++;
    const sec = secs[s.shndx];
    let v = null, why = "";
    const fmt = { 1: "Int8LE", 2: "Int16LE", 4: "Int32LE", 8: "BigInt64LE" }[s.size];
    if (!sec || sec.type !== 1) why = "段类型不是 PROGBITS";
    else if (!fmt) why = "size=" + s.size + "，不是标量";
    else {
      const off = sec.off + s.value;
      if (off + s.size > sec.off + sec.size) why = "偏移越界";
      else v = buf["read" + fmt](off);
    }
    console.log(`符号 ${s.name} 段=${sec ? sec.name : "?"} 偏移=0x${s.value.toString(16)} size=${s.size} 初值=${v === null ? "(不读:" + why + ")" : v}`);
  }
  console.log(`命中 ${hit}/${names.length} 个符号`);
  if (hit === 0) { console.log("提示：这些参数符号名来自 module_param，stripped 版只剩 .modinfo 里的 parm 名"); }
}
