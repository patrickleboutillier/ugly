"""music-diatonic, a Haskell library from Hackage, used from a plain Python
script through the binding (examples/haskell/: Music.Diatonic.so is the
library with its export module, found on UGLY_HASKELL_PATH)."""

import ugly

md = ugly.module("haskell", "Music.Diatonic")
show = md.show          # Haskell's show, for any type the module exports


def check(got, want):
    print(got)
    assert got == want, f"expected {want!r}"


# Haskell values are proxies: scales, chords, notes...
d_major = md.majorScale(md.D)
check([show(n) for n in md.notes(d_major)], ["D", "E", "F#", "G", "A", "B", "C#"])

# A polymorphic function is chosen by its arguments' types: tonic of a scale is
# a note, of a harmony a chord.
check(show(md.tonic(md.minorScale(md.A))), "A")
check(show(md.tonic(md.harmony(md.majorScale(md.F)))), "F")

# Methods take the receiver last, as Haskell does: chord.transpose(i, f) is
# transpose i f chord. md.above is a Haskell function passed back to Haskell.
c_major = md.majorChord(md.C)
check(show(c_major.transpose(md.Min3rd, md.above)), "Eb")

# A Python function where Haskell expects one (noteMap :: (Note -> Note) -> a -> a).
check([show(n) for n in md.notes(md.noteMap(lambda n: md.above(md.Perf5th, n), d_major))],
      ["A", "B", "C#", "D", "E", "F#", "G#"])

# Polymorphic in its result alone: read needs a hint (a type application).
check(show(md.read("Bb", ugly.opts(hint="Note"))), "Bb")
check(show(md.read("Am7", ugly.opts(hint="Chord"))), "Am7")

# Lists, tuples, Maybe (Nothing is None, Just x is x), Bool, Int.
check([show(c) for c in md.chords(md.harmony(md.majorScale(md.C)))], ["C", "Dm", "Em", "F", "G", "Am", "Bo"])
check([(show(d), show(n)) for d, n in md.degrees(md.majorScale(md.E))][:3], [("1", "E"), ("2", "F#"), ("3", "G#")])
check(show(md.dominant(md.majorScale(md.G))), "D")
check(md.equiv(md.sharp(md.C), md.flat(md.D)), True)
check(md.semitones(md.distance(md.C, md.G)), 7)
check(show(md.signature(md.majorKey(md.A))), "[F#,C#,G#] (3 #)")
