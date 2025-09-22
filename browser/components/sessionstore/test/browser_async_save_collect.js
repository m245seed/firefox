/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { BrowserTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/BrowserTestUtils.sys.mjs"
);
const { SessionStore } = ChromeUtils.importESModule(
  "resource:///modules/sessionstore/SessionStore.sys.mjs"
);
const { TabStateFlusher } = ChromeUtils.importESModule(
  "resource:///modules/sessionstore/TabStateFlusher.sys.mjs"
);

add_task(async function setup() {
  registerCleanupFunction(() => {
    if (
      Services.prefs.prefHasUserValue(
        "browser.sessionstore.saver.asyncWindowChunk"
      )
    ) {
      Services.prefs.clearUserPref(
        "browser.sessionstore.saver.asyncWindowChunk"
      );
    }
  });
});

add_task(async function test_async_collection_matches_sync() {
  Services.prefs.setIntPref(
    "browser.sessionstore.saver.asyncWindowChunk",
    1
  );

  let win = await BrowserTestUtils.openNewBrowserWindow();
  registerCleanupFunction(() => BrowserTestUtils.closeWindow(win));

  // Open a handful of tabs so we have a reasonably sized session state.
  for (let i = 0; i < 5; i++) {
    let tab = await BrowserTestUtils.openNewForegroundTab(
      win.gBrowser,
      "about:blank",
      false
    );
    await TabStateFlusher.flush(tab.linkedBrowser);
  }
  await TabStateFlusher.flush(win.gBrowser.selectedBrowser);

  let realDispatch = Services.tm.dispatchToMainThread;
  let dispatchCount = 0;
  Services.tm.dispatchToMainThread = function (callback) {
    dispatchCount++;
    return realDispatch.call(Services.tm, callback);
  };

  let asyncState;
  try {
    asyncState = await SessionStore.getCurrentStateAsync(true, {
      chunkSize: 1,
    });
  } finally {
    Services.tm.dispatchToMainThread = realDispatch;
  }

  let syncState = SessionStore.getCurrentState(true);

  Assert.equal(
    asyncState.windows.length,
    syncState.windows.length,
    "Window counts should match between async and sync state collection"
  );

  for (let i = 0; i < syncState.windows.length; i++) {
    Assert.equal(
      asyncState.windows[i].tabs.length,
      syncState.windows[i].tabs.length,
      `Tab count for window ${i} should match`
    );
  }

  Assert.greater(
    dispatchCount,
    0,
    "Async state collection should yield to the main thread at least once"
  );
});
