#
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.

""""testing
    =======

    The testing module simplifies writing and running tests in an ad-hoc
    fashion.

fn __test-modules (module-dir modules)
    let total =
        i32 (countof modules)

    loop (modules failed-modules = modules '())
        if (empty? modules)
            let failed = (i32 (countof failed-modules))
            if (failed > 0)
                print;
                print "List of failed modules"
                print "======================"
                for m in failed-modules
                    print "*" (m as Symbol as string)
            print;
            print total "tests executed," (total - failed) "succeeded," failed "failed."
            print "done."
            return;

        let module modules = (decons modules)
        let module = (module as Symbol)
        print "* running" (module as string)
        print "***********************************************"
        let ok =
            try
                require-from module-dir module
                true
            except (err)
                io-write!
                    'format err
                io-write! "\n"
                false
        repeat modules
            if ok
                failed-modules
            else
                cons module failed-modules

# (test-modules module ...)
define-syntax-macro test-modules
    list __test-modules 'module-dir
        list syntax-quote
            args

define-syntax-macro assert-error
    inline test-function (f)
        try
            if true
                f;
            false
        except (err)
            io-write! "ASSERT OK: "
            print
                'format err
            true

    inline assertion-error! (anchor msg)
        let assert-msg =
            .. "error assertion failed: "
                if (== (typeof msg) string) msg
                else (repr msg)
        syntax-error! anchor assert-msg
    let cond body = (decons args)
    let sxcond = cond
    let anchor = ('anchor sxcond)
    let tmp =
        sc_symbol_new_unique "tmp"
    list do
        list let tmp '=
            list test-function
                list fn '() cond
        list if tmp
        list 'else
            cons assertion-error!
                active-anchor;
                if (empty? body)
                    list
                        if (('typeof sxcond) == list)
                            repr (sxcond as list)
                        else
                            repr sxcond
                else body

define-syntax-macro assert-compiler-error
    inline test-function (f)
        try
            sc_compile (sc_typify f 0 null) 0:u64
            false
        except (err)
            io-write! "ASSERT OK: "
            print
                'format err
            true

    inline assertion-error! (anchor msg)
        let assert-msg =
            .. "compiler error assertion failed: "
                if (== (typeof msg) string) msg
                else (repr msg)
        syntax-error! anchor assert-msg
    let cond body = (decons args)
    let sxcond = cond
    let anchor = ('anchor sxcond)
    let tmp =
        sc_symbol_new_unique "tmp"
    list do
        list let tmp '=
            list test-function
                list fn '() cond
        list if tmp
        list 'else
            cons assertion-error!
                active-anchor;
                if (empty? body)
                    list
                        if (('typeof sxcond) == list)
                            repr (sxcond as list)
                        else
                            repr sxcond
                else body

locals;
