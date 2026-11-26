{-# LANGUAGE FlexibleInstances, FunctionalDependencies #-}

-- | A small library for uglyModule's test (exports.c), exported by
-- Example.Ugly: types, classes (one with two parameters and a functional
-- dependency), and polymorphic functions, as music-diatonic has them.
module Example
  ( Shape (..), Pt (..), HasArea (..), Scalable (..), Unit (..)
  , origin, largest, firstOf, label, mapPt
  ) where

data Shape = Circle Double | Square Double
  deriving (Show, Eq)

data Pt = Pt Int Int
  deriving (Show, Eq, Ord)

class HasArea a where
  area :: a -> Double

instance HasArea Shape where
  area (Circle r) = pi * r * r
  area (Square s) = s * s

instance HasArea Pt where
  area _ = 0

class Scalable a where
  scale :: Double -> a -> a

instance Scalable Shape where
  scale k (Circle r) = Circle (k * r)
  scale k (Square s) = Square (k * s)

instance Scalable Pt where
  scale k (Pt x y) = Pt (round (k * fromIntegral x)) (round (k * fromIntegral y))

-- | The unit of a thing's measure: the type of the unit depends on the thing's.
class Unit a u | a -> u where
  unit :: a -> u

instance Unit Shape String where
  unit _ = "m2"

instance Unit Pt Int where
  unit (Pt x _) = x

origin :: Pt
origin = Pt 0 0

largest :: Ord a => [a] -> a
largest = maximum

firstOf :: [a] -> a
firstOf (x : _) = x
firstOf [] = error "firstOf: an empty list"

label :: Show a => String -> a -> String
label l x = l ++ ": " ++ show x

-- | A callback: a Haskell function, or a foreign one.
mapPt :: (Int -> Int) -> Pt -> Pt
mapPt f (Pt x y) = Pt (f x) (f y)
