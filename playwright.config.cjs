const { defineConfig } = require('@playwright/test');
const fs = require('fs');

// Use a system Chromium when there is one, otherwise Playwright's own download.
const systemChromium = process.env.CHROMIUM_PATH || ['/usr/bin/chromium', '/usr/bin/chromium-browser'].find(p => fs.existsSync(p));

module.exports = defineConfig({
  testDir: './tests',
  testMatch: /.*\.spec\.cjs/,
  timeout: 60000,
  workers: 1,
  reporter: 'list',
  use: {
    browserName: 'chromium',
    headless: true,
    viewport: { width: 1440, height: 1000 },
    launchOptions: systemChromium ? { executablePath: systemChromium } : {},
  },
});
