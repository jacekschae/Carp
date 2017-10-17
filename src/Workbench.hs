module Workbench where

import ColorText
import Obj
import Types
import Commands
import Template
import Parsing
import Infer
import Constraints
import Emit
import qualified Data.Map as Map

-- | Not part of the cabal file, just for interactive repl sessions:

pt :: XObj -> IO ()
pt = putStrLn . prettyTyped 

