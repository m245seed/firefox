"use strict";

const { SessionStore, _SessionStoreInternal } = ChromeUtils.importESModule(
  "resource:///modules/sessionstore/SessionStore.sys.mjs"
);

function makeEnumerator(windows) {
  let index = 0;
  return {
    next() {
      if (index < windows.length) {
        return { value: windows[index++], done: false };
      }
      return { value: undefined, done: true };
    },
    [Symbol.iterator]() {
      return this;
    },
    hasMoreElements() {
      return index < windows.length;
    },
    getNext() {
      if (!this.hasMoreElements()) {
        throw new Error("No more elements");
      }
      return windows[index++];
    },
  };
}

function withMockedSessionStore({ closedWindows, windowStates }, callback) {
  const originalClosedWindows = _SessionStoreInternal._closedWindows;
  const originalWindows = _SessionStoreInternal._windows;
  const originalGetEnumerator = Services.wm.getEnumerator;

  try {
    _SessionStoreInternal._closedWindows = closedWindows;
    _SessionStoreInternal._windows = windowStates;
    Services.wm.getEnumerator = () =>
      makeEnumerator(
        Object.keys(windowStates).map(id => ({ __SSi: id }))
      );

    callback();
  } finally {
    _SessionStoreInternal._closedWindows = originalClosedWindows;
    _SessionStoreInternal._windows = originalWindows;
    Services.wm.getEnumerator = originalGetEnumerator;
  }
}

add_task(function test_lastClosedObjectType_prefersWindowWhenNoClosedTabs() {
  const closedWindows = [{ closedAt: 50 }];
  const windowStates = Object.create(null);
  windowStates.mockWindow = { _closedTabs: [] };

  withMockedSessionStore({ closedWindows, windowStates }, () => {
    Assert.equal(
      SessionStore.lastClosedObjectType,
      "window",
      "Should report a window when no closed tabs are tracked"
    );
  });
});

add_task(function test_lastClosedObjectType_considersLatestClosedTab() {
  const closedWindows = [{ closedAt: 10 }];
  const windowStates = Object.create(null);
  windowStates.first = { _closedTabs: [{ closedAt: 5 }] };
  windowStates.second = { _closedTabs: [{ closedAt: 20 }] };

  withMockedSessionStore({ closedWindows, windowStates }, () => {
    Assert.equal(
      SessionStore.lastClosedObjectType,
      "tab",
      "Should report a tab when its timestamp is newest"
    );

    closedWindows[0].closedAt = 25;
    Assert.equal(
      SessionStore.lastClosedObjectType,
      "window",
      "Should report a window when it is newer than the closed tabs"
    );
  });
});

