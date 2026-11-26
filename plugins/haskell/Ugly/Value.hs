{-# LANGUAGE ExistentialQuantification, FlexibleContexts, FlexibleInstances, ScopedTypeVariables, UndecidableInstances #-}
-- MonoLocalBinds: without it, generalizing a local 'call' settles its
-- Result (a -> b) constraint with the overlappable instance, too early.
{-# LANGUAGE MonoLocalBinds #-}

-- | Haskell values as the protocol sees them, both ways.
--
-- Out: a 'Value' is a primitive, which crosses by value, or a 'Box': a
-- Haskell value with the operations the protocol may ask of it, closed over
-- it where its type was static. A box needs no class dictionary at run time,
-- and no reflection: describe, show, members, elements and calls are already
-- closures. 'ToUgly' decides which.
--
-- In: an 'Arg' is what a caller passed, decoded from C. 'FromUgly' converts
-- it to a Haskell type the way a literal of its kind would convert (an int
-- to any integer type it fits, or to a Double; a string to String or Text),
-- or takes a Haskell value coming home as itself. Each conversion has a
-- cost, so that a call to an overloaded (polymorphic) function picks the
-- instantiation its arguments fit best.
--
-- The choices are doc/haskell-bridge.md's "Decided": integers out of range
-- are refused, never wrapped; Maybe is flattened unless Just x is itself
-- null.
module Ugly.Value
  ( Value (..), Box (..), Info (..), Members (..), Elems (..), Arg (..), Conv
  , ToUgly (..), FromUgly (..), Result (..)
  , info, boxShow, boxOpaque, boxWith, moduleBox, overloaded
  , typeName, validText, describeArg, home
  ) where

import Control.Exception (evaluate)
import qualified Data.ByteString as B
import Data.Char (ord)
import Data.Int
import Data.List (intercalate, sortOn)
import qualified Data.Map as M
import qualified Data.Text as T
import Data.Typeable
import Data.Word
import Numeric.Natural (Natural)

-- | What crosses: a primitive, by value, or a box, by handle.
data Value
  = VNull
  | VBool !Bool
  | VInt !Int64
  | VDouble !Double
  | VString !T.Text
  | VBytes !B.ByteString
  | VBox !Box

-- | A Haskell value, its identity (the value itself), and what can be asked
-- of it. 'Typeable', so that it can be cast back when it comes home.
data Box = forall a. Typeable a => Box a Info

data Info = Info
  { infoType :: String              -- ^ describe's type name: "Note", "[Int]", "module"
  , infoShow :: Maybe String        -- ^ its text, for want = string (lazy: forced when asked)
  , infoMembers :: Maybe Members    -- ^ MEMBERS
  , infoElems :: Maybe Elems        -- ^ ELEMENTS
  , infoCall :: Maybe ([Arg] -> Conv (IO Value))   -- ^ CALLABLE: match the arguments, then run
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

-- | An argument, as passed: a primitive, or a handle. A handle whose home is
-- this runtime arrives as its box.
data Arg
  = ANull
  | ABool Bool
  | AInt Int64
  | ADouble Double
  | AString T.Text
  | ABytes B.ByteString
  | AHome Box
  | AForeign Word64    -- ^ a borrowed handle from another runtime

-- | A conversion: why not, or its cost (0 is exact) and how. The "how" runs
-- only for the overload that's chosen.
type Conv a = Either String (Int, a)

-- | How a Haskell type crosses out. Every type is 'Typeable', for its name.
class Typeable a => ToUgly a where
  toUgly :: a -> Value
  -- | Its text as 'show' would give it, if the type has one: what a box of
  -- it, or a list of them, shows as.
  textOf :: a -> Maybe String
  textOf _ = Nothing

-- | How a Haskell type is taken from an argument. The default takes only a
-- Haskell value of exactly this type, coming home.
class Typeable a => FromUgly a where
  fromUgly :: Arg -> Conv (IO a)
  fromUgly = home

typeName :: forall a. Typeable a => a -> String
typeName _ = show (typeRep (Proxy :: Proxy a))

typeNameP :: forall a. Typeable a => Proxy a -> String
typeNameP = show . typeRep

-- | A Haskell value of exactly type a, coming home.
home :: forall a. Typeable a => Arg -> Conv (IO a)
home (AHome (Box x _)) | Just y <- cast x = Right (0, return y)
home arg = Left (describeArg arg ++ " isn't " ++ article (typeNameP (Proxy :: Proxy a)))

-- | "a Note", "an Int".
article :: String -> String
article t = (if take 1 t `elem` map (: []) "AEIOUaeiou" then "an " else "a ") ++ t

describeArg :: Arg -> String
describeArg a = case a of
  ANull -> "null"
  ABool _ -> "a bool"
  AInt i -> "the int " ++ show i
  ADouble d -> "the double " ++ show d
  AString _ -> "a string"
  ABytes _ -> "bytes"
  AHome (Box _ i) -> article (infoType i)
  AForeign _ -> "a foreign value"

-- | A value of a type with 'Show': a box with a type name and a text.
boxShow :: (Typeable a, Show a) => a -> Value
boxShow x = VBox (Box x (info (typeName x)) { infoShow = Just (show x) })

-- | A value with nothing to ask but its type.
boxOpaque :: Typeable a => a -> Value
boxOpaque x = VBox (Box x (info (typeName x)))

-- | A value with a text (if any) and methods: what uglyModule gives the types it exports.
boxWith :: Typeable a => Maybe String -> (String -> Maybe Value) -> a -> Value
boxWith txt methods x = VBox (Box x (info (typeName x)) { infoShow = txt, infoMethods = Just methods })

-- | A module: its exports as members. The list is its identity, so the same
-- module is the same handle while one is live.
moduleBox :: String -> [(String, Value)] -> Box
moduleBox name entries = Box entries (info "module")
  { infoShow = Just ("module " ++ name)
  , infoMembers = Just (Members (map fst entries) (`lookup` entries))
  }

-- | A polymorphic function, as its instantiations: a call picks the one its
-- arguments convert to most cheaply. A tie is ambiguous, and refused.
overloaded :: String -> [Value] -> Value
overloaded ty candidates = VBox (Box candidates (info ty) { infoCall = Just call })
  where
    call args =
      let tried = [ (infoType i, f args) | VBox (Box _ i) <- candidates, Just f <- [infoCall i] ]
          fits = sortOn (fst . snd) [ (t, r) | (t, Right r) <- tried ]
      in case fits of
        [] -> Left ("no instance of " ++ ty ++ " takes these arguments:"
                    ++ concat [ "\n  " ++ t ++ ": " ++ why | (t, Left why) <- tried ])
        (_, best@(c, _)) : rest
          | all ((/= c) . fst . snd) rest -> Right best
          | otherwise -> Left ("ambiguous: these instances of " ++ ty ++ " take these arguments equally well:"
                               ++ concat [ "\n  " ++ t | (t, (c', _)) <- fits, c' == c ])

-- | Text that is valid Unicode. A Haskell 'Char' can be a lone surrogate,
-- which UTF-8 can't encode (and Data.Text would silently replace).
validText :: String -> Bool
validText = all (\c -> ord c < 0xD800 || ord c > 0xDFFF)


-- Out.

-- | An integer that may not fit in an int64: a box of its digits if it doesn't.
integral :: (Integral a, Show a, Typeable a) => a -> Value
integral n
  | i >= toInteger (minBound :: Int64) && i <= toInteger (maxBound :: Int64) = VInt (fromInteger i)
  | otherwise = boxShow n
  where i = toInteger n

shown :: Show a => a -> Maybe String
shown = Just . show

instance ToUgly () where
  toUgly () = VNull
  textOf = shown
instance ToUgly Bool where
  toUgly = VBool
  textOf = shown
instance ToUgly Int where
  toUgly = VInt . fromIntegral
  textOf = shown
instance ToUgly Int8 where
  toUgly = VInt . fromIntegral
  textOf = shown
instance ToUgly Int16 where
  toUgly = VInt . fromIntegral
  textOf = shown
instance ToUgly Int32 where
  toUgly = VInt . fromIntegral
  textOf = shown
instance ToUgly Int64 where
  toUgly = VInt
  textOf = shown
instance ToUgly Word8 where
  toUgly = VInt . fromIntegral
  textOf = shown
instance ToUgly Word16 where
  toUgly = VInt . fromIntegral
  textOf = shown
instance ToUgly Word32 where
  toUgly = VInt . fromIntegral
  textOf = shown
instance ToUgly Word where
  toUgly = integral
  textOf = shown
instance ToUgly Word64 where
  toUgly = integral
  textOf = shown
instance ToUgly Integer where
  toUgly = integral
  textOf = shown
instance ToUgly Natural where
  toUgly = integral
  textOf = shown
instance ToUgly Double where
  toUgly = VDouble
  textOf = shown
instance ToUgly Float where
  toUgly = VDouble . realToFrac
  textOf = shown

instance ToUgly Char where
  toUgly c
    | validText [c] = VString (T.singleton c)
    | otherwise = boxOpaque c
  textOf = shown

-- | A String is a string, unless it isn't valid Unicode: then it's a box with no text.
instance {-# OVERLAPPING #-} ToUgly [Char] where
  toUgly s
    | validText s = VString (T.pack s)
    | otherwise = boxOpaque s
  textOf = shown

instance ToUgly T.Text where
  toUgly = VString
  textOf = shown

instance ToUgly B.ByteString where
  toUgly = VBytes
  textOf = shown

-- | Lists are read-only elements. Reading element i walks i cells.
instance {-# OVERLAPPABLE #-} ToUgly a => ToUgly [a] where
  toUgly xs = VBox (Box xs (info (typeName xs)) { infoShow = textOf xs, infoElems = Just elems })
    where
      elems = Elems (length xs) at
      at i
        | i < 0 = Nothing
        | otherwise = case drop i xs of
            y : _ -> Just (toUgly y)
            [] -> Nothing
  textOf xs = (\ts -> "[" ++ intercalate "," ts ++ "]") <$> mapM textOf xs

-- | Nothing is null, and Just x is x, unless x is itself null (Just Nothing,
-- Just ()): then it's a box, one element long, so nothing is lost.
instance ToUgly a => ToUgly (Maybe a) where
  toUgly Nothing = VNull
  toUgly m@(Just x) = case toUgly x of
    VNull -> VBox (Box m (info (typeName m)) { infoShow = textOf m, infoElems = Just (Elems 1 one) })
    v -> v
    where one i = if i == 0 then Just VNull else Nothing
  textOf Nothing = Just "Nothing"
  textOf (Just x) = ("Just " ++) . paren <$> textOf x
    where paren t = if ' ' `elem` t || take 1 t == "-" then "(" ++ t ++ ")" else t

-- | Tuples are read-only elements.
instance (ToUgly a, ToUgly b) => ToUgly (a, b) where
  toUgly p@(a, b) = VBox (Box p (info (typeName p)) { infoShow = textOf p, infoElems = Just (Elems 2 at) })
    where at i = lookup i [(0, toUgly a), (1, toUgly b)]
  textOf (a, b) = (\x y -> "(" ++ x ++ "," ++ y ++ ")") <$> textOf a <*> textOf b

instance (ToUgly a, ToUgly b, ToUgly c) => ToUgly (a, b, c) where
  toUgly t@(a, b, c) = VBox (Box t (info (typeName t)) { infoShow = textOf t, infoElems = Just (Elems 3 at) })
    where at i = lookup i [(0, toUgly a), (1, toUgly b), (2, toUgly c)]
  textOf (a, b, c) = (\x y z -> "(" ++ x ++ "," ++ y ++ "," ++ z ++ ")") <$> textOf a <*> textOf b <*> textOf c

-- | A map with string keys: its entries are members (read-only).
instance ToUgly v => ToUgly (M.Map String v) where
  toUgly m = VBox (Box m (info (typeName m)) { infoMembers = Just members })
    where members = Members (M.keys m) (\k -> toUgly <$> M.lookup k m)

instance ToUgly Value where
  toUgly = id

-- | A function is callable: arguments are matched against its parameter
-- types, then applied. Fewer than its arity give a partial application.
instance (Typeable a, Typeable b, FromUgly a, Result b) => ToUgly (a -> b) where
  toUgly f = VBox (Box f (info (typeName f)) { infoCall = Just call })
    where
      call [] = Left "expects an argument"
      call args = fmap (fmap ($ f)) (matchArgs args :: Conv ((a -> b) -> IO Value))

-- | An action is callable with no arguments: the call runs it.
instance ToUgly a => ToUgly (IO a) where
  toUgly io = VBox (Box io (info (typeName io)) { infoCall = Just call })
    where call args = fmap (fmap ($ io)) (matchArgs args :: Conv (IO a -> IO Value))


-- Calls.

-- | What a call does with its arguments, by the callee's type alone: so
-- that matching (for overloads) never evaluates anything.
class Typeable r => Result r where
  matchArgs :: [Arg] -> Conv (r -> IO Value)

-- | A function takes the next argument; with none left, it's a partial application.
instance {-# OVERLAPPING #-} (Typeable a, Typeable b, FromUgly a, Result b) => Result (a -> b) where
  matchArgs [] = Right (0, return . toUgly)
  matchArgs (x : xs) = do
    (c1, getA) <- fromUgly x
    (c2, runB) <- matchArgs xs :: Conv (b -> IO Value)
    return (c1 + c2, \f -> getA >>= runB . f)

-- | An action is run when the arguments are used up.
instance {-# OVERLAPPING #-} (Typeable a, ToUgly a) => Result (IO a) where
  matchArgs [] = Right (0, fmap toUgly)
  matchArgs _ = Left "too many arguments"

-- | Anything else is the result, evaluated (to weak head normal form) when
-- the arguments are used up.
instance {-# OVERLAPPABLE #-} (Typeable r, ToUgly r) => Result r where
  matchArgs [] = Right (0, evaluate . toUgly)
  matchArgs _ = Left "too many arguments"


-- In: a foreign value converts as a literal of its kind would.

-- | An int, into an integer type: if it fits, at that cost.
intoIntegral :: forall a. (Integral a, Bounded a, Typeable a) => Int -> Arg -> Conv (IO a)
intoIntegral cost (AInt i)
  | toInteger i >= toInteger (minBound :: a) && toInteger i <= toInteger (maxBound :: a) = Right (cost, return (fromIntegral i))
  | otherwise = Left (show i ++ " doesn't fit in " ++ typeNameP (Proxy :: Proxy a))
intoIntegral _ arg = home arg

-- Costs: 0 for a kind's own type (int: Int; double: Double; string:
-- String), 1 for another that holds every value (Int64, Integer, Text), 2 for a
-- narrower one the value fits, 3 for a literal of another kind (an int as a
-- Double), 4 for Value, which takes anything.
instance FromUgly Int where fromUgly = intoIntegral 0
instance FromUgly Int64 where fromUgly = intoIntegral 1
instance FromUgly Int8 where fromUgly = intoIntegral 2
instance FromUgly Int16 where fromUgly = intoIntegral 2
instance FromUgly Int32 where fromUgly = intoIntegral 2
instance FromUgly Word where fromUgly = intoIntegral 2
instance FromUgly Word8 where fromUgly = intoIntegral 2
instance FromUgly Word16 where fromUgly = intoIntegral 2
instance FromUgly Word32 where fromUgly = intoIntegral 2
instance FromUgly Word64 where fromUgly = intoIntegral 2

instance FromUgly Integer where
  fromUgly (AInt i) = Right (1, return (toInteger i))
  fromUgly arg = home arg

instance FromUgly Natural where
  fromUgly (AInt i)
    | i >= 0 = Right (2, return (fromIntegral i))
    | otherwise = Left (show i ++ " isn't a Natural")
  fromUgly arg = home arg

instance FromUgly Double where
  fromUgly (ADouble d) = Right (0, return d)
  fromUgly (AInt i) = Right (3, return (fromIntegral i))
  fromUgly arg = home arg

instance FromUgly Float where
  fromUgly (ADouble d) = Right (2, return (realToFrac d))
  fromUgly (AInt i) = Right (3, return (fromIntegral i))
  fromUgly arg = home arg

instance FromUgly Bool where
  fromUgly (ABool b) = Right (0, return b)
  fromUgly arg = home arg

instance FromUgly () where
  fromUgly ANull = Right (0, return ())
  fromUgly arg = home arg

instance FromUgly Char where
  fromUgly (AString t) | T.length t == 1 = Right (2, return (T.head t))
  fromUgly arg = home arg

instance {-# OVERLAPPING #-} FromUgly [Char] where
  fromUgly (AString t) = Right (0, return (T.unpack t))
  fromUgly arg = home arg

instance FromUgly T.Text where
  fromUgly (AString t) = Right (1, return t)
  fromUgly arg = home arg

instance FromUgly B.ByteString where
  fromUgly (ABytes b) = Right (0, return b)
  fromUgly arg = home arg

-- | Null is Nothing; anything else that converts is Just it.
instance FromUgly a => FromUgly (Maybe a) where
  fromUgly ANull = Right (0, return Nothing)
  fromUgly arg = case home arg of
    Right r -> Right r
    Left _ -> fmap (fmap (fmap Just)) (fromUgly arg)

instance {-# OVERLAPPABLE #-} Typeable a => FromUgly [a]

-- | [Value] (what an unconstrained [a] becomes) takes any sequence that
-- comes home, read lazily through its elements.
instance {-# OVERLAPPING #-} FromUgly [Value] where
  fromUgly arg@(AHome (Box _ i)) = case (home arg, infoElems i) of
    (Right r, _) -> Right r
    (_, Just es) -> Right (4, return (walk es 0))
    _ -> Left (describeArg arg ++ " isn't a sequence")
    where walk es n = maybe [] (: walk es (n + 1)) (elemsIndex es n)
  fromUgly arg = home arg
instance (Typeable a, Typeable b) => FromUgly (a, b)
instance (Typeable a, Typeable b, Typeable c) => FromUgly (a, b, c)
instance Typeable v => FromUgly (M.Map String v)
-- | A function: a Haskell function coming home (foreign callables come later).
instance (Typeable a, Typeable b) => FromUgly (a -> b)

-- | Value takes anything, for a type variable with no constraint.
instance FromUgly Value where
  fromUgly a = case a of
    ANull -> ok VNull
    ABool b -> ok (VBool b)
    AInt i -> ok (VInt i)
    ADouble d -> ok (VDouble d)
    AString t -> ok (VString t)
    ABytes b -> ok (VBytes b)
    AHome b -> ok (VBox b)
    AForeign _ -> Left "foreign values aren't supported in Haskell yet"
    where ok v = Right (4, return v)
