-- | UGLY from Haskell: a Haskell program as the host ('withUgly'), and
-- foreign values, what an export takes as a 'Foreign' (or a 'Value'
-- holding one), and what operations on it return.
--
-- > describe :: Foreign -> IO String
-- > describe obj = do
-- >   n <- Ugly.size obj
-- >   v <- Ugly.invoke obj "get" [Ugly.toValue (0 :: Int)]
-- >   s <- Ugly.fromValue v
-- >   return (show n ++ " items, the first is " ++ s)
--
-- Every operation runs on the calling thread, through the core: a runtime
-- bound to another thread refuses it (a ForeignError, WRONG_THREAD). A
-- failure is a 'ForeignError', which Haskell can catch; if it escapes, the
-- error goes on unchanged. A foreign callable taken as a Haskell function
-- type ('Collect') is called when its result is forced.
--
-- Values crossing back to the caller follow the same rules as an export's
-- arguments and results: 'toValue' (ToUgly) and 'fromValue' (FromUgly).
module Ugly
  ( -- * A Haskell program as the host
    withUgly, module_, eval
    -- * Values
  , Value (..), Foreign, ToUgly (..), FromUgly, toValue, fromValue
    -- * Operations on a foreign value
  , call, call_, invoke, invoke_, get, index, size, members, typeOf
    -- * Iteration and strings
  , toList, iter, next, toString
    -- * Any callable value, Haskell's own or foreign
  , callValue, callValue_
    -- * Errors
  , ForeignError, errorStatus, errorLanguage, errorType, errorMessage, errorPayload
  , UglyException, uglyToException, uglyFromException, exceptionValue, rethrow
  ) where

import Control.Exception (bracket_, throwIO, ErrorCall (..))
import System.IO.Unsafe (unsafeInterleaveIO)

import Ugly.Marshal
import Ugly.Value hiding (callValue)
import qualified Ugly.Value as V

-- | Runs a program's code with UGLY: registers the plugin, attaches to the
-- program's own runtime, and detaches at the end (nested uses are fine).
-- The program is built against the package 'ugly', dynamically, with the
-- threaded RTS:
--
-- > ghc -dynamic -threaded -package-db <libdir>/ugly/haskell/package.db -package ugly Main.hs
--
-- > {-# LANGUAGE OverloadedStrings #-}
-- > main = Ugly.withUgly $ do
-- >   json <- Ugly.module_ "python" "json"
-- >   xs <- Ugly.invoke json "loads" ["[1, 2, 3]"]
-- >   print (sum (xs :: [Int]))
--
-- Other languages start on first use, on the calling thread (a runtime
-- bound to a thread, Lua's, must then be used from that one).
withUgly :: IO a -> IO a
withUgly = bracket_ attach detach

-- | A module of another language: ugly.module(lang, name).
module_ :: String -> String -> IO Foreign
module_ lang name = do
  v <- moduleIn lang name
  case v of
    VForeign f -> return f
    _ -> throwIO (ErrorCall ("ugly: " ++ lang ++ " module " ++ name ++ " isn't a foreign value"))

-- Results: every operation returns the type its caller wants ('FromUgly':
-- a [Int], a String, a Foreign, or the Value itself), converted as an
-- argument would be; a refusal is a TypeError. The ones ending in _ drop
-- the result, for a call made for its effect (no type to infer).
--
-- Arguments: a list of Values. With OverloadedStrings, string and number
-- literals are Values ("x", 2, -1.5); anything else goes through toValue.

-- | Code in another language, evaluated: ugly.eval(lang, code).
eval :: FromUgly r => String -> String -> IO r
eval lang code = evalIn lang code >>= fromValue

-- | Any Haskell value as an argument ('ToUgly'): a function, a list, a
-- value of a type a module exports.
toValue :: ToUgly a => a -> Value
toValue = toUgly

-- | Calls it with these arguments.
call :: FromUgly r => Foreign -> [Value] -> IO r
call f args = foreignCall f args >>= fromValue

call_ :: Foreign -> [Value] -> IO ()
call_ f args = () <$ foreignCall f args

-- | Calls its method: in its own language's terms (a Python method, a Lua
-- function in its table).
invoke :: FromUgly r => Foreign -> String -> [Value] -> IO r
invoke f m args = foreignInvoke f m args >>= fromValue

invoke_ :: Foreign -> String -> [Value] -> IO ()
invoke_ f m args = () <$ foreignInvoke f m args

-- | A member.
get :: FromUgly r => Foreign -> String -> IO r
get f name = foreignGet f name >>= fromValue

-- | An element (from 0).
index :: FromUgly r => Foreign -> Int -> IO r
index f i = foreignIndex f i >>= fromValue

-- | Calls any callable Value, Haskell's own or foreign.
callValue :: FromUgly r => Value -> [Value] -> IO r
callValue v args = V.callValue v args >>= fromValue

callValue_ :: Value -> [Value] -> IO ()
callValue_ v args = () <$ V.callValue v args

-- | Its number of elements.
size :: Foreign -> IO Int
size = foreignLength

-- | Its members' names.
members :: Foreign -> IO [String]
members = foreignNames

-- | Its type's name, in its own language.
typeOf :: Foreign -> IO String
typeOf = foreignType

-- | Its own iteration (ugly_iter, contract §2) as a lazy list, like
-- hGetContents: each cell, when forced, is one step, its element converted
-- then (a refusal is a TypeError thrown from there). An infinite one is
-- fine as long as it isn't forced to the end; an error of the iterator is
-- thrown from the cell that ran into it.
--
-- > gen <- Ugly.eval "python" "(x * x for x in range(10))"
-- > squares <- Ugly.toList gen
-- > print (take 3 squares :: [Int])      -- runs the generator three steps
toList :: FromUgly a => Foreign -> IO [a]
toList f = foreignList f >>= mapM' fromValue
  where
    mapM' conv xs = unsafeInterleaveIO $ case xs of
      [] -> return []
      v : vs -> do
        y <- unsafeInterleaveIO (conv v)
        (y :) <$> mapM' conv vs

-- | Its iterator, for stepping it by hand with 'next' (on an iterator, itself).
iter :: Foreign -> IO Foreign
iter = foreignIter

-- | The iterator's next value, or Nothing at the end.
next :: FromUgly r => Foreign -> IO (Maybe r)
next it = foreignNext it >>= traverse fromValue

-- | Its own language's string for it (ugly_to_string): Python's str(),
-- Lua's tostring, Java's toString().
toString :: Foreign -> IO String
toString = foreignString

-- | Raises a value again: an exception that came home (as a value, from
-- 'exceptionValue' or a payload) as itself, or a foreign value as an error
-- whose payload it is.
rethrow :: Value -> IO a
rethrow = rethrowValue
