{-# LANGUAGE ExistentialQuantification #-}

-- | The types values cross as, both ways (Ugly.Value has the classes,
-- Ugly.Marshal the conversions to and from C).
module Ugly.Types
  ( Value (..), Box (..), Info (..), Members (..), Elems (..), Arg (..), Conv
  , Want (..), Call (..), Refused (..), plainCall
  , Foreign (..), ForeignError (..)
  , info
  ) where

import Control.Exception (Exception (..))
import qualified Data.ByteString as B
import Data.Int
import Data.IORef (IORef)
import qualified Data.Text as T
import Data.Typeable
import Foreign.ForeignPtr (ForeignPtr)
import Foreign.Ptr (Ptr)

import Ugly.Native (Err, Handle, Status)

-- | What crosses: a primitive, by value, a Haskell value in a box, or a
-- foreign value, by handle.
data Value
  = VNull
  | VBool !Bool
  | VInt !Int64
  | VDouble !Double
  | VString !T.Text
  | VBytes !B.ByteString
  | VBox !Box
  | VForeign !Foreign

-- | A Haskell value, its identity (the value itself), and what can be asked
-- of it. 'Typeable', so that it can be cast back when it comes home.
data Box = forall a. Typeable a => Box a Info

data Info = Info
  { infoType :: String              -- ^ describe's type name: "Note", "[Int]", "module"
  , infoShow :: Maybe String        -- ^ its text, for want = string (lazy: forced when asked)
  , infoMembers :: Maybe Members    -- ^ MEMBERS
  , infoElems :: Maybe Elems        -- ^ ELEMENTS
  , infoCall :: Maybe (Call -> [Arg] -> Either Refused (Int, IO Value))   -- ^ CALLABLE: match, then run
  , infoMethods :: Maybe (String -> Maybe Value)   -- ^ for invoke: functions taking this value last
  }

-- | A box with nothing but a type name.
info :: String -> Info
info ty = Info ty Nothing Nothing Nothing Nothing Nothing

data Members = Members
  { memberNames :: [String]
  , memberGet :: String -> Maybe Value
  }

-- | Read-only elements. The length is lazy: an infinite list has none, but
-- its elements can still be read.
data Elems = Elems
  { elemsLength :: Int
  , elemsIndex :: Int -> Maybe Value    -- ^ Nothing outside the elements
  }

-- | An argument, as passed: a primitive, a Haskell value (a handle whose
-- home is this runtime, or a value already in Haskell), or a foreign
-- value. A foreign argument is borrowed: taking it (running the action)
-- makes a 'Foreign' of its own.
data Arg
  = ANull
  | ABool Bool
  | AInt Int64
  | ADouble Double
  | AString T.Text
  | ABytes B.ByteString
  | AHome Box
  | AForeign (IO Foreign)

-- | A conversion: why not, or its cost (0 is exact) and how. The "how" runs
-- only for the overload that's chosen.
type Conv a = Either String (Int, a)

-- | The kind the caller wants (ugly_kind).
data Want = WantAny | WantNull | WantBool | WantInt | WantDouble | WantString | WantBytes | WantHandle
  deriving (Eq, Show)

-- | What a call asks besides its arguments: the kind it wants back, which
-- ranks a polymorphic function's instantiations by their result, and the
-- hint, type arguments as TypeApplications gives them ("_" for any), which
-- picks among them.
data Call = Call
  { callWant :: Want
  , callHint :: Maybe [String]
  }

plainCall :: Call
plainCall = Call WantAny Nothing

-- | Why a call can't be made: its arguments (or the result wanted) don't
-- fit (UGLY_ERR_TYPE), or the hint names no instantiation (UGLY_ERR_NOT_FOUND).
data Refused = Refused String | NoInstance String

-- | A value from another language: an owned handle, released by a
-- finalizer once Haskell drops it (on a finalizer thread: the core defers
-- the release to the value's home thread).
data Foreign = Foreign
  { foreignHandle :: !Handle
  , foreignKeep :: !(ForeignPtr ())    -- ^ its finalizer releases the handle
  }

-- | A failure of an operation on a foreign value: a foreign exception, or
-- any other error (no such member, a stale handle), as the core reported
-- it. If it escapes Haskell, the original error goes on unchanged.
data ForeignError = ForeignError
  { errorStatus :: Status
  , errorLanguage :: String           -- ^ "python", "lua", "c", "core"...
  , errorType :: String               -- ^ in that language's terms: "ValueError"
  , errorMessage :: String
  , errorPayload :: Maybe Value       -- ^ the exception object, if any
  , errorOwned :: IORef (Ptr Err)     -- ^ the core's error, until it's restored or freed
  }

instance Show ForeignError where
  show e = errorLanguage e ++ " " ++ errorType e ++ ": " ++ errorMessage e

instance Exception ForeignError
