import qbs.Host

Product {
    condition: {
        var result = qbs.targetPlatform === Host.platform();
        if (!result)
            console.info("targetPlatform differs from hostPlatform");
        return result;
    }

    name: "lrelease-test"
    type: ["ts"]
    Depends { name: "Qt.core" }
    files: ["de.ts", "hu.ts"]
}
