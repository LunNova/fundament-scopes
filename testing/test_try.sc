do
    let ok =
        try
            print 1
            if true
                error! "runtime error"
            print 2
            true
        except (exc)
            print
                'format exc
            false
    assert (ok == false)

fn test-loop-xp ()
    loop (counter = 0)
        if (counter == 10)
            return;
        try
            if (counter == 5)
                error! "loop error"
            print "success branch" counter
            true
        except (exc)
            print "fail branch" counter exc
            print
                'format exc
            assert (counter == 5)
            false
        counter + 1

test-loop-xp;

