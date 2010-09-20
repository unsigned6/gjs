var DBus = imports.dbus;
var Mainloop = imports.mainloop;

function testExportName() {
    var exportedNameId = DBus.session.acquire_name('com.litl.Real', DBus.SINGLE_INSTANCE,
						   function(name){log("Acquired name " + name); Mainloop.quit('dbus'); },
						   function(name){log("Lost name  " + name);});
    Mainloop.run('dbus');
}

gjstestRun();
