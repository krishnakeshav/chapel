use Time;

class LocC {
  var id: int;
  
  proc writeThis(x: Writer) {
    on this {
      x.write(id);
    }
  }
}

class C {
  var locCs: [LocaleSpace] LocC;

  proc initialize() {
    for loc in LocaleSpace {
      on Locales(loc) {
        locCs(loc) = new LocC(loc);
      }
    }
  }

  proc writeThis(x: Writer) {
    for loc in LocaleSpace {
      on Locales(loc) {
        if loc != 0 then
          write(" ");
        write(locCs(loc));
      }
    }
  }
}

// Once this test is working, only the first
// branch of this cobegin is necessary; the
// second branch is just to kill the test in
// a reasonable amount of time while it's not
// working.
cobegin {
  {
    var myC = new C();
    writeln("C is: ", myC);
  }
  {
    sleep(10);
    halt("Timed out");
  }
}
