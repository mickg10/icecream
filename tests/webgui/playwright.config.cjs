module.exports = {
    testDir: ".",
    timeout: 60000,
    expect: {
        timeout: 10000
    },
    fullyParallel: false,
    retries: 0,
    reporter: [["line"]],
    outputDir: "tests/webgui/test-results",
    use: {
        headless: true,
        viewport: { width: 1640, height: 1040 }
    }
};
