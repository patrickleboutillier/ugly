{-# LANGUAGE TemplateHaskell #-}

-- | A module for the plugin's tests (values.c, calls.c), written by hand
-- with Ugly.Value: one value of each kind the protocol maps, and functions
-- to call. Module "Smoke" in the library Smoke.so (uglyModuleFrom).
module Smoke (smoke) where

import qualified Data.ByteString as B
import Data.Int
import Data.IORef
import qualified Data.Map as M
import Data.Word
import System.IO.Unsafe (unsafePerformIO)

import Ugly.Export (uglyModuleFrom)
import Ugly.Value

data Color = Red | Green | Blue
  deriving (Show, Eq, Enum, Bounded)

-- | Its methods are this module's functions (receiver last).
instance ToUgly Color where
  toUgly c = boxWith (Just (show c)) (`lookup` entries) c
  textOf = Just . show
instance FromUgly Color

next :: Color -> Color
next c = if c == maxBound then minBound else succ c

mix :: Color -> Color -> String
mix a b = show a ++ "+" ++ show b

counter :: IORef Int
counter = unsafePerformIO (newIORef 0)
{-# NOINLINE counter #-}

smoke :: Box
smoke = moduleBox "Smoke" entries

entries :: [(String, Value)]
entries =
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
    -- functions
  , ("add", toUgly ((+) :: Int -> Int -> Int))
  , ("greet", toUgly (("hello, " ++) :: String -> String))
  , ("halve", toUgly ((/ 2) :: Double -> Double))
  , ("widen", toUgly (fromIntegral :: Word8 -> Int))
  , ("safeDiv", toUgly (\a b -> if b == 0 then Nothing else Just (a `div` b) :: Maybe Int))
  , ("orZero", toUgly (maybe 0 id :: Maybe Int -> Int))
  , ("identity", toUgly (id :: Value -> Value))
  , ("apply", toUgly ((\f x -> f x) :: (Int -> Int) -> Int -> Int))
  , ("explode", toUgly ((\n -> if n > 0 then n else error "not positive") :: Int -> Int))
  , ("tick", toUgly (atomicModifyIORef' counter (\n -> (n + 1, n + 1))))
  , ("tickBy", toUgly (\k -> atomicModifyIORef' counter (\n -> (n + k, n + k)) :: IO Int))
  , ("next", toUgly next)
  , ("mix", toUgly mix)
    -- overloaded: one instantiation per type, chosen by the arguments
  , ("size", overloaded "size :: Sized a => a -> Int"
      [ toUgly (length :: [Int] -> Int)
      , toUgly (length :: String -> Int)
      , toUgly (const 1 :: Color -> Int) ])
  , ("narrow", overloaded "narrow :: Narrow a => a -> String"
      [ toUgly ((\x -> "Int8 " ++ show x) :: Int8 -> String)
      , toUgly ((\x -> "Word8 " ++ show x) :: Word8 -> String) ])
  ]

uglyModuleFrom "Smoke" 'smoke
