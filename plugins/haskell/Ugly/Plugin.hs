{-# LANGUAGE ForeignFunctionInterface #-}

-- | The plugin's Haskell side: the entry points haskell.c puts in the
-- vtable, as @foreign export@s with the vtable's C signatures.
--
-- A runtime is plain Haskell state, held by C through a 'StablePtr': GHC has
-- no global interpreter state, so runtimes are cheap and independent
-- (UGLY_CAP_MULTI_RUNTIME), and the threaded RTS takes calls from any OS
-- thread (UGLY_CAP_ANY_THREAD).
--
-- A ref is a 'StablePtr' to a 'Ref' (Ugly.Marshal): a box, and the
-- 'StableName' its identity was taken from, kept alive so that the key
-- stays unique.
--
-- Every entry point catches every exception ('guarded'): one escaping a
-- foreign export ends the process. Laziness makes that a real risk, so
-- values are forced inside the catch, when they're converted. A foreign
-- error escaping goes on unchanged.
module Ugly.Plugin () where

import Control.Exception (evaluate)
import qualified Data.ByteString.Unsafe as BU
import Data.Int
import Data.IORef
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import Data.Word
import Foreign.C.String
import Foreign.C.Types
import Foreign.Marshal.Array
import Foreign.Ptr
import Foreign.StablePtr
import Foreign.Storable

import Ugly.Marshal
import Ugly.Native
import Ugly.Value

-- | haskell.c's BRIDGE_ABI: the two sides must come from the same build.
bridgeAbi :: CInt
bridgeAbi = 5

-- | A runtime. Modules are libraries, loaded once per process (haskell.c),
-- so a runtime has no state yet.
newtype Runtime = Runtime (IORef ())

-- The entry points.

hsAbi :: IO CInt
hsAbi = return bridgeAbi

rtNew :: IO (StablePtr Runtime)
rtNew = newIORef () >>= newStablePtr . Runtime

rtDelete :: StablePtr Runtime -> IO ()
rtDelete = freeStablePtr

-- | A module, from the stable pointer its library's foreign export gave
-- (haskell.c found it: Ugly.Export.uglyModuleFrom). Taken: freed here.
hsImport :: Ptr Ctx -> Ptr Rt -> StablePtr Box -> Ptr Val -> IO Status
hsImport ctx rt root out = guarded ctx rt $ do
  m <- deRefStablePtr root
  freeStablePtr root
  put ctx rt kAny (VBox m) out

hsDescribe :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Word32 -> Ptr Val -> IO Status
hsDescribe ctx rt ref traits typeName' = guarded ctx rt $ do
  Box _ i <- deref ref
  let has f bit = maybe 0 (const bit) (f i)
  poke traits (has infoMembers traitMembers + has infoElems traitElements + has infoCall traitCallable)
  setText ctx typeName' (T.pack (infoType i))

-- | A box's members, or a refusal.
withMembers :: Ptr Ctx -> Ptr Rt -> Ptr () -> (Members -> IO Status) -> IO Status
withMembers ctx rt ref k = do
  Box _ inf <- deref ref
  case infoMembers inf of
    Just ms -> k ms
    Nothing -> failWith ctx rt errNotCapable "TypeError" ("a value of type " ++ infoType inf ++ " has no members")

withElems :: Ptr Ctx -> Ptr Rt -> Ptr () -> (Elems -> IO Status) -> IO Status
withElems ctx rt ref k = do
  Box _ inf <- deref ref
  case infoElems inf of
    Just es -> k es
    Nothing -> failWith ctx rt errNotCapable "TypeError" ("a value of type " ++ infoType inf ++ " has no elements")

hsGetMember :: Ptr Ctx -> Ptr Rt -> Ptr () -> CString -> Kind -> Ptr Val -> IO Status
hsGetMember ctx rt ref cname want out = guarded ctx rt $ withMembers ctx rt ref $ \ms -> do
  name <- peekUtf8 cname
  case memberGet ms name of
    Just v -> put ctx rt want v out
    Nothing -> failWith ctx rt errNotFound "NotInScope" ("Variable not in scope: " ++ name)

hsMemberNames :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Handle -> IO Status
hsMemberNames ctx rt ref out = guarded ctx rt $ withMembers ctx rt ref $ \ms -> do
  let encoded = map (TE.encodeUtf8 . T.pack) (memberNames ms)
      n = length encoded
  allocaArray n $ \ptrs -> allocaArray n $ \lens ->
    let go _ [] = c_names ctx ptrs lens (fromIntegral n) out
        go i (b : bs) = BU.unsafeUseAsCStringLen b $ \(p, len) -> do
          pokeElemOff ptrs i p
          pokeElemOff lens i (fromIntegral len)
          go (i + 1) bs
    in go 0 encoded

hsLength :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Int64 -> IO Status
hsLength ctx rt ref out = guarded ctx rt $ withElems ctx rt ref $ \es -> do
  n <- evaluate (elemsLength es)
  poke out (fromIntegral n)
  return ok

hsGetIndex :: Ptr Ctx -> Ptr Rt -> Ptr () -> Int64 -> Kind -> Ptr Val -> IO Status
hsGetIndex ctx rt ref i want out = guarded ctx rt $ withElems ctx rt ref $ \es ->
  case elemsIndex es (fromIntegral i) of
    Just v -> put ctx rt want v out
    Nothing -> failWith ctx rt errNotFound "ArrayException" "index out of range"

-- | The arguments, decoded (Ugly.Marshal.readArg).
decodeArgs :: Ptr Ctx -> Ptr Rt -> Ptr Val -> CSize -> IO (Either String [Arg])
decodeArgs ctx rt argv nargs =
  sequence <$> mapM (\i -> c_arg argv (fromIntegral i) >>= readArg ctx rt) [0 .. fromIntegral nargs - 1 :: Int]   -- nargs is unsigned: 0 - 1 wraps

-- | The call's want, and its hint (the hint call option; NULL for none).
callOf :: Kind -> CString -> IO Call
callOf want chint = do
  hint <- if chint == nullPtr then return Nothing else Just . parseHint <$> peekUtf8 chint
  return (Call (wantOf want) hint)
  where
    wantOf k = maybe WantAny id (lookup k
      [ (kNull, WantNull), (kBool, WantBool), (kInt, WantInt), (kDouble, WantDouble)
      , (kString, WantString), (kBytes, WantBytes), (kHandle, WantHandle) ])

-- | Calls what v is, with these arguments.
callValue :: Ptr Ctx -> Ptr Rt -> String -> Value -> [Arg] -> Call -> Kind -> Ptr Val -> IO Status
callValue ctx rt name v args call want out = case v of
  VBox (Box _ i) | Just f <- infoCall i -> case f call args of
    Left (Refused why) -> failWith ctx rt errType "TypeError" (name ++ ": " ++ why)
    Left (NoInstance why) -> failWith ctx rt errNotFound "TypeError" (name ++ ": " ++ why)
    Right (_, run) -> run >>= \r -> put ctx rt want r out
  _ -> failWith ctx rt errNotCapable "TypeError" (name ++ " isn't a function")

hsCall :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Val -> CSize -> CString -> Kind -> Ptr Val -> IO Status
hsCall ctx rt ref argv nargs hint want out = guarded ctx rt $ do
  b@(Box _ i) <- deref ref
  decoded <- decodeArgs ctx rt argv nargs
  call <- callOf want hint
  case decoded of
    Left why -> failWith ctx rt errType "TypeError" why
    Right args -> callValue ctx rt ("a value of type " ++ infoType i) (VBox b) args call want out

-- | A method call. On a module (anything with members), its function, with
-- no receiver. On any other box, a function from the module that exported
-- its type, with the receiver last: x.m(a, b) is m a b x.
hsInvoke :: Ptr Ctx -> Ptr Rt -> Ptr () -> CString -> Ptr Val -> CSize -> CString -> Kind -> Ptr Val -> IO Status
hsInvoke ctx rt ref cname argv nargs hint want out = guarded ctx rt $ do
  b@(Box _ i) <- deref ref
  name <- peekUtf8 cname
  decoded <- decodeArgs ctx rt argv nargs
  call <- callOf want hint
  case decoded of
    Left why -> failWith ctx rt errType "TypeError" why
    Right args -> case (infoMembers i >>= (`memberGet` name), infoMethods i >>= ($ name)) of
      (Just f, _) -> callValue ctx rt name f args call want out
      (_, Just f) -> callValue ctx rt name f (args ++ [AHome b]) call want out
      _ -> failWith ctx rt errNotFound "NotInScope" ("Variable not in scope: " ++ name ++ " (for a value of type " ++ infoType i ++ ")")

-- | Called on any thread, possibly in the middle of a call: freeing a
-- stable pointer is safe anywhere.
hsRelease :: Ptr Rt -> Ptr () -> IO ()
hsRelease _ ref = do
  freeStablePtr (castPtrToStablePtr ref :: StablePtr Ref)
  atomicModifyIORef' liveRefs (\n -> (n - 1, ()))

hsLiveRefs :: IO CInt
hsLiveRefs = fromIntegral <$> readIORef liveRefs

hsLiveForeign :: IO CInt
hsLiveForeign = fromIntegral <$> readIORef liveForeign

foreign export ccall "ugly_hs_abi" hsAbi :: IO CInt
foreign export ccall "ugly_hs_rt_new" rtNew :: IO (StablePtr Runtime)
foreign export ccall "ugly_hs_rt_delete" rtDelete :: StablePtr Runtime -> IO ()
foreign export ccall "ugly_hs_import" hsImport :: Ptr Ctx -> Ptr Rt -> StablePtr Box -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_describe" hsDescribe :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Word32 -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_get_member" hsGetMember :: Ptr Ctx -> Ptr Rt -> Ptr () -> CString -> Kind -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_member_names" hsMemberNames :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Handle -> IO Status
foreign export ccall "ugly_hs_length" hsLength :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Int64 -> IO Status
foreign export ccall "ugly_hs_get_index" hsGetIndex :: Ptr Ctx -> Ptr Rt -> Ptr () -> Int64 -> Kind -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_release" hsRelease :: Ptr Rt -> Ptr () -> IO ()
foreign export ccall "ugly_hs_live_refs" hsLiveRefs :: IO CInt
foreign export ccall "ugly_hs_live_foreign" hsLiveForeign :: IO CInt
foreign export ccall "ugly_hs_call" hsCall :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Val -> CSize -> CString -> Kind -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_invoke" hsInvoke :: Ptr Ctx -> Ptr Rt -> Ptr () -> CString -> Ptr Val -> CSize -> CString -> Kind -> Ptr Val -> IO Status
