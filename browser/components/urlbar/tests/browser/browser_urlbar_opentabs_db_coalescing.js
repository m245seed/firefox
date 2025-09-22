/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { UrlbarProviderOpenTabs } = ChromeUtils.importESModule(
  "moz-src:///browser/components/urlbar/UrlbarProviderOpenTabs.sys.mjs"
);

add_task(async function test_coalesced_db_writes() {
  const TEST_URL = `https://example.com/${Math.random().toString(16).slice(2)}`;

  await UrlbarProviderOpenTabs.promiseDBPopulated;
  await UrlbarProviderOpenTabs.flushPendingMemoryTableUpdatesForTests();

  const conn = await PlacesUtils.promiseLargeCacheDBConnection();
  const originalExecuteCached = conn.executeCached;
  let writeCount = 0;
  conn.executeCached = function (sql, params, callback) {
    if (sql.includes("moz_openpages_temp")) {
      writeCount++;
    }
    return originalExecuteCached.call(this, sql, params, callback);
  };
  registerCleanupFunction(async () => {
    conn.executeCached = originalExecuteCached;
    const rows = await UrlbarProviderOpenTabs.getDatabaseRegisteredOpenTabsForTests();
    for (const row of rows) {
      if (row.url !== TEST_URL) {
        continue;
      }
      for (let i = 0; i < row.count; i++) {
        UrlbarProviderOpenTabs.unregisterOpenTab(
          row.url,
          row.userContextId,
          row.tabGroup,
          false
        );
      }
    }
    await UrlbarProviderOpenTabs.flushPendingMemoryTableUpdatesForTests();
  });

  UrlbarProviderOpenTabs.registerOpenTab(TEST_URL, 1, null, false);
  await UrlbarProviderOpenTabs.flushPendingMemoryTableUpdatesForTests();

  writeCount = 0;

  const registerCount = 50;
  const unregisterCount = 20;
  for (let i = 0; i < registerCount; i++) {
    UrlbarProviderOpenTabs.registerOpenTab(TEST_URL, 1, null, false);
  }
  for (let i = 0; i < unregisterCount; i++) {
    UrlbarProviderOpenTabs.unregisterOpenTab(TEST_URL, 1, null, false);
  }

  await UrlbarProviderOpenTabs.flushPendingMemoryTableUpdatesForTests();

  Assert.equal(
    writeCount,
    1,
    "Registering and unregistering many tabs in the same tick should be coalesced"
  );
});
