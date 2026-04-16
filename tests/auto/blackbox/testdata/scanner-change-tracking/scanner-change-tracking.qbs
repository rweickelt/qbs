import qbs.FileInfo
import "subdir/helper.js" as Helper

CppApplication {
    name: "app"
    property bool discoverDep5
    Depends { name: "m" }
    Scanner {
        condition: true
        inputs: 'none'
        recursive: false
        scan: {
            if (FileInfo.fileName(filePath) === "helper.cpp") // recursive case
                return ["dep4.txt"];
            var deps = ["dep1.txt", "dep3.txt", "helper.cpp"];
            deps = deps.concat(Helper.additionalDependencies(product));
            if (product.m.discoverDep6)
                deps.push("dep6.txt");
            if (input.m.discoverDep7)
                deps.push("dep7.txt");
            return deps;
        }
        searchPaths: { return [product.sourceDirectory] }
    }

    files: ["main.cpp"]
}
