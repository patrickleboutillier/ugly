{-# LANGUAGE ExistentialQuantification, ScopedTypeVariables #-}

-- | Values to and from C, both ways.
--
-- In from the core (Ugly.Plugin's entry points): refs for boxes, arguments
-- decoded, results put, exceptions out ('guarded').
--
-- Out to the core (a foreign value used from Haskell): calls through the
-- core's API, on the calling thread's ctx. Arguments are put in a
-- borrowed array; a box among them is exported to the current runtime for
-- the call. Results come back as Values: a handle whose home is the
-- current runtime is its box (rule 5), any other a 'Foreign'. A failure
-- is thrown as a 'ForeignError', which owns the core's error; a Haskell
-- exception coming home is thrown as itself (rule 7).
module Ugly.Marshal
  ( -- * Strings
    withUtf8, peekUtf8, validText
    -- * Refs
  , Ref (..), newRef, deref, export, liveRefs, liveForeign
    -- * Entry points
  , guarded, failWith, put, setText, readArg, exceptionValue, rethrowValue
    -- * Calling out
  , foreignCall, foreignInvoke, foreignGet, foreignIndex, foreignLength, foreignNames, foreignType
  , foreignTraits, foreignIter, foreignNext, foreignList, foreignString
  , throwConv
    -- * The binding
  , attach, detach, moduleIn, evalIn
  ) where

import Control.DeepSeq (force)
import Control.Exception
import Control.Concurrent (isCurrentThreadBound, rtsSupportsBoundThreads, runInBoundThread)
import Control.Monad (forM, when)
import qualified Data.ByteString as B
import qualified Data.ByteString.Unsafe as BU
import Data.Char (ord)
import Data.Int
import Data.IORef
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import qualified Data.Text.Encoding.Error as TEE
import Data.Typeable
import Data.Word
import Foreign.C.String
import Foreign.C.Types
import qualified Foreign.Concurrent as FC
import Foreign.ForeignPtr (withForeignPtr)
import Foreign.Marshal.Alloc
import Foreign.Ptr
import Foreign.StablePtr
import Foreign.Storable
import System.IO.Unsafe (unsafeInterleaveIO, unsafePerformIO)
import System.Mem.StableName

import Ugly.Native
import Ugly.Types


-- Strings to and from C are UTF-8, whatever the locale, and neither way
-- throws: these carry error messages and names, on paths that mustn't fail.
-- A lone surrogate (a Char that UTF-8 can't encode: GHC puts them in the
-- FilePath of a file whose name isn't UTF-8, so in an IOException's message)
-- goes out as U+FFFD, and bytes that aren't UTF-8 come in as U+FFFD.
withUtf8 :: String -> (CString -> IO a) -> IO a
withUtf8 s = B.useAsCString (TE.encodeUtf8 (T.pack s))

peekUtf8 :: CString -> IO String
peekUtf8 p = T.unpack . TE.decodeUtf8With TEE.lenientDecode <$> B.packCString p

-- | Text that is valid Unicode. A Haskell 'Char' can be a lone surrogate,
-- which UTF-8 can't encode (and Data.Text would silently replace).
validText :: String -> Bool
validText = all (\c -> ord c < 0xD800 || ord c > 0xDFFF)

setText :: Ptr Ctx -> Ptr Val -> T.Text -> IO Status
setText ctx out t = BU.unsafeUseAsCStringLen (TE.encodeUtf8 t) $ \(p, n) ->
  c_ugly_val_set_string ctx out p (fromIntegral n)


-- Refs.

-- | What C holds for a value: its box, and the stable name of its identity.
data Ref = Ref Box Named
data Named = forall a. Named (StableName a)

-- | A new ref for a box, and its identity key: the index of its value's
-- stable name, plus one (zero means "no identity"). The value is forced to
-- weak head normal form first, so a thunk and its value are one identity.
--
-- An exception's identity is the exception inside its SomeException: since
-- GHC 9.10, throwIO wraps it again (with its context) every time it's
-- thrown, so the wrapper of an exception rethrown at home is a new object,
-- but what it wraps isn't (rule 7).
newRef :: Box -> IO (Ptr (), CUIntPtr)
newRef b@(Box x _) = do
  sn <- case cast x of
    Just (SomeException inner) -> evaluate inner >>= fmap Named . makeStableName
    Nothing -> evaluate x >>= fmap Named . makeStableName
  sp <- newStablePtr (Ref b sn)
  count liveRefs 1
  return (castStablePtrToPtr sp, fromIntegral (case sn of Named n -> hashStableName n) + 1)

-- | Refs not yet released, for the tests: ugly_hs_live_refs.
liveRefs :: IORef Int
liveRefs = unsafePerformIO (newIORef 0)
{-# NOINLINE liveRefs #-}

-- | Foreign values and taken errors Haskell still holds, for the tests:
-- ugly_hs_live_foreign.
liveForeign :: IORef Int
liveForeign = unsafePerformIO (newIORef 0)
{-# NOINLINE liveForeign #-}

count :: IORef Int -> Int -> IO ()
count r d = atomicModifyIORef' r (\n -> (n + d, ()))

deref :: Ptr () -> IO Box
deref p = do
  Ref b _ <- deRefStablePtr (castPtrToStablePtr p)
  return b

-- | A box as an owned handle. The core takes the ref either way.
export :: Ptr Ctx -> Ptr Rt -> Box -> Ptr Handle -> IO Status
export ctx rt b out = do
  (sp, key) <- newRef b
  c_ugly_export ctx rt sp key out

-- | An owned handle as a Foreign, released when Haskell drops it.
newForeign :: Handle -> IO Foreign
newForeign h = do
  keep <- FC.newForeignPtr nullPtr (c_ugly_release nullPtr h >> count liveForeign (-1))
  count liveForeign 1
  return (Foreign h keep)

-- | Runs an action with a foreign value's handle, keeping it alive meanwhile.
withHandle :: Foreign -> (Handle -> IO a) -> IO a
withHandle (Foreign h keep) act = withForeignPtr keep (\_ -> act h)


-- Entry points.

-- | A failure that isn't an exception: a status, and a type in Haskell's terms.
failWith :: Ptr Ctx -> Ptr Rt -> Status -> String -> String -> IO Status
failWith ctx rt st ty msg =
  withUtf8 ty $ \t -> withUtf8 msg $ \m -> c_ugly_fail ctx rt st t m

-- | Runs an entry point for runtime rt (the current one, while it runs),
-- turning any exception into an error.
guarded :: Ptr Ctx -> Ptr Rt -> IO Status -> IO Status
guarded ctx rt act = do
  before <- c_enter rt
  (act `catch` raise ctx rt) `finally` c_leave before

-- | An exception escaping to the core. A foreign error goes on unchanged
-- (rule 7). Any other is UGLY_ERR_RAISED: its type's name, its message, and
-- itself as the payload (a box).
--
-- It must not throw itself: an exception out of a foreign export ends the
-- process. One that can't even be described (its value is itself an error,
-- forced by newRef) is raised without a payload.
raise :: Ptr Ctx -> Ptr Rt -> SomeException -> IO Status
raise ctx rt e = described `catch` \(_ :: SomeException) ->
  withCString "SomeException" $ \t -> withCString "an exception that can't be evaluated" $ \m ->
    c_ugly_raise_native ctx rt t m nullPtr 0
  where
    described = case fromException e of
      Just fe -> restore ctx rt fe
      Nothing -> do
        (b, ty, msg) <- exceptionBox e
        (sp, key) <- newRef b
        withUtf8 ty $ \t -> withUtf8 msg $ \m -> c_ugly_raise_native ctx rt t m sp key

-- | An exception as the core sees it: a box of the SomeException itself
-- (so that it comes home as itself), its type's name and its message. One
-- declared under UglyException has its own type's members.
exceptionBox :: SomeException -> IO (Box, String, String)
exceptionBox e = do
  let (ty, own) = case fromException e of
        Just (UglyException inner i) -> (show (typeOf inner), i)
        Nothing -> (case e of SomeException inner -> show (typeOf inner), Nothing)
  msg <- evaluate (force (displayException e)) `catch` \(_ :: SomeException) -> return ty
  let base = maybe (info ty) id own
  return (Box e base { infoType = ty, infoShow = Just (return msg) }, ty, msg)

-- | Raises a value again: an exception that came home (a box of a
-- SomeException) as itself; a foreign value as an error whose payload is
-- that value, unchanged (rule 7 at its home).
rethrowValue :: Value -> IO a
rethrowValue v = case v of
  VBox (Box x _) | Just (e :: SomeException) <- cast x -> throwIO e
  VForeign f -> do
    ty <- foreignType f `catch` \(_ :: SomeException) -> return "?"
    owned <- newIORef nullPtr
    throwIO (ForeignError errRaised "haskell" ty "a foreign value, raised again" (Just v) owned)
  _ -> throwIO (ErrorCall "ugly: only an exception or a foreign value can be raised again")

-- | An exception as a value: what a Haskell catch hands back to a caller.
exceptionValue :: SomeException -> IO Value
exceptionValue e = (\(b, _, _) -> VBox b) <$> exceptionBox e

-- | A foreign error leaving Haskell: the core's error, as it was taken. If
-- it left once already (Haskell kept the exception and threw it again),
-- it's raised again from what Haskell copied: its payload, or its status.
restore :: Ptr Ctx -> Ptr Rt -> ForeignError -> IO Status
restore ctx rt fe = do
  err <- atomicModifyIORef' (errorOwned fe) (\p -> (nullPtr, p))
  if err /= nullPtr
    then count liveForeign (-1) >> c_ugly_restore_error ctx err
    else do
      payload <- maybe (return Nothing) (handleOf ctx rt) (errorPayload fe)
      case payload of
        Just h -> withUtf8 (errorType fe) $ \t -> withUtf8 (errorMessage fe) $ \m ->
          c_ugly_raise_handle ctx rt t m h
        Nothing -> failWith ctx rt (errorStatus fe) (errorType fe) (errorMessage fe)
  where
    handleOf c r v = case v of
      VBox b -> alloca $ \hp -> do
        st <- export c r b hp
        if st == ok then Just <$> peek hp else return Nothing
      VForeign f -> withHandle f $ \h -> alloca $ \hp -> do
        st <- c_ugly_dup c h hp
        if st == ok then Just <$> peek hp else return Nothing
      _ -> return Nothing

-- | A value as the kind the caller wants, by Haskell's rules: strict, as in
-- Python. An int widens to a double; a box's text (its show) is its string;
-- nothing else converts. A foreign value is its own handle (rule 5).
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
    | want == kString, Just text <- infoShow inf -> do
        s <- text
        if validText s then setText ctx out (T.pack s) else failWith ctx rt errType "TypeError" "its text isn't valid Unicode"
  VForeign f
    | want `elem` [kAny, kHandle] -> withHandle f $ \h -> alloca $ \hp -> do
        st <- c_ugly_dup ctx h hp
        when (st == ok) (peek hp >>= c_set_handle out)
        return st
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
  VForeign _ -> "a foreign value"

kindName :: Kind -> String
kindName k = maybe "that kind" id (lookup k
  [ (kNull, "null"), (kBool, "a bool"), (kInt, "an int"), (kDouble, "a double")
  , (kString, "a string"), (kBytes, "bytes"), (kHandle, "a handle") ])

-- | An argument, decoded (it's borrowed: strings and bytes are copied). A
-- handle whose home is rt is its box; any other is taken (duplicated) only
-- if the call uses it.
readArg :: Ptr Ctx -> Ptr Rt -> Ptr Val -> IO (Either String Arg)
readArg ctx rt v = do
  k <- c_kind v
  case () of
    _ | k == kNull -> ok' ANull
      | k == kBool -> ok' . ABool . (/= 0) =<< c_bool v
      | k == kInt -> ok' . AInt =<< c_int v
      | k == kDouble -> ok' . ADouble =<< c_double v
      | k == kString -> do
          b <- bytesOf v
          return (either (const (Left "a string argument isn't valid UTF-8")) (Right . AString) (TE.decodeUtf8' b))
      | k == kBytes -> ok' . ABytes =<< bytesOf v
      | otherwise -> do
          h <- c_handle v
          r <- resolve ctx rt h
          case r of
            Just b -> ok' (AHome b)
            Nothing -> ok' (AForeign (dup ctx h))
  where
    ok' = return . Right

-- | The box of a handle whose home is rt, if it is.
resolve :: Ptr Ctx -> Ptr Rt -> Handle -> IO (Maybe Box)
resolve ctx rt h
  | rt == nullPtr = return Nothing
  | otherwise = do
      r <- alloca $ \rp -> do
        poke rp nullPtr
        st <- c_ugly_resolve ctx rt h rp
        if st == ok then peek rp else return nullPtr
      if r == nullPtr then return Nothing else Just <$> deref r

dup :: Ptr Ctx -> Handle -> IO Foreign
dup ctx h = alloca $ \hp -> do
  st <- c_ugly_dup ctx h hp
  if st /= ok then failed ctx st else peek hp >>= newForeign

bytesOf :: Ptr Val -> IO B.ByteString
bytesOf v = do
  p <- c_ptr v
  n <- c_len v
  B.packCStringLen (p, fromIntegral n)


-- Calling out.

-- | A result, owned: a primitive (strings and bytes copied, then released),
-- or a handle, taken: its box if its home is the current runtime, else a
-- Foreign.
readResult :: Ptr Ctx -> Ptr Val -> IO Value
readResult ctx v = do
  k <- c_kind v
  case () of
    _ | k == kNull -> return VNull
      | k == kBool -> VBool . (/= 0) <$> c_bool v
      | k == kInt -> VInt <$> c_int v
      | k == kDouble -> VDouble <$> c_double v
      | k == kString -> do
          b <- bytesOf v
          c_ugly_val_release ctx v
          return (VString (TE.decodeUtf8With TEE.lenientDecode b))
      | k == kBytes -> do
          b <- bytesOf v
          c_ugly_val_release ctx v
          return (VBytes b)
      | otherwise -> c_handle v >>= handleValue ctx

-- | An owned handle, taken.
handleValue :: Ptr Ctx -> Handle -> IO Value
handleValue ctx h = do
  rt <- c_current_rt
  r <- resolve ctx rt h
  case r of
    Just b -> c_ugly_release ctx h >> return (VBox b)
    Nothing -> VForeign <$> newForeign h

-- | The core's error after a failed operation, thrown: a Haskell exception
-- coming home as itself, anything else as a ForeignError that owns it.
failed :: Ptr Ctx -> Status -> IO a
failed ctx st = do
  err <- c_ugly_take_error ctx
  if err == nullPtr
    then newIORef nullPtr >>= throwIO . ForeignError st "core" "Error" "the operation failed" Nothing
    else do
      payload <- alloca $ \hp -> do
        poke hp noHandle
        _ <- c_ugly_error_payload ctx err hp
        h <- peek hp
        if h == noHandle then return Nothing else Just <$> handleValue ctx h
      case payload of
        Just (VBox (Box x _)) | Just (e :: SomeException) <- cast x -> do
          c_ugly_error_free err
          throwIO e
        _ -> do
          status <- c_ugly_error_status err
          lang <- c_ugly_error_language err >>= peekUtf8
          ty <- c_ugly_error_type err >>= peekUtf8
          msg <- c_ugly_error_message err >>= peekUtf8
          owned <- newIORef err
          _ <- mkWeakIORef owned (freeTaken owned)
          count liveForeign 1
          throwIO (ForeignError status lang ty msg payload owned)
  where
    freeTaken owned = do
      err <- atomicModifyIORef' owned (\p -> (nullPtr, p))
      when (err /= nullPtr) (count liveForeign (-1) >> c_ugly_error_free err)

-- | This thread's ctx.
currentCtx :: IO (Ptr Ctx)
currentCtx = do
  ctx <- c_ctx
  when (ctx == nullPtr) (throwIO (ErrorCall "ugly: no ctx for this thread"))
  outside <- c_outside
  when (outside /= 0) c_drain     -- the program's own code: run the releases queued for its runtime
  return ctx

-- | Runs a call out on one OS thread from start to end. The ctx is the OS
-- thread's (currentCtx), and the calls made with it are safe foreign calls:
-- an unbound Haskell thread (forkIO) can be moved to another OS thread
-- between two of them, and would then use a ctx that isn't its thread's. A
-- bound thread (main, forkOS, any entry from another language) stays put;
-- an unbound one makes its call out on a bound thread of its own.
bound :: IO a -> IO a
bound act = do
  here <- isCurrentThreadBound
  if here || not rtsSupportsBoundThreads then act else runInBoundThread act

-- | An operation on a foreign value, with a result.
operation :: Foreign -> (Ptr Ctx -> Handle -> Ptr Val -> IO Status) -> IO Value
operation f act = bound $ do
  ctx <- currentCtx
  size <- fromIntegral <$> c_val_size
  withHandle f $ \h -> allocaBytes size $ \out -> do
    c_set_null out
    st <- act ctx h out
    if st /= ok then failed ctx st else readResult ctx out

-- | Arguments as an array of ugly_val, borrowed for the call. A box is
-- exported to the current runtime for the call, then released.
withArgs :: Ptr Ctx -> [Value] -> (Ptr Val -> CSize -> IO a) -> IO a
withArgs ctx vs k = do
  size <- fromIntegral <$> c_val_size
  allocaBytes (max 1 (n * size)) $ \arr -> go arr 0 vs
  where
    n = length vs
    go arr _ [] = k arr (fromIntegral n)
    go arr i (v : rest) = do
      p <- c_val_at arr i
      let next = go arr (i + 1) rest
      case v of
        VNull -> c_set_null p >> next
        VBool b -> c_set_bool p (if b then 1 else 0) >> next
        VInt x -> c_set_int p x >> next
        VDouble d -> c_set_double p d >> next
        VString t -> BU.unsafeUseAsCStringLen (TE.encodeUtf8 t) $ \(s, len) ->
          c_set_string p s (fromIntegral len) >> next
        VBytes b -> BU.unsafeUseAsCStringLen b $ \(s, len) ->
          c_set_bytes p (castPtr s) (fromIntegral len) >> next
        VForeign f -> withHandle f $ \h -> c_set_handle p h >> next
        VBox b -> do
          rt <- c_current_rt
          when (rt == nullPtr) (throwIO (ErrorCall "ugly: a Haskell value can't be passed from outside a runtime"))
          h <- alloca $ \hp -> do
            st <- export ctx rt b hp
            if st /= ok then failed ctx st else peek hp
          (c_set_handle p h >> next) `finally` c_ugly_release ctx h

foreignCall :: Foreign -> [Value] -> IO Value
foreignCall f args = bound $ do
  ctx <- currentCtx
  withArgs ctx args $ \argv n -> operation f $ \c h out -> c_ugly_call c h argv n kAny out

foreignInvoke :: Foreign -> String -> [Value] -> IO Value
foreignInvoke f name args = bound $ do
  ctx <- currentCtx
  withUtf8 name $ \m -> withArgs ctx args $ \argv n ->
    operation f $ \c h out -> c_ugly_invoke c h m argv n kAny out

foreignGet :: Foreign -> String -> IO Value
foreignGet f name = withUtf8 name $ \m -> operation f $ \c h out -> c_ugly_get_member c h m kAny out

foreignIndex :: Foreign -> Int -> IO Value
foreignIndex f i = operation f $ \c h out -> c_ugly_get_index c h (fromIntegral i) kAny out

foreignLength :: Foreign -> IO Int
foreignLength f = bound $ do
  ctx <- currentCtx
  withHandle f $ \h -> alloca $ \np -> do
    st <- c_ugly_length ctx h np
    if st /= ok then failed ctx st else fromIntegral <$> (peek np :: IO Int64)

-- | Its members' names.
foreignNames :: Foreign -> IO [String]
foreignNames f = bound $ do
  ctx <- currentCtx
  names <- withHandle f $ \h -> alloca $ \lp -> do
    st <- c_ugly_member_names ctx h lp
    if st /= ok then failed ctx st else peek lp >>= newForeign
  n <- foreignLength names
  forM [0 .. n - 1] $ \i -> do
    v <- foreignIndex names i
    case v of
      VString t -> return (T.unpack t)
      _ -> throwIO (ErrorCall "ugly: a member name isn't a string")

-- | Its type's name, in its own language's terms.
foreignType :: Foreign -> IO String
foreignType f = do
  v <- operation f $ \c h out -> alloca $ \(traits :: Ptr Word32) -> c_ugly_describe c h traits out
  case v of
    VString t -> return (T.unpack t)
    _ -> return "?"

-- | Its traits (describe).
foreignTraits :: Foreign -> IO Word32
foreignTraits f = bound $ do
  ctx <- currentCtx
  size <- fromIntegral <$> c_val_size
  withHandle f $ \h -> alloca $ \tp -> allocaBytes size $ \out -> do
    c_set_null out
    st <- c_ugly_describe ctx h tp out
    if st /= ok then failed ctx st else c_ugly_val_release ctx out >> peek tp

-- | Its own iterator (ugly_iter): on an iterator, itself.
foreignIter :: Foreign -> IO Foreign
foreignIter f = bound $ do
  ctx <- currentCtx
  withHandle f $ \h -> alloca $ \hp -> do
    st <- c_ugly_iter ctx h hp
    if st /= ok then failed ctx st else peek hp >>= newForeign

-- | One step of a foreign iterator (ugly_next, one value): Nothing at the end.
foreignNext :: Foreign -> IO (Maybe Value)
foreignNext f = bound $ do
  ctx <- currentCtx
  size <- fromIntegral <$> c_val_size
  withHandle f $ \h -> allocaBytes size $ \out -> alloca $ \gp -> do
    c_set_null out
    poke gp 0
    st <- c_ugly_next ctx h kAny out 1 gp
    got <- peek gp
    if st /= ok then failed ctx st
      else if got == 0 then return Nothing else Just <$> readResult ctx out

-- | A foreign iterable as a lazy list, like hGetContents: each cell, when
-- forced, is one step of its own iteration, and an error is thrown from
-- there.
foreignList :: Foreign -> IO [Value]
foreignList f = foreignIter f >>= steps
  where steps it = unsafeInterleaveIO (foreignNext it >>= maybe (return []) (\v -> (v :) <$> steps it))

-- | Its own language's string for it (ugly_to_string; bytes decoded leniently).
foreignString :: Foreign -> IO String
foreignString f = do
  v <- operation f $ \c h out -> c_ugly_to_string c h out
  case v of
    VString t -> return (T.unpack t)
    VBytes b -> return (T.unpack (TE.decodeUtf8With TEE.lenientDecode b))
    _ -> return "?"

-- | A refused conversion, thrown (Control.Exception's TypeError).
throwConv :: String -> IO a
throwConv = throwIO . TypeError



-- The binding: a Haskell program as the host.

-- | Registers the plugin and attaches to the program's runtime (once;
-- nested uses count). A failure is thrown.
attach :: IO ()
attach = do
  st <- c_binding_attach
  when (st /= ok) $ do
    why <- c_binding_error >>= peekUtf8
    throwIO (ErrorCall ("ugly: can't attach: " ++ why))

detach :: IO ()
detach = c_binding_detach

-- | This thread's runtime of a language (ugly_rt_get: shared, made on first use).
runtimeOf :: Ptr Ctx -> String -> IO (Ptr Rt)
runtimeOf ctx lang = withUtf8 lang $ \l -> alloca $ \rp -> do
  st <- c_ugly_rt_get ctx l rp
  if st /= ok then failed ctx st else peek rp

-- | A module of a language, by name: ugly.module(lang, name).
moduleIn :: String -> String -> IO Value
moduleIn lang name = bound $ do
  ctx <- currentCtx
  rt <- runtimeOf ctx lang
  h <- withUtf8 name $ \n -> alloca $ \hp -> do
    st <- c_ugly_rt_import ctx rt n hp
    if st /= ok then failed ctx st else peek hp
  handleValue ctx h

-- | Code in a language: ugly.eval(lang, code).
evalIn :: String -> String -> IO Value
evalIn lang code = bound $ do
  ctx <- currentCtx
  rt <- runtimeOf ctx lang
  size <- fromIntegral <$> c_val_size
  BU.unsafeUseAsCStringLen (TE.encodeUtf8 (T.pack code)) $ \(p, n) -> allocaBytes size $ \out -> do
    c_set_null out
    st <- c_ugly_rt_eval ctx rt p (fromIntegral n) kAny out
    if st /= ok then failed ctx st else readResult ctx out
