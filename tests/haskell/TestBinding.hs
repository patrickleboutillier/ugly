{-# LANGUAGE OverloadedStrings, ScopedTypeVariables, TemplateHaskell #-}

-- | The Haskell binding's test (TAP): a Haskell program as the host, built
-- against the package 'ugly' (-dynamic -threaded, and -rdynamic so that
-- its own export module, below, is found in the process). Languages that
-- aren't built (UGLY_TEST_LANGS) are skipped.
module Main (main) where

import Control.Exception
import Control.Monad (unless)
import Data.IORef
import System.Environment (lookupEnv)
import System.Exit (exitFailure)
import System.IO

import qualified Ugly
import Ugly.Export (uglyModule)

-- The program's own module, for the other languages: "TestBinding".
double :: Int -> Int
double = (* 2)

greet :: String -> String
greet name = "hello, " ++ name

uglyModule "TestBinding" ['double, 'greet]


data Tap = Tap (IORef Int) (IORef Bool)

ok :: Tap -> Bool -> String -> IO ()
ok (Tap n bad) good what = do
  i <- atomicModifyIORef' n (\k -> (k + 1, k + 1))
  putStrLn ((if good then "ok " else "not ok ") ++ show i ++ " - " ++ what)
  unless good (writeIORef bad True)

skip :: Tap -> String -> IO ()
skip (Tap n _) what = do
  i <- atomicModifyIORef' n (\k -> (k + 1, k + 1))
  putStrLn ("ok " ++ show i ++ " - " ++ what ++ " # SKIP")

-- | Whether an action throws an exception this predicate accepts.
throws :: Exception e => (e -> Bool) -> IO a -> IO Bool
throws p act = either p (const False) <$> try (act >> return ())

main :: IO ()
main = do
  hSetBuffering stdout LineBuffering
  langs <- maybe [] (words . map (\c -> if c == ',' then ' ' else c)) <$> lookupEnv "UGLY_TEST_LANGS"
  tap@(Tap n bad) <- Tap <$> newIORef 0 <*> newIORef False

  Ugly.withUgly $ do
    ok tap True "withUgly: the plugin registered, attached to this program's runtime"

    if "python" `notElem` langs then skip tap "python isn't built" else do
      json <- Ugly.module_ "python" "json"
      xs <- Ugly.invoke json "loads" ["[1, 2, 3]"]
      ok tap (xs == [1, 2, 3 :: Int]) "json.loads \"[1, 2, 3]\": a Python list, as a Haskell [Int] (the result by type)"
      s <- Ugly.invoke json "dumps" ["h\233llo"]
      ok tap (s == ("\"h\\u00e9llo\"" :: String)) "json.dumps \"h\233llo\": a string literal is a Value, a Python str"
      neg <- Ugly.invoke json "dumps" [-1.5]
      ok tap (neg == ("-1.5" :: String)) "json.dumps -1.5: a negative number literal is a Value"
      v <- Ugly.invoke json "loads" ["{\"a\": 1}"]
      ok tap (case v of Ugly.VForeign _ -> True; _ -> False) "json.loads to a Value: the raw result, a Python dict (VForeign)"
      Ugly.invoke_ json "dumps" [1]
      ok tap True "invoke_: a call made for its effect, its result dropped"

      twice <- Ugly.eval "python" "lambda f, x: f(f(x))"
      r <- Ugly.callValue twice [Ugly.toValue ((+ 1) :: Int -> Int), 40]
      ok tap (r == (42 :: Int)) "Python calls back a Haskell function: f(f(40)), f = (+ 1)"

      caught <- throws (\(e :: Ugly.ForeignError) -> Ugly.errorLanguage e == "python" && Ugly.errorType e == "JSONDecodeError")
        (Ugly.invoke_ json "loads" ["{"])
      ok tap caught "json.loads \"{\": Python's JSONDecodeError, caught as a ForeignError"

      call0 <- Ugly.eval "python" "lambda f: f(0)"
      home <- throws (\(ErrorCall m) -> m == "zero")
        (Ugly.callValue_ call0 [Ugly.toValue ((\x -> if x == 0 then error "zero" else x) :: Int -> Int)])
      ok tap home "a Haskell error raised through Python comes home as itself (an ErrorCall)"

      own <- Ugly.eval "python" "__import__('ugly').module('haskell', 'TestBinding').double(21)"
      ok tap (own == (42 :: Int)) "Python uses this program's own export module (found in the process)"
      g <- Ugly.eval "python" "__import__('ugly').module('haskell', 'TestBinding').greet('python')"
      ok tap (g == ("hello, python" :: String)) "  ... greet(\"python\")"

    if "lua" `notElem` langs then skip tap "lua isn't built" else do
      str <- Ugly.module_ "lua" "string"
      u <- Ugly.invoke str "upper" ["abc"]
      ok tap (u == ("ABC" :: String)) "string.upper \"abc\": a Lua module from Haskell, on this thread"
      rep <- Ugly.invoke str "rep" ["ab", 3]
      ok tap (rep == ("ababab" :: String)) "string.rep \"ab\" 3: string and number literals as Values"
      wrong <- throws (\(_ :: TypeError) -> True) (Ugly.invoke str "upper" ["abc"] >>= \(i :: Int) -> return i)
      ok tap wrong "a result that isn't the type asked for: a TypeError"

    Ugly.withUgly (ok tap True "withUgly nested: attached once")

  ok tap True "detached at the end of withUgly"
  count <- readIORef n
  putStrLn ("1.." ++ show count)
  failed <- readIORef bad
  if failed then exitFailure else return ()
