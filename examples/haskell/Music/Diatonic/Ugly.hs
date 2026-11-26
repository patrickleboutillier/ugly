{-# LANGUAGE TemplateHaskell #-}
{-# OPTIONS_GHC -Wno-orphans #-}

-- | music-diatonic (Hackage) for every other language: the module
-- "Music.Diatonic", built with the library into Music.Diatonic.so. The
-- export list is written by hand; uglyModule does the rest: the listed
-- types cross as boxes whose text is their show and whose methods are these
-- functions (receiver last: chord.transpose(Min3rd, above) is transpose
-- Min3rd above chord), and the polymorphic functions (transpose, tonic,
-- notes, show, read...) are expanded over the library's instances.
module Music.Diatonic.Ugly () where

import Music.Diatonic
import Music.Diatonic.Chord
import Music.Diatonic.Harmony
import Music.Diatonic.Key
import Music.Diatonic.Scale
import Ugly.Export

uglyModule "Music.Diatonic"
  [ ''Note, ''Accidental, ''Interval, ''Quality, ''Degree
  , ''Scale, ''Chord, ''Key, ''Signature, ''Harmony
    -- notes
  , 'C, 'D, 'E, 'F, 'G, 'A, 'B, 'Sharp, 'Flat, 'Natural
  , 'raise, 'lower, 'sharp, 'flat, 'natural, 'accidental, 'next, 'prev, 'canonize, 'circleOfFifths
  , 'above, 'below, 'distance, 'transpose, 'noteMap, 'notes
    -- intervals and qualities
  , 'Unison, 'Min2nd, 'Maj2nd, 'Min3rd, 'Maj3rd, 'Perf4th, 'Perf5th, 'Min6th, 'Maj6th, 'Min7th, 'Maj7th
  , 'augment, 'diminish, 'steps, 'semitones, 'octave
  , 'Major, 'Minor, 'Diminished, 'Augmented, 'Perfect, 'quality
    -- scales, chords, keys, harmonies
  , 'majorScale, 'minorScale, 'majorPentatonicScale, 'minorPentatonicScale, 'minorHarmonicScale, 'minorMelodicScale
  , 'majorChord, 'minorChord, 'diminishedChord, 'augmentedChord, 'major7thChord, 'dominant7thChord, 'minor7thChord
  , 'root
  , 'majorKey, 'minorKey, 'relative, 'parallel, 'signature, 'key
  , 'harmony, 'harmony7, 'chords
    -- degrees
  , 'degree, 'degrees, 'tonic, 'dominant, 'subdominant, 'mediant, 'leadingTone
    -- the classes everything has
  , 'equiv, 'show, 'read ]
