{-# LANGUAGE ForeignFunctionInterface #-}

-- | The core's services and glue.c's helpers, as seen from Haskell.
--
-- Every import that can lead back into Haskell (ugly_export may release a
-- duplicate ref; raising exports the payload) is @safe@: an @unsafe@ call
-- must not re-enter the RTS. Only glue.c's setters, which do nothing but
-- write a struct, are @unsafe@.
module Ugly.Native
  ( Ctx, Rt, Val, Handle, Err
  , Status, Kind
  , ok, errRaised, errType, errNotFound, errNotCapable, errStale, errNotSupported, errNotImplemented, errNoMem
  , noHandle
  , kAny, kNull, kBool, kInt, kDouble, kString, kBytes, kHandle
  , traitMembers, traitElements, traitCallable, traitInstantiable
  , c_ugly_export, c_ugly_fail, c_ugly_raise_native
  , c_ugly_val_set_string, c_ugly_val_set_bytes
  , c_set_null, c_set_bool, c_set_int, c_set_double, c_set_handle, c_names
  , c_ugly_resolve
  , c_arg, c_kind, c_bool, c_int, c_double, c_ptr, c_len, c_handle
    -- calling out
  , c_enter, c_leave, c_current_rt, c_ctx, c_val_size, c_val_at, c_set_string, c_set_bytes
  , c_ugly_dup, c_ugly_release, c_ugly_val_release
  , c_ugly_call, c_ugly_invoke, c_ugly_get_member, c_ugly_get_index, c_ugly_length, c_ugly_member_names
  , c_ugly_describe
  , c_ugly_take_error, c_ugly_restore_error, c_ugly_error_free, c_ugly_error_payload, c_ugly_raise_handle
  , c_ugly_error_status, c_ugly_error_language, c_ugly_error_type, c_ugly_error_message
  ) where

import Data.Int
import Data.Word
import Foreign.C.String
import Foreign.C.Types
import Foreign.Ptr

data Ctx
data Rt
data Val
data Err

type Handle = Word64
type Status = CInt
type Kind = CInt

-- ugly_status and ugly_kind, as glue.c pins them.
ok, errRaised, errType, errNotFound, errNotCapable, errStale, errNotSupported, errNotImplemented, errNoMem :: Status
ok = 0
errRaised = 1
errType = 2
errNotFound = 3
errNotCapable = 4
errStale = 5
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

noHandle :: Handle
noHandle = 0

traitMembers, traitElements, traitCallable, traitInstantiable :: Word32
traitMembers = 1
traitElements = 2
traitCallable = 4
traitInstantiable = 8

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

foreign import ccall safe "ugly_resolve"
  c_ugly_resolve :: Ptr Ctx -> Ptr Rt -> Handle -> Ptr (Ptr ()) -> IO Status

foreign import ccall unsafe "ugly_hs_arg" c_arg :: Ptr Val -> CSize -> IO (Ptr Val)
foreign import ccall unsafe "ugly_hs_kind" c_kind :: Ptr Val -> IO Kind
foreign import ccall unsafe "ugly_hs_bool" c_bool :: Ptr Val -> IO CInt
foreign import ccall unsafe "ugly_hs_int" c_int :: Ptr Val -> IO Int64
foreign import ccall unsafe "ugly_hs_double" c_double :: Ptr Val -> IO Double
foreign import ccall unsafe "ugly_hs_ptr" c_ptr :: Ptr Val -> IO CString
foreign import ccall unsafe "ugly_hs_len" c_len :: Ptr Val -> IO CSize
foreign import ccall unsafe "ugly_hs_handle" c_handle :: Ptr Val -> IO Handle


-- Calling out (Ugly.Marshal). The core's operations can call back into
-- Haskell (a callback, a release), so they're all safe.
foreign import ccall unsafe "ugly_hs_enter" c_enter :: Ptr Rt -> IO (Ptr Rt)
foreign import ccall unsafe "ugly_hs_leave" c_leave :: Ptr Rt -> IO ()
foreign import ccall unsafe "ugly_hs_current_rt" c_current_rt :: IO (Ptr Rt)
foreign import ccall unsafe "ugly_hs_ctx" c_ctx :: IO (Ptr Ctx)
foreign import ccall unsafe "ugly_hs_val_size" c_val_size :: IO CSize
foreign import ccall unsafe "ugly_hs_val_at" c_val_at :: Ptr Val -> CSize -> IO (Ptr Val)
foreign import ccall unsafe "ugly_hs_set_string" c_set_string :: Ptr Val -> CString -> CSize -> IO ()
foreign import ccall unsafe "ugly_hs_set_bytes" c_set_bytes :: Ptr Val -> Ptr Word8 -> CSize -> IO ()

foreign import ccall safe "ugly_dup" c_ugly_dup :: Ptr Ctx -> Handle -> Ptr Handle -> IO Status
foreign import ccall safe "ugly_release" c_ugly_release :: Ptr Ctx -> Handle -> IO ()
foreign import ccall safe "ugly_val_release" c_ugly_val_release :: Ptr Ctx -> Ptr Val -> IO ()
foreign import ccall safe "ugly_call"
  c_ugly_call :: Ptr Ctx -> Handle -> Ptr Val -> CSize -> Kind -> Ptr Val -> IO Status
foreign import ccall safe "ugly_invoke"
  c_ugly_invoke :: Ptr Ctx -> Handle -> CString -> Ptr Val -> CSize -> Kind -> Ptr Val -> IO Status
foreign import ccall safe "ugly_get_member"
  c_ugly_get_member :: Ptr Ctx -> Handle -> CString -> Kind -> Ptr Val -> IO Status
foreign import ccall safe "ugly_get_index"
  c_ugly_get_index :: Ptr Ctx -> Handle -> Int64 -> Kind -> Ptr Val -> IO Status
foreign import ccall safe "ugly_length" c_ugly_length :: Ptr Ctx -> Handle -> Ptr Int64 -> IO Status
foreign import ccall safe "ugly_member_names" c_ugly_member_names :: Ptr Ctx -> Handle -> Ptr Handle -> IO Status
foreign import ccall safe "ugly_describe" c_ugly_describe :: Ptr Ctx -> Handle -> Ptr Word32 -> Ptr Val -> IO Status

foreign import ccall unsafe "ugly_take_error" c_ugly_take_error :: Ptr Ctx -> IO (Ptr Err)
foreign import ccall unsafe "ugly_restore_error" c_ugly_restore_error :: Ptr Ctx -> Ptr Err -> IO Status
-- Frees the payload's handle too, which can release a ref of ours.
foreign import ccall safe "ugly_error_free" c_ugly_error_free :: Ptr Err -> IO ()
foreign import ccall safe "ugly_error_payload" c_ugly_error_payload :: Ptr Ctx -> Ptr Err -> Ptr Handle -> IO Status
foreign import ccall safe "ugly_raise_handle"
  c_ugly_raise_handle :: Ptr Ctx -> Ptr Rt -> CString -> CString -> Handle -> IO Status
foreign import ccall unsafe "ugly_error_status" c_ugly_error_status :: Ptr Err -> IO Status
foreign import ccall unsafe "ugly_error_language" c_ugly_error_language :: Ptr Err -> IO CString
foreign import ccall unsafe "ugly_error_type" c_ugly_error_type :: Ptr Err -> IO CString
foreign import ccall unsafe "ugly_error_message" c_ugly_error_message :: Ptr Err -> IO CString
