(function(exports, importNativeModule) {
    "use strict";

    const Importer = importNativeModule('_importer');
    const Gio = Importer.importGIModule('Gio', '2.0');

    function runOverridesForGIModule(module, moduleID) {
        let overridesModule = imports.overrides[moduleID];
        if (!overridesModule)
            return;

        let initFunc = overridesModule._init;
        if (!initFunc)
            return;

        initFunc.call(module);
    }

    function importGIModuleWithOverrides(parent, moduleID, moduleVersion) {
        let module = Importer.importGIModule(moduleID, moduleVersion);
        parent[moduleID] = module;
        runOverridesForGIModule(module, moduleID);
    }

    function installImports() {
        // Implement the global "imports" object.

        // imports.gi
        let gi = new Proxy({
            versions: {},
            __gjsPrivateNS: {},
        }, {
            get: function(target, name) {
                if (!target[name]) {
                    let version = target.versions[name] || null;
                    importGIModuleWithOverrides(target, name, version);
                }

                return target[name];
            },
        });

        function importModule(module, file) {
            let success, script;
            try {
                [success, script] = file.load_contents(null);
            } catch(e) {
                return null;
            }

            // Don't catch errors for the eval, as those should propagate
            // back up to the user...
            Importer.evalWithScope(module, script, file.get_parse_name());
            return module;
        }

        function importFile(parent, name, file) {
            let module = {};
            parent[name] = module;
            module.__file__ = file.get_parse_name();
            module.__moduleName__ = name;
            module.__parentModule__ = parent;
            importModule(module, file);
        }

        function importDirectory(parent, name) {
            let searchPath = parent.searchPath.map(function(path) {
                return path + '/' + name;
            }).filter(function(path) {
                let file = Gio.File.new_for_commandline_arg(path);
                let type = file.query_file_type(Gio.FileQueryInfoFlags.NONE, null);
                return (type == Gio.FileType.DIRECTORY);
            });

            let module = createSearchPathImporter();
            parent[name] = module;
            module.searchPath = searchPath;
            module.__moduleName__ = name;
            module.__parentModule__ = parent;

            tryImport(module, '__init__');
        }

        function tryImport(proxy, name) {
            function tryPath(path) {
                let file, type;
                file = Gio.File.new_for_commandline_arg(path);
                type = file.query_file_type(Gio.FileQueryInfoFlags.NONE, null);
                if (type == Gio.FileType.DIRECTORY) {
                    importDirectory(proxy, name);
                    return true;
                } else {
                    file = Gio.File.new_for_commandline_arg(path + '.js');
                    if (file.query_exists(null)) {
                        importFile(proxy, name, file);
                        return true;
                    }
                }
                return false;
            }

            for (let path of proxy.searchPath) {
                let modulePath = path + '/' + name;
                if (tryPath(modulePath))
                    return;
            }
        }

        function createSearchPathImporter() {
            let proxy = new Proxy({ __init__: {} }, {
                get: function(target, name) {
                    if (target.__init__[name])
                        return target.__init__[name];

                    if (!target[name])
                        tryImport(proxy, name);

                    return target[name];
                },
            });
            return proxy;
        }

        let rootDirectoryImporter = createSearchPathImporter();
        rootDirectoryImporter.searchPath = Importer.getBuiltinSearchPath();

        // root importer, checks for native modules
        let rootImporter = new Proxy(rootDirectoryImporter, {
            get: function(target, name) {
                if (!target[name])
                    target[name] = importNativeModule(name);
                if (!target[name])
                    target[name] = rootDirectoryImporter[name];
                return target[name];
            },
        });
        rootImporter.gi = gi;

        exports.imports = rootImporter;
    }
    installImports();

})(window, importNativeModule);
