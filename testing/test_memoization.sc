
import fnchain

fn memoized ()
    typename "bang"

fn memoized-clone ()
    typename "bang"

fn memoized2 (x)
    typename x

fn! not-memoized ()
    typename "T"

assert
    (not-memoized) != (not-memoized)

assert
    (memoized) == (memoized)
assert
    (memoized) != (memoized-clone)

assert
    (memoized2 "test") == (memoized2 "test")

let chain1 = (fnchain "test")
let chain2 = (fnchain "test")
assert (chain1 != chain2)
