{-# LANGUAGE ForeignFunctionInterface #-}

-- | The core's services and glue.c's helpers, as seen from Haskell.
--
-- Every import that can lead back into Haskell (ugly_export may release a
-- duplicate ref; raising exports the payload) is @safe@: an @unsafe@ call
-- must not re-enter the RTS. Only glue.c's setters, which do nothing but
-- write a struct, are @unsafe@.
module Ugly.Native
  ( Ctx, Rt, Val, Handle
  , Status, Kind
  , ok, errRaised, errType, errNotFound, errNotCapable, errNotSupported, errNotImplemented, errNoMem
  , kAny, kNull, kBool, kInt, kDouble, kString, kBytes, kHandle
  , traitMembers, traitElements, traitCallable
  , c_ugly_export, c_ugly_fail, c_ugly_raise_native
  , c_ugly_val_set_string, c_ugly_val_set_bytes
  , c_set_null, c_set_bool, c_set_int, c_set_double, c_set_handle, c_names
  ) where

import Data.Int
import Data.Word
import Foreign.C.String
import Foreign.C.Types
import Foreign.Ptr

data Ctx
data Rt
data Val

type Handle = Word64
type Status = CInt
type Kind = CInt

-- ugly_status and ugly_kind, as glue.c pins them.
ok, errRaised, errType, errNotFound, errNotCapable, errNotSupported, errNotImplemented, errNoMem :: Status
ok = 0
errRaised = 1
errType = 2
errNotFound = 3
errNotCapable = 4
errNotSupported = 9
errNotImplemented = 10
errNoMem = 12

kAny, kNull, kBool, kInt, kDouble, kString, kBytes, kHandle :: Kind
kAny = 0
kNull = 1
kBool = 2
kInt = 3
kDouble = 4
kString = 5
kBytes = 6
kHandle = 7

traitMembers, traitElements, traitCallable :: Word32
traitMembers = 1
traitElements = 2
traitCallable = 4

foreign import ccall safe "ugly_export"
  c_ugly_export :: Ptr Ctx -> Ptr Rt -> Ptr () -> CUIntPtr -> Ptr Handle -> IO Status
foreign import ccall safe "ugly_fail"
  c_ugly_fail :: Ptr Ctx -> Ptr Rt -> Status -> CString -> CString -> IO Status
foreign import ccall safe "ugly_raise_native"
  c_ugly_raise_native :: Ptr Ctx -> Ptr Rt -> CString -> CString -> Ptr () -> CUIntPtr -> IO Status
foreign import ccall safe "ugly_val_set_string"
  c_ugly_val_set_string :: Ptr Ctx -> Ptr Val -> CString -> CSize -> IO Status
foreign import ccall safe "ugly_val_set_bytes"
  c_ugly_val_set_bytes :: Ptr Ctx -> Ptr Val -> Ptr Word8 -> CSize -> IO Status
foreign import ccall safe "ugly_hs_names"
  c_names :: Ptr Ctx -> Ptr CString -> Ptr CSize -> CSize -> Ptr Handle -> IO Status

foreign import ccall unsafe "ugly_hs_set_null" c_set_null :: Ptr Val -> IO ()
foreign import ccall unsafe "ugly_hs_set_bool" c_set_bool :: Ptr Val -> CInt -> IO ()
foreign import ccall unsafe "ugly_hs_set_int" c_set_int :: Ptr Val -> Int64 -> IO ()
foreign import ccall unsafe "ugly_hs_set_double" c_set_double :: Ptr Val -> Double -> IO ()
foreign import ccall unsafe "ugly_hs_set_handle" c_set_handle :: Ptr Val -> Handle -> IO ()
