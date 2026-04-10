import "subdir/helper.js" as Helper

CppApplication {
    Scanner {
        condition: true
        inputs: 'none'
        recursive: false
        scan: {
            if (input.fileName === "helper.cpp") // recursive case
                return ["dep4.txt"];
            var deps = ["dep1.txt", "dep3.txt", "helper.cpp"];
            deps = deps.concat(Helper.additionalDependencies());
            return deps;
        }
        searchPaths: { return [product.sourceDirectory] }
    }

    files: ["main.cpp"]
}
