{-# LANGUAGE ExistentialQuantification, ForeignFunctionInterface, ScopedTypeVariables #-}

-- | The plugin's Haskell side: the entry points haskell.c puts in the
-- vtable, as @foreign export@s with the vtable's C signatures.
--
-- A runtime is plain Haskell state, held by C through a 'StablePtr': GHC has
-- no global interpreter state, so runtimes are cheap and independent
-- (UGLY_CAP_MULTI_RUNTIME), and the threaded RTS takes calls from any OS
-- thread (UGLY_CAP_ANY_THREAD).
--
-- A ref is a 'StablePtr' to a 'Ref': a box, and the 'StableName' its
-- identity was taken from, kept alive so that the key stays unique.
--
-- Every entry point catches every exception ('guarded'): one escaping a
-- foreign export ends the process. Laziness makes that a real risk, so
-- values are forced inside the catch, when they're converted.
module Ugly.Plugin () where

import Control.DeepSeq (force)
import Control.Exception
import qualified Data.ByteString as B
import qualified Data.ByteString.Unsafe as BU
import Data.Int
import Data.IORef
import qualified Data.Map as M
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import Data.Typeable
import Data.Word
import Foreign.C.String
import Foreign.C.Types
import Foreign.Marshal.Alloc
import Foreign.Marshal.Array
import Foreign.Ptr
import Foreign.StablePtr
import Foreign.Storable
import qualified GHC.Foreign as GF
import GHC.IO.Encoding (utf8)
import System.IO.Unsafe (unsafePerformIO)
import System.Mem.StableName

import Ugly.Native
import qualified Ugly.ExampleExports as Example
import qualified Ugly.Smoke as Smoke
import Ugly.Value

-- | haskell.c's BRIDGE_ABI: the two sides must come from the same build.
bridgeAbi :: CInt
bridgeAbi = 3

-- | A runtime. Its loaded modules will go here.
newtype Runtime = Runtime (IORef ())

-- | What C holds for a value: its box, and the stable name of its identity.
data Ref = Ref Box Named
data Named = forall a. Named (StableName a)

-- | Modules built into the plugin, until modules load from export tables.
builtins :: M.Map String Box
builtins = M.fromList [("Ugly.Smoke", Smoke.smoke), ("Ugly.Example", Example.uglyExports)]


-- Strings to and from C are UTF-8, whatever the locale.
withUtf8 :: String -> (CString -> IO a) -> IO a
withUtf8 = GF.withCString utf8

peekUtf8 :: CString -> IO String
peekUtf8 = GF.peekCString utf8

-- | A failure that isn't an exception: a status, and a type in Haskell's terms.
failWith :: Ptr Ctx -> Ptr Rt -> Status -> String -> String -> IO Status
failWith ctx rt st ty msg =
  withUtf8 ty $ \t -> withUtf8 msg $ \m -> c_ugly_fail ctx rt st t m

-- | Runs an entry point, turning any exception into UGLY_ERR_RAISED.
guarded :: Ptr Ctx -> Ptr Rt -> IO Status -> IO Status
guarded ctx rt act = act `catch` raise ctx rt

-- | An exception escaping to the core: its type's name, its message, and
-- itself as the payload (a box).
raise :: Ptr Ctx -> Ptr Rt -> SomeException -> IO Status
raise ctx rt e = do
  let ty = case e of SomeException inner -> show (typeOf inner)
  msg <- evaluate (force (displayException e)) `catch` \(_ :: SomeException) -> return ty
  (sp, key) <- newRef (Box e (info ty) { infoShow = Just msg })
  withUtf8 ty $ \t -> withUtf8 msg $ \m -> c_ugly_raise_native ctx rt t m sp key

-- | A new ref for a box, and its identity key: the index of its value's
-- stable name, plus one (zero means "no identity"). The value is forced to
-- weak head normal form first, so a thunk and its value are one identity.
newRef :: Box -> IO (Ptr (), CUIntPtr)
newRef b@(Box x _) = do
  x' <- evaluate x
  sn <- makeStableName x'
  sp <- newStablePtr (Ref b (Named sn))
  atomicModifyIORef' liveRefs (\n -> (n + 1, ()))
  return (castStablePtrToPtr sp, fromIntegral (hashStableName sn) + 1)

-- | Refs not yet released, for the tests: ugly_hs_live_refs.
liveRefs :: IORef Int
liveRefs = unsafePerformIO (newIORef 0)
{-# NOINLINE liveRefs #-}

deref :: Ptr () -> IO Box
deref p = do
  Ref b _ <- deRefStablePtr (castPtrToStablePtr p)
  return b

-- | A box as an owned handle. The core takes the ref either way.
export :: Ptr Ctx -> Ptr Rt -> Box -> Ptr Handle -> IO Status
export ctx rt b out = do
  (sp, key) <- newRef b
  c_ugly_export ctx rt sp key out

setText :: Ptr Ctx -> Ptr Val -> T.Text -> IO Status
setText ctx out t = BU.unsafeUseAsCStringLen (TE.encodeUtf8 t) $ \(p, n) ->
  c_ugly_val_set_string ctx out p (fromIntegral n)

-- | A value as the kind the caller wants, by Haskell's rules: strict, as in
-- Python. An int widens to a double; a box's text (its show) is its string;
-- nothing else converts.
put :: Ptr Ctx -> Ptr Rt -> Kind -> Value -> Ptr Val -> IO Status
put ctx rt want v out = case v of
  VNull | want `elem` [kAny, kNull] -> c_set_null out >> return ok
  VBool b | want `elem` [kAny, kBool] -> c_set_bool out (if b then 1 else 0) >> return ok
  VInt i
    | want `elem` [kAny, kInt] -> c_set_int out i >> return ok
    | want == kDouble -> c_set_double out (fromIntegral i) >> return ok
  VDouble d | want `elem` [kAny, kDouble] -> c_set_double out d >> return ok
  VString t | want `elem` [kAny, kString] -> setText ctx out t
  VBytes b | want `elem` [kAny, kBytes] -> BU.unsafeUseAsCStringLen b $ \(p, n) ->
    c_ugly_val_set_bytes ctx out (castPtr p) (fromIntegral n)
  VBox b@(Box _ inf)
    | want `elem` [kAny, kHandle] -> alloca $ \hp -> do
        st <- export ctx rt b hp
        if st == ok then peek hp >>= c_set_handle out >> return ok else return st
    | want == kString, Just s <- infoShow inf, validText s -> setText ctx out (T.pack s)
  _ -> failWith ctx rt errType "TypeError" ("can't convert " ++ what v ++ " to " ++ kindName want)

what :: Value -> String
what v = case v of
  VNull -> "()"
  VBool _ -> "a Bool"
  VInt _ -> "an integer"
  VDouble _ -> "a Double"
  VString _ -> "a String"
  VBytes _ -> "a ByteString"
  VBox (Box _ inf) -> "a value of type " ++ infoType inf

kindName :: Kind -> String
kindName k = maybe "that kind" id (lookup k
  [ (kNull, "null"), (kBool, "a bool"), (kInt, "an int"), (kDouble, "a double")
  , (kString, "a string"), (kBytes, "bytes"), (kHandle, "a handle") ])


-- The entry points.

hsAbi :: IO CInt
hsAbi = return bridgeAbi

rtNew :: IO (StablePtr Runtime)
rtNew = newIORef () >>= newStablePtr . Runtime

rtDelete :: StablePtr Runtime -> IO ()
rtDelete = freeStablePtr

hsImport :: Ptr Ctx -> Ptr Rt -> CString -> Ptr Val -> IO Status
hsImport ctx rt cname out = guarded ctx rt $ do
  name <- peekUtf8 cname
  case M.lookup name builtins of
    Just m -> put ctx rt kAny (VBox m) out
    Nothing -> failWith ctx rt errNotFound "ModuleNotFound" ("Could not find module " ++ show name)

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

-- | The arguments, decoded. Strings and bytes are copied (they're borrowed);
-- a handle whose home is this runtime is its box.
decodeArgs :: Ptr Ctx -> Ptr Rt -> Ptr Val -> CSize -> IO (Either String [Arg])
decodeArgs ctx rt argv nargs = sequence <$> mapM (decode . fromIntegral) [0 .. fromIntegral nargs - 1 :: Int]   -- nargs is unsigned: 0 - 1 wraps
  where
    decode i = do
      v <- c_arg argv i
      k <- c_kind v
      case () of
        _ | k == kNull -> ok' ANull
          | k == kBool -> ok' . ABool . (/= 0) =<< c_bool v
          | k == kInt -> ok' . AInt =<< c_int v
          | k == kDouble -> ok' . ADouble =<< c_double v
          | k == kString -> do
              b <- bytes v
              return (either (const (Left "a string argument isn't valid UTF-8")) (Right . AString) (TE.decodeUtf8' b))
          | k == kBytes -> ok' . ABytes =<< bytes v
          | otherwise -> do
              h <- c_handle v
              r <- alloca $ \rp -> do
                st <- c_ugly_resolve ctx rt h rp
                if st == ok then peek rp else return nullPtr
              if r == nullPtr then ok' (AForeign h) else ok' . AHome =<< deref r
    ok' = return . Right
    bytes v = do
      p <- c_ptr v
      n <- c_len v
      B.packCStringLen (p, fromIntegral n)

-- | Calls what v is, with these arguments.
callValue :: Ptr Ctx -> Ptr Rt -> String -> Value -> [Arg] -> Kind -> Ptr Val -> IO Status
callValue ctx rt name v args want out = case v of
  VBox (Box _ i) | Just f <- infoCall i -> case f args of
    Left why -> failWith ctx rt errType "TypeError" (name ++ ": " ++ why)
    Right (_, run) -> run >>= \r -> put ctx rt want r out
  _ -> failWith ctx rt errNotCapable "TypeError" (name ++ " isn't a function")

hsCall :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Val -> CSize -> Kind -> Ptr Val -> IO Status
hsCall ctx rt ref argv nargs want out = guarded ctx rt $ do
  b@(Box _ i) <- deref ref
  decoded <- decodeArgs ctx rt argv nargs
  case decoded of
    Left why -> failWith ctx rt errType "TypeError" why
    Right args -> callValue ctx rt ("a value of type " ++ infoType i) (VBox b) args want out

-- | A method call. On a module (anything with members), its function, with
-- no receiver. On any other box, a function from the module that exported
-- its type, with the receiver last: x.m(a, b) is m a b x.
hsInvoke :: Ptr Ctx -> Ptr Rt -> Ptr () -> CString -> Ptr Val -> CSize -> Kind -> Ptr Val -> IO Status
hsInvoke ctx rt ref cname argv nargs want out = guarded ctx rt $ do
  b@(Box _ i) <- deref ref
  name <- peekUtf8 cname
  decoded <- decodeArgs ctx rt argv nargs
  case decoded of
    Left why -> failWith ctx rt errType "TypeError" why
    Right args -> case (infoMembers i >>= (`memberGet` name), infoMethods i >>= ($ name)) of
      (Just f, _) -> callValue ctx rt name f args want out
      (_, Just f) -> callValue ctx rt name f (args ++ [AHome b]) want out
      _ -> failWith ctx rt errNotFound "NotInScope" ("Variable not in scope: " ++ name ++ " (for a value of type " ++ infoType i ++ ")")

-- | Called on any thread, possibly in the middle of a call: freeing a
-- stable pointer is safe anywhere.
hsRelease :: Ptr Rt -> Ptr () -> IO ()
hsRelease _ ref = do
  freeStablePtr (castPtrToStablePtr ref :: StablePtr Ref)
  atomicModifyIORef' liveRefs (\n -> (n - 1, ()))

hsLiveRefs :: IO CInt
hsLiveRefs = fromIntegral <$> readIORef liveRefs

foreign export ccall "ugly_hs_abi" hsAbi :: IO CInt
foreign export ccall "ugly_hs_rt_new" rtNew :: IO (StablePtr Runtime)
foreign export ccall "ugly_hs_rt_delete" rtDelete :: StablePtr Runtime -> IO ()
foreign export ccall "ugly_hs_import" hsImport :: Ptr Ctx -> Ptr Rt -> CString -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_describe" hsDescribe :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Word32 -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_get_member" hsGetMember :: Ptr Ctx -> Ptr Rt -> Ptr () -> CString -> Kind -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_member_names" hsMemberNames :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Handle -> IO Status
foreign export ccall "ugly_hs_length" hsLength :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Int64 -> IO Status
foreign export ccall "ugly_hs_get_index" hsGetIndex :: Ptr Ctx -> Ptr Rt -> Ptr () -> Int64 -> Kind -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_release" hsRelease :: Ptr Rt -> Ptr () -> IO ()
foreign export ccall "ugly_hs_live_refs" hsLiveRefs :: IO CInt
foreign export ccall "ugly_hs_call" hsCall :: Ptr Ctx -> Ptr Rt -> Ptr () -> Ptr Val -> CSize -> Kind -> Ptr Val -> IO Status
foreign export ccall "ugly_hs_invoke" hsInvoke :: Ptr Ctx -> Ptr Rt -> Ptr () -> CString -> Ptr Val -> CSize -> Kind -> Ptr Val -> IO Status
