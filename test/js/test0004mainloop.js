var Mainloop = imports.mainloop;

function testBasicMainloop() {
    log('running mainloop test');
    Mainloop.idle_add(function() { Mainloop.quit('testMainloop'); });
    Mainloop.run('testMainloop');
    log('mainloop test done');
}

/* A dangling timeout should get removed and not leaked */
function testDanglingTimeout() {
    Mainloop.timeout_add(5000, function() { log("this should not run"); });
}

gjstestRun();
