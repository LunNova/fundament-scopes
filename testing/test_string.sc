
# raw string blocks
###################

# keeps first trailing LF
assert
    "hello world\n" == """hello world

# doesn't escape anything
assert
    "hello \"world\"\\n\n" == """hello "world"\n

# sub 4-space indentation is trimmed
assert
    "hello world\nwhat's up\n" == """hello world
                                   what's up

# all indentation up to 4-spaces is trimmed
assert
    "hello world\n  what's up\n" == """hello world
                                          what's up

# empty first line isn't trimmed
assert
    ==
        """
            hi everyone
        "\nhi everyone\n"

# multiline block with indented lines
assert
    ==
        """first line
            second line

            third "line"
                fourth line
            fifth line
                sixth line

        # we're doing static compare of a string with more than 32 characters, do it at runtime
        unconst "first line\nsecond line\n\nthird \"line\"\n    fourth line\nfifth line\n    sixth line\n"
