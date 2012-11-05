// application/javascript;version=1.8
const GJSON = imports.gjson;
const Gio = imports.gi.Gio;

function testLoad() {
    var jsonData = "{'hello': 'world', 'v': 42, 'x': 3.14159}";
    var mem = Gio.MemoryInputStream.new_from_bytes(GLib.Bytes.new(jsonData, );
                                                       
}

function testIdle() {
    var trackIdles = {
        runTwiceCount : 0,
        runOnceCount : 0,
        neverRunsCount : 0,
        quitAfterManyRunsCount : 0
    };
    Mainloop.idle_add(function() {
                          trackIdles.runTwiceCount += 1;
                          if (trackIdles.runTwiceCount == 2)
                              return false;
                          else
                              return true;
                      });
    Mainloop.idle_add(function() {
                          trackIdles.runOnceCount += 1;
                          return false;
                      });
    var neverRunsId =
        Mainloop.idle_add(function() {
                              trackIdles.neverRunsCount += 1;
                              return false;
                          });
    Mainloop.idle_add(function() {
                          trackIdles.quitAfterManyRunsCount += 1;
                          if (trackIdles.quitAfterManyRunsCount > 10) {
                              Mainloop.quit('foobar');
                              return false;
                          } else {
                              return true;
                          }
                      });

    Mainloop.source_remove(neverRunsId);

    Mainloop.run('foobar');

    assertEquals("one-shot ran once", 1, trackIdles.runOnceCount);
    assertEquals("two-shot ran twice", 2, trackIdles.runTwiceCount);
    assertEquals("removed never ran", 0, trackIdles.neverRunsCount);
    assertEquals("quit after many ran 11", 11, trackIdles.quitAfterManyRunsCount);

    // check re-entrancy of removing closures while they
    // are being invoked

    trackIdles.removeId = Mainloop.idle_add(function() {
                                                Mainloop.source_remove(trackIdles.removeId);
                                                Mainloop.quit('foobar');
                                                return false;
                                            });
    Mainloop.run('foobar');

    // Add an idle before exit, then never run main loop again.
    // This is to test that we remove idle callbacks when the associated
    // JSContext is blown away. The leak check in gjs-unit will
    // fail if the idle function is not garbage collected.
    Mainloop.idle_add(function() {
                          fail("This should never have been called");
                          return true;
                      });
}

gjstestRun();
