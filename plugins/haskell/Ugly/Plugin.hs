{-# LANGUAGE ForeignFunctionInterface #-}

-- | The plugin's Haskell side: the entry points haskell.c calls, as
-- @foreign export@s.
--
-- Skeleton: a handshake, and runtimes that come and go. A runtime is plain
-- Haskell state (the modules it has loaded, later), held by C through a
-- 'StablePtr': GHC has no global interpreter state, so runtimes are cheap
-- and independent (UGLY_CAP_MULTI_RUNTIME), and the threaded RTS takes calls
-- from any OS thread (UGLY_CAP_ANY_THREAD).
--
-- Every entry point will catch every exception at the boundary (an
-- exception escaping a foreign export ends the process), and force what it
-- returns inside that catch, or laziness lets an error escape later.
module Ugly.Plugin () where

import Data.IORef
import Foreign.C.Types
import Foreign.StablePtr

-- | haskell.c's BRIDGE_ABI: the two sides must come from the same build.
bridgeAbi :: CInt
bridgeAbi = 1

-- | A runtime. Its modules will go here.
newtype Runtime = Runtime (IORef ())

hsAbi :: IO CInt
hsAbi = return bridgeAbi

rtNew :: IO (StablePtr Runtime)
rtNew = newIORef () >>= newStablePtr . Runtime

rtDelete :: StablePtr Runtime -> IO ()
rtDelete = freeStablePtr

foreign export ccall "ugly_hs_abi" hsAbi :: IO CInt
foreign export ccall "ugly_hs_rt_new" rtNew :: IO (StablePtr Runtime)
foreign export ccall "ugly_hs_rt_delete" rtDelete :: StablePtr Runtime -> IO ()
