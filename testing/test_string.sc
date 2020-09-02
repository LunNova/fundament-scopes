using import testing
using import String

# raw string blocks
###################

# keeps first trailing LF
test
    "hello world\n" == """"hello world

# doesn't escape anything
test
    "hello \"world\"\\n\n" == """"hello "world"\n

# sub 4-space indentation is illegal
#test
    "hello world\nwhat's up\n" == """"hello world
                                     what's up

# all indentation up to 4-spaces is trimmed
test
    "hello world\nwhat's up\n" == """"hello world
                                      what's up

# empty first line isn't trimmed
test
    ==
        """"
            hi everyone
        "\nhi everyone\n"

# nested use of string block token has no effect
test
    ==
        "\"\"\"\"\n\"\"\"\"\n    \"\"\"\"\n"
        """"""""
            """"
                """"

# multiline block with indented lines
test
    ==
        """"first line
            second line

            third "line"
                fourth line

        # all string compares are done at runtime
        "first line\nsecond line\n\nthird \"line\"\n    fourth line\n"

# global strings
###################

let str = (sc_global_string_new "te\x00st\n" 6)
run-stage;
print (sizeof str)
let C = (include "stdio.h")
C.extern.printf str
print (imply str rawstring)
for k in str
    print k

# strings objects
###################

do
    local s : String
    test ((countof s) == 0)
    test ((s @ 0) == 0:i8)
    s = "salvage"
    test ((countof s) == 7)
    test (s == "salvage")
    test (s != "solvager")
    test (s != "solvag")
    test (s != "salvate")
    test (s >= "salvage")
    test (s > "Salvage")
    test (s < "zal")
    s = ("test" as rawstring)
    test ((countof s) == 4)
    s ..= s
    test (s == "testtest")
    let t = (copy s)
    for a b in (zip s t)
        test (a == b)
    test (s == t)
    test ((hash s) == (hash t))
    local q : String = "init"
    test (q == "init")
    test ((String "test") == "test")
    'append q "tini"
    test (q == "inittini")
    'append q (String "init")
    test (q == "inittiniinit")
    ;

# testing proper globalization
local s = (String "test")
run-stage;
test (s == "test")

;