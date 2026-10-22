# libluaaml

Mod AML (arm64-v8a) yang menanam LuaJIT. Skrip Lua mentah, tanpa API global.
Lib target ditentukan murni lewat config.

## Folder (di Android/data/<package>/files/libluaaml/)
- config.cfg  -> `target=libGame.so`, `scripts=lua`
- lua/*.lua   -> dimuat berurutan setelah lib target termuat
- log.txt     -> log mod dan output `print`

## Skrip
Menerima satu argumen `ctx` (lib, base, size, root, script).
Kalau skrip me-return fungsi, fungsi itu dipanggil tiap frame (thread render).

## Build
GitHub Actions -> artifact `libluaaml-arm64` berisi `libluaaml.so`.
Taruh di folder mods milik AML.
