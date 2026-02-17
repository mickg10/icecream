const fs = require("fs");
const http = require("http");
const net = require("net");
const os = require("os");
const path = require("path");
const { spawn } = require("child_process");
const { test, expect } = require("@playwright/test");

function freePort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.unref();
        server.on("error", reject);
        server.listen(0, "127.0.0.1", () => {
            const address = server.address();
            const port = address && typeof address === "object" ? address.port : 0;
            server.close((err) => {
                if (err) {
                    reject(err);
                    return;
                }
                resolve(port);
            });
        });
    });
}

function waitForHttpJson(url, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    return new Promise((resolve, reject) => {
        const once = () => {
            const req = http.get(url, (res) => {
                const status = Number(res.statusCode || 0);
                res.resume();
                if (status >= 200 && status < 300) {
                    resolve();
                    return;
                }
                if (Date.now() > deadline) {
                    reject(new Error(`timeout waiting for ${url}, last status=${status}`));
                    return;
                }
                setTimeout(once, 150);
            });
            req.on("error", () => {
                if (Date.now() > deadline) {
                    reject(new Error(`timeout waiting for ${url}`));
                    return;
                }
                setTimeout(once, 150);
            });
        };
        once();
    });
}

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

let daemonProcess;
let daemonLogPath;
let daemonTmpDir;
let baseUrl;

test.beforeAll(async () => {
    const repoRoot = path.resolve(__dirname, "..", "..");
    const daemonBin = path.join(repoRoot, "daemon", "iceccd");
    if (!fs.existsSync(daemonBin)) {
        throw new Error(`missing daemon binary: ${daemonBin}`);
    }

    daemonTmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "iceccd-webgui-pw-"));
    daemonLogPath = path.join(daemonTmpDir, "iceccd-webgui.log");
    const socketPath = path.join(daemonTmpDir, "iceccd.socket");
    const webPort = await freePort();
    baseUrl = `http://127.0.0.1:${webPort}`;

    const logFd = fs.openSync(daemonLogPath, "w");
    daemonProcess = spawn(
        daemonBin,
        ["--no-remote", "-m", "0", "--webgui", "--webgui-port", String(webPort), "--webgui-addr", "127.0.0.1", "-n", "playwright-webgui", "-v"],
        {
            cwd: repoRoot,
            env: { ...process.env, ICECC_TEST_SOCKET: socketPath },
            stdio: ["ignore", logFd, logFd]
        }
    );
    fs.closeSync(logFd);

    daemonProcess.on("error", (err) => {
        throw err;
    });

    await waitForHttpJson(`${baseUrl}/api/state`, 20000);
});

test.afterAll(async () => {
    if (daemonProcess && !daemonProcess.killed) {
        daemonProcess.kill("SIGTERM");
        for (let i = 0; i < 20; ++i) {
            if (daemonProcess.exitCode !== null) {
                break;
            }
            await sleep(100);
        }
        if (daemonProcess.exitCode === null) {
            daemonProcess.kill("SIGKILL");
        }
    }
});

test("web gui renders dashboard and styles", async ({ page }, testInfo) => {
    await page.goto(baseUrl, { waitUntil: "domcontentloaded" });

    await expect(page).toHaveTitle(/iceccd web gui/i);
    await expect(page.locator("h1.title")).toContainText("iceccd live dashboard");
    await expect(page.locator("#scheduler")).not.toHaveText("-");
    await expect(page.locator("#slots")).toContainText("/");
    await expect(page.locator("#preprocess-slots")).toContainText("/");
    await expect(page.locator("#clients-total")).not.toHaveText("-");
    await expect(page.locator("#job-limit")).toBeVisible();

    const backgroundImage = await page.evaluate(() => getComputedStyle(document.body).backgroundImage);
    expect(backgroundImage).not.toBe("none");
    expect(backgroundImage.length).toBeGreaterThan(20);

    const barRows = await page.locator("#status-bars .bar-row").count();
    if (barRows === 0) {
        await expect(page.locator("#status-bars")).toContainText("No active clients");
    }

    await page.selectOption("#job-limit", "200");
    await expect(page.locator("#jobs-sub")).toContainText("rows");
    await page.screenshot({ path: testInfo.outputPath("webgui-dashboard.png"), fullPage: true });
});

test("web gui api endpoints return structured data", async ({ request }) => {
    const state = await request.get(`${baseUrl}/api/state`);
    expect(state.ok()).toBeTruthy();
    const stateJson = await state.json();
    expect(stateJson.type).toBe("iceccd_state");
    expect(stateJson.node).toBeTruthy();
    expect(stateJson.clients).toBeTruthy();

    const clients = await request.get(`${baseUrl}/api/clients`);
    expect(clients.ok()).toBeTruthy();
    const clientsJson = await clients.json();
    expect(clientsJson.type).toBe("iceccd_clients");
    expect(Array.isArray(clientsJson.clients)).toBeTruthy();
    if (clientsJson.clients.length > 0) {
        expect(typeof clientsJson.clients[0].cmdline).toBe("string");
    }

    const jobs = await request.get(`${baseUrl}/api/jobs?limit=5`);
    expect(jobs.ok()).toBeTruthy();
    const jobsJson = await jobs.json();
    expect(jobsJson.type).toBe("iceccd_job_history");
    expect(jobsJson.capacity).toBe(20000);
    expect(jobsJson.returned).toBeLessThanOrEqual(5);
    expect(Array.isArray(jobsJson.jobs)).toBeTruthy();
    if (jobsJson.jobs.length > 0) {
        expect(typeof jobsJson.jobs[0].cmdline).toBe("string");
    }

    const insights = await request.get(`${baseUrl}/insights`);
    expect(insights.ok()).toBeTruthy();
    const insightsHtml = await insights.text();
    expect(insightsHtml.includes("iceccd insights")).toBeTruthy();
});
