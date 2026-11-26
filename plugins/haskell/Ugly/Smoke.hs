-- | A module built into the plugin, for its smoke test, until modules load
-- from export tables: one value of each kind the protocol maps.
module Ugly.Smoke (smoke) where

import qualified Data.ByteString as B
import qualified Data.Map as M
import Data.Word

import Ugly.Value

data Color = Red | Green | Blue
  deriving Show

instance ToUgly Color where
  toUgly = boxShow
  textOf = Just . show

smoke :: Box
smoke = moduleBox "Ugly.Smoke"
  [ ("unit", toUgly ())
  , ("yes", toUgly True)
  , ("answer", toUgly (42 :: Int))
  , ("byte", toUgly (200 :: Word8))
  , ("big", toUgly (2 ^ (70 :: Int) :: Integer))
  , ("half", toUgly (0.5 :: Double))
  , ("greeting", toUgly "h\233llo")
  , ("surrogate", toUgly "\xD800")
  , ("bytes", toUgly (B.pack [0, 255]))
  , ("color", toUgly Green)
  , ("colors", toUgly [Red, Green, Blue])
  , ("primes", toUgly [2, 3, 5, 7, 11 :: Int])
  , ("naturals", toUgly [0 :: Integer ..])
  , ("nothing", toUgly (Nothing :: Maybe Int))
  , ("just", toUgly (Just 'x'))
  , ("justNothing", toUgly (Just (Nothing :: Maybe Int)))
  , ("pair", toUgly (1 :: Int, "one"))
  , ("table", toUgly (M.fromList [("a", 1), ("b", 2 :: Int)]))
  , ("boom", toUgly (error "boom" :: Int))
  , ("lazyBoom", toUgly [1, error "lazy boom", 3 :: Int])
  , ("divZero", toUgly (1 `div` (0 :: Int)))
  ]
