# libluaaml

Mod AML (arm64-v8a) yang menanam LuaJIT. Dua fitur:
1. **Shell Lua** dengan UI Android native (tombol "Lua" mengambang -> panel: input, Run, Stop,
   Clr out, Clr in, Paste, Copy out, Copy hasil, riwayat, Reset). State Lua persisten.
2. **Skrip per-frame** dari folder `lua/` (skrip me-return fungsi -> dipanggil tiap frame).

## Folder (Android/data/<package>/files/libluaaml/)
- config.cfg: `target=libGame.so` (opsional), `scripts=lua`, `timeout=5000`, `ui=1`
- lua/*.lua, log.txt

## Shell
- Ekspresi (`1+1`) otomatis dicetak sebagai `=> 2`; pernyataan dijalankan biasa.
- Global `ctx` = {lib, base, size, root}. `ffi` tersedia (`require("ffi")`).
- Loop panjang dihentikan otomatis oleh timeout; tombol Stop menghentikan manual.

## Build
GitHub Actions -> artifact `libluaaml-arm64` berisi `libluaaml.so` (dex UI ikut tertanam).
