import qualified Ugly

main = Ugly.withUgly $ do
  json <- Ugly.module_ "python" "json"
  xs <- Ugly.invoke json "loads" [Ugly.toValue "[1, 2, 3]"] >>= Ugly.fromValue
  print (sum xs :: Int)                     -- a Python list, as a [Int]
  twice <- Ugly.eval "python" "lambda f, x: f(f(x))"
  print =<< (Ugly.fromValue =<< Ugly.callValue twice
    [Ugly.toValue ((+ 1) :: Int -> Int), Ugly.toValue (40 :: Int)] :: IO Int)   -- 42: Python calls back

