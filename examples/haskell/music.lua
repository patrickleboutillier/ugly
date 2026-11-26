-- music-diatonic, a Haskell library from Hackage, used from a plain Lua
-- script through the binding (examples/haskell/: Music.Diatonic.so is the
-- library with its export module, found on UGLY_HASKELL_PATH).

local ugly = require "ugly"

local md = ugly.module("haskell", "Music.Diatonic")
local show = md.show        -- Haskell's show, for any type the module exports

local function check(got, want)
  print(got)
  assert(got == want, "expected " .. tostring(want))
end

-- Showing each element of a Haskell list (1-based in Lua).
local function shown(list)
  local out = {}
  for i, x in ipairs(list) do
    out[i] = show(x)
  end
  return table.concat(out, " ")
end

local d_major = md.majorScale(md.D)
check(shown(md.notes(d_major)), "D E F# G A B C#")
check(#md.notes(d_major), 7)

-- Polymorphic functions, chosen by the arguments' types.
check(show(md.tonic(md.minorScale(md.A))), "A")
check(show(md.tonic(md.harmony(md.majorScale(md.F)))), "F")

-- Methods take the receiver last: chord:transpose(i, f) is transpose i f chord.
check(show(md.majorChord(md.C):transpose(md.Min3rd, md.above)), "Eb")

-- A Lua function where Haskell expects one.
check(shown(md.notes(md.noteMap(function(n) return md.below(md.Maj2nd, n) end, d_major))),
  "C D E F G A B")

-- read needs a hint: a type application.
check(show(md.read("F#m", ugly.opts{hint = "Chord"})), "F#m")

check(shown(md.chords(md.harmony7(md.majorScale(md.G)))), "Gmaj7 Am7 Bm7 Cmaj7 D7 Em7 F#m7(b5)")
check(md.equiv(md.sharp(md.E), md.F), true)
check(show(md.relative(md.majorKey(md.C))), "Am")

os.exit(0, true)
