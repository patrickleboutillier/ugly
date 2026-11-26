{-# LANGUAGE TemplateHaskell #-}

-- | A second module in the library Example.so, "Example.Points": there's no
-- Example.Points.so, so import finds it in Example.so, the longest prefix.
-- Its types' instances are Example.Ugly's.
module Example.Points () where

import Example
import Example.Ugly ()
import Ugly.Export

uglyModule "Example.Points" ['origin, 'Pt]
