-- bootstrap.lua — Internal bootstrap for pdluajit (embedded in pdluajit.c)
--
-- This script is executed automatically when a LuaJIT state is created.
-- It wraps raw C pointers into indexable float arrays so users never
-- need to touch FFI directly.
--
-- User-facing API:
--   perform(ins, outs, n)    — ins[channel][sample], outs[channel][sample]
--   dsp(samplerate, blocksize)
--   control(selector, value)
--
-- Channels are 1-indexed, samples are 0-indexed (C convention for performance).

local ffi = require("ffi")
ffi.cdef("typedef float t_float;")

local cast = ffi.cast
local float_ptr_t = ffi.typeof("t_float*")

-- Wrap lightuserdata pointers into indexable float* arrays
-- Called from C perform routine: __perform_wrapper(n_in, n_out, n, ptr1, ptr2, ...)
function __perform_wrapper(n_in, n_out, n, ...)
    local args = {...}
    local ins = {}
    local outs = {}
    for i = 1, n_in do
        ins[i] = cast(float_ptr_t, args[i])
    end
    for i = 1, n_out do
        outs[i] = cast(float_ptr_t, args[n_in + i])
    end
    perform(ins, outs, n)
end

-- pd table: utilities exposed to user scripts
pd = pd or {}
