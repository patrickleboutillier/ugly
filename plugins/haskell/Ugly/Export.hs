{-# LANGUAGE CPP, TemplateHaskell #-}

-- | Export tables, generated at compile time: the reflection GHC doesn't
-- have at run time.
--
-- > uglyModule "Music.Diatonic" [''Note, ''Chord, 'C, 'transpose, 'tonic]
--
-- declares 'uglyExports', the module's box, from a list written by hand
-- (TH can reify a name, but can't list a module's exports):
--
-- * A type name (''Note) gets 'ToUgly' and 'FromUgly' instances, unless it
--   has them: a box with its 'show' as text (if it has 'Show'), whose
--   methods (for invoke, receiver last) are this module's exports.
-- * A value name ('transpose) is an export. A polymorphic one is expanded
--   into its instantiations, one per combination of instances (in scope
--   here) satisfying its context, each type variable without a constraint
--   becoming 'Value'; a call picks one by its arguments ('overloaded').
--   Instantiations that would need a type with no ToUgly/FromUgly instance
--   are left out, silently (Ord has instances for many types that can't
--   cross); a listed value with none left, or with a type variable of a
--   higher kind, is a compile error.
--
-- The instances are orphans (the export module is neither the class's nor
-- the type's), so an export module needs {-# OPTIONS_GHC -Wno-orphans #-}.
--
-- A module is found in a module library (a .so built against the package
-- 'ugly') by a C symbol, ugly_hs_module_<the name, encoded>, a foreign
-- export giving its box. 'uglyModule' declares it, and so does
-- 'uglyModuleFrom', for a module whose box is written by hand.
module Ugly.Export (uglyModule, uglyModuleFrom, moduleSymbol) where

import Control.Monad (filterM, foldM, forM)
import qualified Data.ByteString as B
import Data.Char (isAsciiLower, isAsciiUpper, isDigit, chr)
import Data.List (intercalate, nub)
import qualified Data.Map as M
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import Data.Typeable (Proxy (..), Typeable, typeRep)
import Foreign.StablePtr (StablePtr, newStablePtr)
import Language.Haskell.TH
import Numeric (showHex)

import Ugly.Value (Box, FromUgly, ToUgly (..), Value, boxRecord, boxWith, constructor, instantiation, moduleBox, overloaded)

type Subst = M.Map Name Type

-- | One splice per Haskell module: it declares 'uglyExports'.
uglyModule :: String -> [Name] -> Q [Dec]
uglyModule modName names = do
  infos <- forM names $ \n -> (,) n <$> reify n
  let types = [ n | (n, TyConI _) <- infos ]
      values = [ (n, i) | (n, i) <- infos, not (isType i) ]
  instances <- concat <$> mapM (typeInstances types) types
  entries <- concat <$> mapM (entry types) values
  let entriesName = mkName "uglyEntries"
      exportsName = mkName "uglyExports"
  entriesSig <- sigD entriesName [t| [(String, Value)] |]
  entriesDec <- valD (varP entriesName) (normalB (listE (map return entries))) []
  exportsSig <- sigD exportsName [t| Box |]
  exportsDec <- valD (varP exportsName) (normalB [| moduleBox modName $(varE entriesName) |]) []
  root <- uglyModuleFrom modName exportsName
  return (instances ++ [entriesSig, entriesDec, exportsSig, exportsDec] ++ root)
  where
    isType (TyConI _) = True
    isType _ = False

-- | Declares module modName, whose box is the top-level 'Box' named: the
-- foreign export the plugin finds it by (moduleSymbol), giving a new stable
-- pointer to the box each time (the plugin frees it).
--
-- > smoke :: Box
-- > smoke = moduleBox "Smoke" [("answer", toUgly (42 :: Int))]
-- > uglyModuleFrom "Smoke" 'smoke
uglyModuleFrom :: String -> Name -> Q [Dec]
uglyModuleFrom modName box = do
  let root = mkName ("uglyRoot_" ++ drop (length symbolPrefix) (moduleSymbol modName))
  ty <- [t| IO (StablePtr Box) |]
  def <- valD (varP root) (normalB [| newStablePtr $(varE box) |]) []
  return [SigD root ty, def, ForeignD (ExportF CCall (moduleSymbol modName) root ty)]

-- | The C symbol of a module's foreign export: ugly_hs_module_, then the
-- name's UTF-8 bytes, ASCII letters and digits as they are, any other byte
-- as _ and two hex digits: Music.Diatonic is ugly_hs_module_Music_2eDiatonic.
-- haskell.c encodes the same way.
moduleSymbol :: String -> String
moduleSymbol name = symbolPrefix ++ concatMap enc (B.unpack (TE.encodeUtf8 (T.pack name)))
  where
    enc b = let c = chr (fromIntegral b) in
      if isAsciiUpper c || isAsciiLower c || isDigit c then [c] else '_' : pad (showHex b "")
    pad h = replicate (2 - length h) '0' ++ h

symbolPrefix :: String
symbolPrefix = "ugly_hs_module_"

-- | ToUgly and FromUgly for a listed type, unless it has them.
typeInstances :: [Name] -> Name -> Q [Dec]
typeInstances listed t = do
  info <- reify t
  case info of
    TyConI dec | not (null (tyParams dec)) ->
      fail ("uglyModule: " ++ nameBase t ++ " has type parameters; only types of kind * can be listed")
    _ -> return ()
  hasTo <- isInstance ''ToUgly [ConT t]
  hasFrom <- isInstance ''FromUgly [ConT t]
  hasShow <- isInstance ''Show [ConT t]
  -- A record (one constructor, with fields): its fields that can cross are members.
  fields <- case info of
    TyConI (DataD _ _ _ _ [RecC _ fs] _) -> crossing fs
    TyConI (NewtypeD _ _ _ _ (RecC _ fs) _) -> crossing fs
    _ -> return []
  let methods = [| (`lookup` $(varE (mkName "uglyEntries"))) |]
      text = if hasShow then [| Just . show |] else [| const Nothing |]
      members = listE [ [| ($(stringE (nameBase f)), \r -> toUgly ($(varE f) r)) |] | f <- fields ]
  to <- if hasTo then return [] else
    if null fields
      then [d| instance ToUgly $(conT t) where
                 toUgly x = boxWith ($text x) $methods x
                 textOf = $text |]
      else [d| instance ToUgly $(conT t) where
                 toUgly x = boxRecord ($text x) $methods $members x
                 textOf = $text |]
  from <- if hasFrom then return [] else [d| instance FromUgly $(conT t) |]
  return (to ++ from)
  where
    crossing fs = map (\(f, _, _) -> f) <$> filterM (\(_, _, ft) -> holds listed (AppT (ConT ''ToUgly) ft)) fs
    tyParams (DataD _ _ ps _ _ _) = ps
    tyParams (NewtypeD _ _ ps _ _ _) = ps
    tyParams (TySynD _ ps _) = ps
    tyParams _ = []

-- | An export's entry, (name, value): a compile error if it has no
-- usable instantiation.
entry :: [Name] -> (Name, Info) -> Q [Exp]
entry listed (n, info) = do
  ty <- unlinear <$> case info of
    VarI _ t _ -> return t
    ClassOpI _ t _ -> return t
    DataConI _ t _ -> return t
    _ -> fail ("uglyModule: " ++ nameBase n ++ " isn't a value or a type")
  let (vars, ctx, body) = splitType ty
      ref = case info of
        DataConI {} -> ConE n
        _ -> VarE n
  if any (`appliedIn` body) vars
    then failWith "type variables of higher kinds aren't supported yet"
    else do
      substs <- foldM (\ss c -> concat <$> mapM (`extend` c) ss) [M.empty] ctx
      let close s = M.union s (M.fromList [ (v, ConT ''Value) | v <- vars ])
      satisfied <- filterM (\s -> and <$> mapM (holds listed . apply (close s)) ctx) substs
      -- Each instantiation: its type arguments, in the variables' order, and its type.
      let candidates = nub [ (map (apply (close s) . VarT) vars, apply (close s) body) | s <- satisfied ]
      usable <- filterM (usableType listed . snd) candidates
      case usable of
        [] -> failWith "no usable instantiation (a type without ToUgly/FromUgly, or an unsatisfied context)"
        _ | null vars -> one [| toUgly $(return ref) |]
        -- Even one instantiation is overloaded: a hint names it, and want checks its result.
        _ -> one [| overloaded $(stringE (nameBase n ++ " :: " ++ pretty ty))
                      $(listE [ [| instantiation $(listE (map typeArg targs)) ($(return ref) :: $(return m)) |]
                              | (targs, m) <- usable ]) |]
  where
    -- A data constructor is also what new calls (INSTANTIABLE).
    one e = case info of
      DataConI {} -> (: []) <$> [| ($(stringE (nameBase n)), constructor $e) |]
      _ -> (: []) <$> [| ($(stringE (nameBase n)), $e) |]
    typeArg t = [| show (typeRep (Proxy :: Proxy $(return t))) |]
    failWith msg = fail ("uglyModule: " ++ nameBase n ++ ": " ++ msg)

-- | forall vars. context => body
splitType :: Type -> ([Name], [Type], Type)
splitType (ForallT bs c t) = let (vs, cs, b) = splitType t in (map tvName bs ++ vs, c ++ cs, b)
splitType t = ([], [], t)

tvName :: TyVarBndr' -> Name
#if MIN_VERSION_template_haskell(2,17,0)
type TyVarBndr' = TyVarBndr Specificity
tvName (PlainTV v _) = v
tvName (KindedTV v _ _) = v
#else
type TyVarBndr' = TyVarBndr
tvName (PlainTV v) = v
tvName (KindedTV v _) = v
#endif

-- | Linear arrows as ordinary ones: since GHC 9.0, TH gives a data
-- constructor's type with them (Double %1 -> Shape), and a constructor can
-- always be used at the ordinary type.
unlinear :: Type -> Type
unlinear t = case t of
#if MIN_VERSION_template_haskell(2,16,0)
  AppT (AppT MulArrowT _) a -> AppT ArrowT (unlinear a)
#endif
  AppT a b -> AppT (unlinear a) (unlinear b)
  ForallT bs c b -> ForallT bs (map unlinear c) (unlinear b)
  _ -> t

-- | Whether v is applied to something (f a): a variable of a higher kind.
appliedIn :: Name -> Type -> Bool
appliedIn v t = case t of
  AppT (VarT w) _ | w == v -> True
  AppT a b -> appliedIn v a || appliedIn v b
  _ -> False

-- | Extends a substitution with each instance of a constraint's class whose
-- head matches it, as far as it's known. Instances whose heads have type
-- variables of their own are left out.
extend :: Subst -> Type -> Q [Subst]
extend s c = case unapp (apply s c) of
  Just (cls, args) -> do
    is <- reifyInstances cls args
    return [ s' | InstanceD _ _ h _ <- is, Just (_, iargs) <- [unapp h], not (any hasVar iargs)
                , Just s' <- [foldM bind s (zip args iargs)] ]
  Nothing -> return [s]
  where
    bind sub (VarT v, t) = case M.lookup v sub of
      Nothing -> Just (M.insert v t sub)
      Just t' | t' == t -> Just sub
      _ -> Nothing
    bind sub (a, b) | a == b = Just sub
    bind sub _ = Just sub     -- a concrete argument against the head: reifyInstances matched it

-- | Whether a constraint holds, its instance's own context included (Ord
-- [a] needs Ord a). A listed type has the instances this splice makes.
holds :: [Name] -> Type -> Q Bool
holds listed = go (8 :: Int)
  where
    go 0 _ = return False
    go depth c = case unapp c of
      Nothing -> return True
      Just (cls, args)
        | any hasVar args -> return False
        | cls `elem` [''ToUgly, ''FromUgly], [ConT t] <- args, t `elem` listed -> return True
        | cls == ''Typeable -> return True
        | otherwise -> do
            is <- reifyInstances cls args
            case [ (h, cx) | InstanceD _ cx h _ <- is ] of
              [] -> return False
              (h, cx) : _ -> case unapp h of
                Just (_, hargs) | Just s <- matchAll hargs args -> and <$> mapM (go (depth - 1) . apply s) cx
                _ -> return True

-- | Whether a monomorphic function type can be called: its parameters
-- FromUgly, its result (an IO's result) ToUgly. A parameter that is itself
-- a function (a callback: a Haskell function coming home, or a foreign
-- callable) the other way round: its parameters ToUgly, its result FromUgly.
usableType :: [Name] -> Type -> Q Bool
usableType listed t = do
  let (params, result) = arrows t
  ps <- mapM param params
  r <- holds listed (AppT (ConT ''ToUgly) (unIO result))
  return (and ps && r)
  where
    param p
      | isArrow p = do
          let (cps, cr) = arrows p
          as <- mapM (holds listed . AppT (ConT ''ToUgly)) cps
          b <- holds listed (AppT (ConT ''FromUgly) (unIO cr))
          return (and as && b)
      | otherwise = holds listed (AppT (ConT ''FromUgly) p)
    arrows (AppT (AppT ArrowT a) b) = let (ps, r) = arrows b in (a : ps, r)
    arrows x = ([], x)
    isArrow (AppT (AppT ArrowT _) _) = True
    isArrow _ = False
    unIO (AppT (ConT io) x) | io == ''IO = x
    unIO x = x

-- | Matches instance head arguments (with variables) against concrete ones.
matchAll :: [Type] -> [Type] -> Maybe Subst
matchAll hs ts = foldM go M.empty (zip hs ts)
  where
    go s (VarT v, t) = case M.lookup v s of
      Nothing -> Just (M.insert v t s)
      Just t' | t' == t -> Just s
      _ -> Nothing
    go s (AppT a b, AppT c d) = go s (a, c) >>= \s' -> go s' (b, d)
    go s (a, b) | a == b = Just s
    go _ _ = Nothing

unapp :: Type -> Maybe (Name, [Type])
unapp t = go t []
  where
    go (AppT f x) as = go f (x : as)
    go (ConT c) as = Just (c, as)
    go _ _ = Nothing

hasVar :: Type -> Bool
hasVar (VarT _) = True
hasVar (AppT a b) = hasVar a || hasVar b
hasVar _ = False

apply :: Subst -> Type -> Type
apply s (VarT v) = M.findWithDefault (VarT v) v s
apply s (AppT a b) = AppT (apply s a) (apply s b)
apply s (ForallT bs c t) = ForallT bs (map (apply s) c) (apply s t)
apply _ t = t

-- | A type as its declaration reads, without module qualifiers or forall:
-- "Nte a => Interval -> (Interval -> Note -> Note) -> a -> a".
pretty :: Type -> String
pretty ty = let (_, ctx, body) = splitType ty
                plain = unqualify body
                cs = map unqualify ctx
            in unwords (words ((if null cs then "" else context cs ++ " => ") ++ pprint plain))
  where
    context [c] = pprint c
    context cs = "(" ++ intercalate ", " (map pprint cs) ++ ")"
    unqualify t = case t of
      ConT n -> ConT (mkName (nameBase n))
      VarT n -> VarT (mkName (nameBase n))
      AppT a b -> AppT (unqualify a) (unqualify b)
      _ -> t
