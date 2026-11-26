-- | Foreign values, from Haskell: what an export takes as a 'Foreign' (or
-- a 'Value' holding one), and what operations on it return.
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
  ( -- * Values
    Value (..), Foreign, ToUgly (..), FromUgly, toValue, fromValue
    -- * Operations on a foreign value
  , call, invoke, get, index, size, members, typeOf
    -- * Any callable value, Haskell's own or foreign
  , callValue
    -- * Errors
  , ForeignError, errorStatus, errorLanguage, errorType, errorMessage, errorPayload
  , UglyException, uglyToException, uglyFromException, exceptionValue, rethrow
  ) where

import Ugly.Marshal
import Ugly.Value

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
