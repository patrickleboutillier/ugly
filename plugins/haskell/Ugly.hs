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
  , call, invoke, get, index, size, members, typeOf
    -- * Any callable value, Haskell's own or foreign
  , callValue
    -- * Errors
  , ForeignError, errorStatus, errorLanguage, errorType, errorMessage, errorPayload
  , UglyException, uglyToException, uglyFromException, exceptionValue, rethrow
  ) where

import Control.Exception (bracket_, throwIO, ErrorCall (..))

import Ugly.Marshal
import Ugly.Value

-- | Runs a program's code with UGLY: registers the plugin, attaches to the
-- program's own runtime, and detaches at the end (nested uses are fine).
-- The program is built against the package 'ugly', dynamically, with the
-- threaded RTS:
--
-- > ghc -dynamic -threaded -package-db <libdir>/ugly/haskell/package.db -package ugly Main.hs
--
-- > main = Ugly.withUgly $ do
-- >   json <- Ugly.module_ "python" "json"
-- >   s <- Ugly.invoke json "dumps" [Ugly.toValue [1, 2, 3 :: Int]]
-- >   putStrLn =<< Ugly.fromValue s
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

-- | Code in another language, evaluated: ugly.eval(lang, code).
eval :: String -> String -> IO Value
eval = evalIn

toValue :: ToUgly a => a -> Value
toValue = toUgly

-- | Calls it with these arguments.
call :: Foreign -> [Value] -> IO Value
call = foreignCall

-- | Calls its method: in its own language's terms (a Python method, a Lua
-- function in its table).
invoke :: Foreign -> String -> [Value] -> IO Value
invoke = foreignInvoke

-- | A member.
get :: Foreign -> String -> IO Value
get = foreignGet

-- | An element (from 0).
index :: Foreign -> Int -> IO Value
index = foreignIndex

-- | Its number of elements.
size :: Foreign -> IO Int
size = foreignLength

-- | Its members' names.
members :: Foreign -> IO [String]
members = foreignNames

-- | Its type's name, in its own language.
typeOf :: Foreign -> IO String
typeOf = foreignType

-- | Raises a value again: an exception that came home (as a value, from
-- 'exceptionValue' or a payload) as itself, or a foreign value as an error
-- whose payload it is.
rethrow :: Value -> IO a
rethrow = rethrowValue
