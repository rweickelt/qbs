function additionalDependencies(product) {
    var deps = [/*'dep2.txt'*/];
    if (product.discoverDep5)
        deps.push("dep5.txt");
    return deps;
}
