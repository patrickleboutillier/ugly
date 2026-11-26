{-# LANGUAGE ExistentialQuantification, FlexibleInstances, ScopedTypeVariables #-}

-- | Haskell values as the protocol sees them.
--
-- A 'Value' is a primitive, which crosses by value, or a 'Box': a Haskell
-- value with the operations the protocol may ask of it, closed over it where
-- its type was static. A box needs no class dictionary at run time, and no
-- reflection: describe, show, members and elements are already closures.
--
-- 'ToUgly' decides which: its instances are the value mapping of
-- doc/haskell-bridge.md. Two choices to confirm with the author are built in:
-- integers out of range don't wrap (an 'Integer' beyond int64 is a box, not
-- an int), and 'Maybe' is flattened ('Nothing' is null, 'Just' x is x) except
-- where that would lose information ('Just' of something null stays a box).
module Ugly.Value
  ( Value (..), Box (..), Info (..), Members (..), Elems (..)
  , ToUgly (..)
  , boxShow, boxOpaque, moduleBox, typeName, validText
  ) where

import qualified Data.ByteString as B
import Data.Char (ord)
import Data.Int
import Data.List (intercalate)
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

-- | A Haskell value, its identity (the value itself), and what can be asked of it.
data Box = forall a. Box a Info

data Info = Info
  { infoType :: String              -- ^ describe's type name: "Note", "[Int]", "module"
  , infoShow :: Maybe String        -- ^ its text, for want = string (lazy: forced when asked)
  , infoMembers :: Maybe Members    -- ^ MEMBERS
  , infoElems :: Maybe Elems        -- ^ ELEMENTS
  }

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

-- | How a Haskell type crosses. Every type is 'Typeable', for its name.
class Typeable a => ToUgly a where
  toUgly :: a -> Value
  -- | Its text as 'show' would give it, if the type has one: what a box of
  -- it, or a list of them, shows as.
  textOf :: a -> Maybe String
  textOf _ = Nothing

typeName :: forall a. Typeable a => a -> String
typeName _ = show (typeRep (Proxy :: Proxy a))

-- | A value of a type with 'Show': a box with a type name and a text.
boxShow :: (Typeable a, Show a) => a -> Value
boxShow x = VBox (Box x (Info (typeName x) (Just (show x)) Nothing Nothing))

-- | A value with nothing to ask but its type.
boxOpaque :: Typeable a => a -> Value
boxOpaque x = VBox (Box x (Info (typeName x) Nothing Nothing Nothing))

-- | A module: its exports as members. The list is its identity, so the same
-- module is the same handle while one is live.
moduleBox :: String -> [(String, Value)] -> Box
moduleBox name entries = Box entries Info
  { infoType = "module"
  , infoShow = Just ("module " ++ name)
  , infoMembers = Just (Members (map fst entries) (`lookup` entries))
  , infoElems = Nothing
  }

-- | Text that is valid Unicode. A Haskell 'Char' can be a lone surrogate,
-- which UTF-8 can't encode (and Data.Text would silently replace).
validText :: String -> Bool
validText = all (\c -> ord c < 0xD800 || ord c > 0xDFFF)

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
  toUgly xs = VBox (Box xs (Info (typeName xs) (textOf xs) Nothing (Just elems)))
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
    VNull -> VBox (Box m (Info (typeName m) (textOf m) Nothing (Just (Elems 1 one))))
    v -> v
    where one i = if i == 0 then Just VNull else Nothing
  textOf Nothing = Just "Nothing"
  textOf (Just x) = ("Just " ++) . paren <$> textOf x
    where paren t = if ' ' `elem` t || take 1 t == "-" then "(" ++ t ++ ")" else t

-- | Tuples are read-only elements.
instance (ToUgly a, ToUgly b) => ToUgly (a, b) where
  toUgly p@(a, b) = VBox (Box p (Info (typeName p) (textOf p) Nothing (Just (Elems 2 at))))
    where at i = lookup i [(0, toUgly a), (1, toUgly b)]
  textOf (a, b) = (\x y -> "(" ++ x ++ "," ++ y ++ ")") <$> textOf a <*> textOf b

instance (ToUgly a, ToUgly b, ToUgly c) => ToUgly (a, b, c) where
  toUgly t@(a, b, c) = VBox (Box t (Info (typeName t) (textOf t) Nothing (Just (Elems 3 at))))
    where at i = lookup i [(0, toUgly a), (1, toUgly b), (2, toUgly c)]
  textOf (a, b, c) = (\x y z -> "(" ++ x ++ "," ++ y ++ "," ++ z ++ ")") <$> textOf a <*> textOf b <*> textOf c

-- | A map with string keys: its entries are members (read-only).
instance ToUgly v => ToUgly (M.Map String v) where
  toUgly m = VBox (Box m (Info (typeName m) Nothing (Just members) Nothing))
    where members = Members (M.keys m) (\k -> toUgly <$> M.lookup k m)

instance ToUgly Value where
  toUgly = id
