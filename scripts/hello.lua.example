-- Skrip menerima satu argumen (ctx): lib, base, size, root, script.
-- Jika skrip me-return fungsi, fungsi itu dipanggil tiap frame.
local ctx = ...
local ffi = require("ffi")
print("lib", ctx.lib, ffi.cast("void*", ctx.base), ctx.size)
local p = ffi.cast("uint8_t*", ctx.base)
print("magic", string.char(p[1], p[2], p[3]))   -- harus "ELF"
local n = 0
return function()
  n = n + 1
  if n % 300 == 0 then print("frame", n) end
end
