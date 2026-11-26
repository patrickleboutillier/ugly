{-# LANGUAGE ScopedTypeVariables, TemplateHaskell #-}

-- | The conformance fixtures (doc/contract.md section 9), in Haskell:
-- module "conformance", in the library conformance.so.
--
-- Most of it is ordinary Haskell exported by uglyModule. What the contract
-- asks that Haskell doesn't have is written by hand with Ugly.Value: a
-- mutable counter, and its list (IORefs behind a box, with the type names
-- the suite expects, "counter" and "list"); an exception whose payload has
-- a 'message' member (declared under UglyException); and finalization,
-- counted by weak pointers that a major GC has found dead.
module Conformance
  ( Counter, List, Point (..), ConformanceError (..), Over (..)
  , echo, kind_of, make_counter, apply, call_twice, raise_error, catch_and_return, rethrow, finalized
  , norm2, bench, bench_self
  ) where

import Control.Exception
import Control.Monad (filterM, forM_)
import Data.Int (Int64)
import Data.IORef
import Data.Maybe (fromMaybe, isJust)
import qualified Data.Text as T
import System.IO.Unsafe (unsafePerformIO)
import System.Mem (performMajorGC)
import System.Mem.Weak (deRefWeak)

import qualified Ugly
import Ugly.Export (uglyModule)
import Ugly.Value


-- Finalization: each fixture object is tracked by a weak pointer, and
-- finalized() counts those a major GC found dead. Deterministic, unlike
-- waiting for the finalizers themselves, which run on a thread of their own.

registry :: IORef [IO Bool]      -- ^ each: is it still alive?
registry = unsafePerformIO (newIORef [])
{-# NOINLINE registry #-}

dead :: IORef Int
dead = unsafePerformIO (newIORef 0)
{-# NOINLINE dead #-}

track :: IORef a -> IO ()
track ref = do
  w <- mkWeakIORef ref (return ())
  atomicModifyIORef' registry (\ws -> ((isJust <$> deRefWeak w) : ws, ()))

finalized :: IO Int
finalized = do
  performMajorGC
  ws <- atomicModifyIORef' registry (\ws -> ([], ws))
  alive <- filterM id ws
  atomicModifyIORef' registry (\ws' -> (ws' ++ alive, ()))
  atomicModifyIORef' dead (\n -> let n' = n + length ws - length alive in (n', n'))


-- The counter: count, items (a list of the counts so far), inc().

data Counter = Counter (IORef Int) List

-- | A mutable list of any values.
newtype List = List (IORef [Value])

instance ToUgly List where
  toUgly l@(List ref) = VBox (Box l (info "list") { infoElems = Just elems })
    where
      elems = Elems (length <$> readIORef ref) at (Just splice)
      at i = (\xs -> if i >= 0 && i < length xs then Just (xs !! i) else Nothing) <$> readIORef ref
      splice off n args = do
        convs <- mapM fromUgly args
        return (sum (map fst convs), do
          vs <- sequence (map snd convs)
          atomicModifyIORef' ref (\xs -> (take off xs ++ vs ++ drop (off + n) xs, ())))
instance FromUgly List

instance ToUgly Counter where
  toUgly c@(Counter count items) = VBox (Box c (info "counter")
    { infoMembers = Just (Members (return ["count", "items"]) get (Just set))
    , infoMethods = Just (\m -> if m == "inc" then Just (toUgly inc) else Nothing) })
    where
      get "count" = Just . toUgly <$> readIORef count
      get "items" = return (Just (toUgly items))
      get _ = return Nothing
      set "count" = Just (\arg -> fmap (fmap (>>= writeIORef count)) (fromUgly arg))
      set _ = Nothing
instance FromUgly Counter

-- | The counter's method (invoke: the receiver last).
inc :: Counter -> IO Int
inc (Counter count (List items)) = do
  n <- atomicModifyIORef' count (\k -> (k + 1, k + 1))
  atomicModifyIORef' items (\xs -> (xs ++ [VInt (fromIntegral n)], ()))
  return n

make_counter :: IO Counter
make_counter = do
  count <- newIORef 0
  items <- newIORef []
  track count
  track items
  return (Counter count (List items))


-- Point: new(Point, x, y), members x and y (a record), norm2().

data Point = Point { x :: Double, y :: Double }
  deriving Show

norm2 :: Point -> Double
norm2 (Point a b) = a * a + b * b


-- Errors.

data ConformanceError = ConformanceError { message :: String }
  deriving Show

-- | Raised with its own members: its payload's 'message'.
instance Exception ConformanceError where
  toException = uglyToException
  fromException = uglyFromException
  displayException = message

instance ToUgly ConformanceError where
  toUgly e = boxRecord (Just (message e)) (const Nothing) [("message", toUgly . message)] e

raise_error :: String -> IO ()
raise_error = throwIO . ConformanceError

-- | The exception f() raised: a foreign error's payload, or a Haskell
-- exception as the same object the core would carry.
catch_and_return :: Value -> IO Value
catch_and_return f = Ugly.callValue f [] `catch` \(e :: SomeException) -> case fromException e of
  Just fe -> return (fromMaybe VNull (Ugly.errorPayload fe))
  Nothing -> Ugly.exceptionValue e

rethrow :: Value -> IO ()
rethrow = Ugly.rethrow


-- Values and calls.

echo :: Value -> Value
echo v = v

kind_of :: Value -> String
kind_of v = case v of
  VNull -> "null"
  VBool _ -> "bool"
  VInt _ -> "int"
  VDouble _ -> "double"
  VString _ -> "string"
  VBytes _ -> "bytes"
  VForeign _ -> "proxy"
  VBox (Box _ i) -> infoType i

apply :: Value -> Value -> IO Value
apply f v = Ugly.callValue f [v]

call_twice :: Value -> Value -> IO Value
call_twice f v = Ugly.callValue f [v] >>= \r -> Ugly.callValue f [r]

-- | Benchmarks (tests/bench.c): f(i) n times, and echo n times.
bench :: Value -> Int -> IO Int
bench f n = forM_ [0 .. n - 1] (\i -> Ugly.callValue f [VInt (fromIntegral i)]) >> return n

bench_self :: Int -> IO Int
bench_self n = forM_ [0 .. n - 1] (\i -> evaluate (echo (VInt (fromIntegral i)))) >> return n


-- Call options (tests/options.c): a hint chooses the instantiation.

class Over a where
  over :: a -> String
instance Over Int where
  over _ = "Int"
instance Over Int64 where
  over _ = "Int64"
instance Over T.Text where
  over _ = "Text"
instance Over Value where
  over _ = "Value"


uglyModule "conformance"
  [ ''Counter, ''List, ''Point, ''ConformanceError
  , 'echo, 'kind_of, 'make_counter, 'apply, 'call_twice, 'raise_error, 'catch_and_return, 'rethrow
  , 'finalized, 'Point, 'norm2, 'bench, 'bench_self, 'over ]
