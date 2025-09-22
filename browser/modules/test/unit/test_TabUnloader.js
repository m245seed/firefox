/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */
"use strict";

const { TabUnloader, TabUnloaderTestSupport } = ChromeUtils.importESModule(
  "moz-src:///browser/components/tabbrowser/TabUnloader.sys.mjs"
);

let TestTabUnloaderMethods = {
  isNonDiscardable(tab, weight) {
    return /\bselected\b/.test(tab.keywords) ? weight : 0;
  },

  isParentProcess(tab, weight) {
    return /\bparent\b/.test(tab.keywords) ? weight : 0;
  },

  isPinned(tab, weight) {
    return /\bpinned\b/.test(tab.keywords) ? weight : 0;
  },

  isLoading(tab, weight) {
    return /\bloading\b/.test(tab.keywords) ? weight : 0;
  },

  usingPictureInPicture(tab, weight) {
    return /\bpictureinpicture\b/.test(tab.keywords) ? weight : 0;
  },

  playingMedia(tab, weight) {
    return /\bmedia\b/.test(tab.keywords) ? weight : 0;
  },

  usingWebRTC(tab, weight) {
    return /\bwebrtc\b/.test(tab.keywords) ? weight : 0;
  },

  isPrivate(tab, weight) {
    return /\bprivate\b/.test(tab.keywords) ? weight : 0;
  },

  getMinTabCount() {
    // Use a low number for testing.
    return 3;
  },

  getNow() {
    return 100;
  },

  *iterateProcesses(tab) {
    for (let process of tab.process.split(",")) {
      yield Number(process);
    }
  },

  async calculateMemoryUsage(processMap, tabs) {
    let memory = tabs[0].memory;
    for (let pid of processMap.keys()) {
      processMap.get(pid).memory = memory ? memory[pid - 1] : 1;
    }
  },
};

let unloadTests = [
  // Each item in the array represents one test. The test is a subarray
  // containing an element per tab. This is a string of keywords that
  // identify which criteria apply. The first part of the string may contain
  // a number that represents the last visit time, where higher numbers
  // are later. The last element in the subarray is special and identifies
  // the expected order of the tabs sorted by weight. The first tab in
  // this list is the one that is expected to selected to be discarded.
  { tabs: ["1 selected", "2", "3"], result: "1,2,0" },
  { tabs: ["1", "2 selected", "3"], result: "0,2,1" },
  { tabs: ["1 selected", "2", "3"], process: ["1", "2", "3"], result: "1,2,0" },
  {
    tabs: ["1 selected", "2 selected", "3 selected"],
    process: ["1", "2", "3"],
    result: "0,1,2",
  },
  {
    tabs: ["1 selected", "2", "3"],
    process: ["1,2,3", "2", "3"],
    result: "1,2,0",
  },
  {
    tabs: ["9", "8", "6", "5 selected", "2", "3", "4", "1"],
    result: "7,4,5,6,2,1,0,3",
  },
  {
    tabs: ["9", "8 pinned", "6", "5 selected", "2", "3 pinned", "4", "1"],
    result: "7,4,6,2,0,5,1,3",
  },
  {
    tabs: [
      "9",
      "8 pinned",
      "6",
      "5 selected pinned",
      "2",
      "3 pinned",
      "4",
      "1",
    ],
    result: "7,4,6,2,0,5,1,3",
  },
  {
    tabs: [
      "9",
      "8 pinned",
      "6",
      "5 selected pinned",
      "2",
      "3 selected pinned",
      "4",
      "1",
    ],
    result: "7,4,6,2,0,1,5,3",
  },
  {
    tabs: ["1", "2 selected", "3", "4 media", "5", "6"],
    result: "0,2,4,5,1,3",
  },
  {
    tabs: ["1 media", "2 selected media", "3", "4 media", "5", "6"],
    result: "2,4,5,0,3,1",
  },
  {
    tabs: ["1 media", "2 media pinned", "3", "4 media", "5 pinned", "6"],
    result: "2,5,4,0,3,1",
  },
  {
    tabs: [
      "1 media",
      "2 media pinned",
      "3",
      "4 media",
      "5 media pinned",
      "6 selected",
    ],
    result: "2,0,3,5,1,4",
  },
  {
    tabs: [
      "10 selected",
      "20 private",
      "30 webrtc",
      "40 pictureinpicture",
      "50 loading pinned",
      "60",
    ],
    result: "5,4,0,1,2,3",
  },
  {
    // Since TestTabUnloaderMethods.getNow() returns 100 and the test
    // passes minInactiveDuration = 0 to TabUnloader.getSortedTabs(),
    // tab 200 and 300 are excluded from the result.
    tabs: ["300", "10", "50", "100", "200"],
    result: "1,2,3",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6"],
    process: ["1", "2", "1", "1", "1", "1"],
    result: "1,0,2,3,4,5",
  },
  {
    tabs: ["1", "2 selected", "3", "4", "5", "6"],
    process: ["1", "2", "1", "1", "1", "1"],
    result: "0,2,3,4,5,1",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6"],
    process: ["1", "2", "2", "1", "1", "1"],
    result: "0,1,2,3,4,5",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6"],
    process: ["1", "2", "3", "1", "1", "1"],
    result: "1,0,2,3,4,5",
  },
  {
    tabs: ["1", "2 media", "3", "4", "5", "6"],
    process: ["1", "2", "3", "1", "1", "1"],
    result: "2,0,3,4,5,1",
  },
  {
    tabs: ["1", "2 media", "3", "4", "5", "6"],
    process: ["1", "2", "3", "1", "1,2,3", "1"],
    result: "0,2,3,4,5,1",
  },
  {
    tabs: ["1", "2 media", "3", "4", "5", "6"],
    process: ["1", "2", "3", "1", "1,4,5", "1"],
    result: "2,0,3,4,5,1",
  },
  {
    tabs: ["1", "2 media", "3 media", "4", "5 media", "6"],
    process: ["1", "2", "3", "1", "1,4,5", "1"],
    result: "0,3,5,1,2,4",
  },
  {
    tabs: ["1", "2 media", "3 media", "4", "5 media", "6"],
    process: ["1", "1", "3", "1", "1,4,5", "1"],
    result: "0,3,5,1,2,4",
  },
  {
    tabs: ["1", "2 media", "3 media", "4", "5 media", "6"],
    process: ["1", "2", "3", "4", "1,4,5", "5"],
    result: "0,3,5,1,2,4",
  },
  {
    tabs: ["1", "2 media", "3 media", "4", "5 media", "6"],
    process: ["1", "1", "3", "4", "1,4,5", "5"],
    result: "0,3,5,1,2,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6"],
    process: ["1", "1", "1", "2", "1,3,4,5,6,7,8", "1"],
    result: "0,1,2,3,4,5",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8"],
    process: ["1", "1", "1", "2", "1,3,4,5,6,7,8", "1", "1", "1"],
    result: "4,0,3,1,2,5,6,7",
  },
  {
    tabs: ["1", "2", "3", "4", "5 selected", "6"],
    process: ["1", "1", "1", "2", "1,3,4,5,6,7,8", "1"],
    result: "0,1,2,3,5,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6"],
    process: ["1", "1", "1", "2", "1,3,4,5,6,7,8", "1"],
    result: "0,1,2,3,5,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1", "1", "1", "2", "1,3,4,5,6,7,8", "1", "1", "1"],
    result: "0,3,1,2,5,6,7,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1", "1,3,4,5,6,7,8", "1", "1", "1", "1", "1", "1"],
    result: "1,0,2,3,5,6,7,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1", "1", "1,3,4,5,6,7,8", "1", "1", "1", "1", "1"],
    result: "2,0,1,3,5,6,7,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1", "1", "1,1,1,1,1,1,1", "1", "1", "1", "1,1,1,1,1", "1"],
    result: "0,1,2,3,5,6,7,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1", "1", "1,2,3,4,5", "1", "1", "1", "1,2,3,4,5", "1"],
    result: "0,1,2,3,5,6,7,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1", "1", "1,6", "1", "1", "1", "1,2,3,4,5", "1"],
    result: "0,2,1,3,5,6,7,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1", "1", "1,6", "1,7", "1,8", "1,9", "1,2,3,4,5", "1"],
    result: "2,3,0,5,1,6,7,4",
  },
  {
    tabs: ["1", "2", "3", "4", "5 media", "6", "7", "8"],
    process: ["1,10,11", "1", "1,2", "1,7", "1,8", "1,9", "1,2,3,4,5", "1"],
    result: "0,3,1,5,2,6,7,4",
  },
  {
    tabs: [
      "1 media",
      "2 media",
      "3 media",
      "4 media",
      "5 media",
      "6",
      "7",
      "8",
    ],
    process: ["1,10,11", "1", "1,2", "1,7", "1,8", "1,9", "1,2,3,4,5", "1"],
    result: "6,5,7,0,1,2,3,4",
  },
  {
    tabs: ["1", "2", "3"],
    process: ["1", "2", "3"],
    memory: ["100", "200", "300"],
    result: "0,1,2",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
    process: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
    memory: [
      "100",
      "200",
      "300",
      "400",
      "500",
      "600",
      "700",
      "800",
      "900",
      "1000",
    ],
    result: "0,1,2,3,4,5,6,7,8,9",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
    process: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
    memory: [
      "100",
      "900",
      "300",
      "500",
      "400",
      "700",
      "600",
      "1000",
      "200",
      "200",
    ],
    result: "1,0,2,3,5,4,6,7,8,9",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
    process: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
    memory: [
      "1000",
      "900",
      "300",
      "500",
      "400",
      "1000",
      "600",
      "1000",
      "200",
      "200",
    ],
    result: "0,1,2,3,5,4,6,7,8,9",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6"],
    process: ["1", "2,7", "3", "4", "5", "6"],
    memory: ["100", "200", "300", "400", "500", "600", "700"],
    result: "1,0,2,3,4,5",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8"],
    process: ["1,6", "2,7", "3,8", "4,1,2", "5", "6", "7", "8"],
    memory: ["100", "200", "300", "400", "500", "600", "700", "800"],
    result: "2,3,0,1,4,5,6,7",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8"],
    process: ["1", "1", "1", "2", "1", "1", "1", "1"],
    memory: ["700", "1000"],
    result: "0,3,1,2,4,5,6,7",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8"],
    process: ["1", "1", "1", "1", "2,1", "2,1", "3", "3"],
    memory: ["1000", "2000", "3000"],
    result: "0,1,2,4,3,5,6,7",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8"],
    process: ["2", "2", "2", "2", "2,1", "2,1", "3", "3"],
    memory: ["1000", "600", "1000"],
    result: "0,1,2,4,3,5,6,7",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8"],
    process: ["1", "1", "1", "2", "2,1,1,1", "2,1", "3", "3"],
    memory: ["1000", "1800", "1000"],
    result: "0,1,3,2,4,5,6,7",
  },
  {
    tabs: ["1", "2", "3", "4", "5", "6", "7", "8"],
    process: ["1", "1", "1", "2", "2,1,1,1", "2,1", "3", "3"],
    memory: ["4000", "1800", "1000"],
    result: "0,1,2,4,3,5,6,7",
  },
  {
    // The tab "1" contains 4 frames, but its uniqueCount is 1 because
    // all of those frames are backed by the process "1".  As a result,
    // TabUnloader puts the tab "1" first based on the last access time.
    tabs: ["1", "2", "3", "4", "5"],
    process: ["1,1,1,1", "2", "3", "3", "3"],
    memory: ["100", "100", "100"],
    result: "0,1,2,3,4",
  },
  {
    // The uniqueCount of the tab "1", "2", and "3" is 1, 2, and 3,
    // respectively.  As a result the first three tabs are sorted as 2,1,0.
    tabs: ["1", "2", "3", "4", "5", "6"],
    process: ["1,7,1,7,1,1,7,1", "7,3,7,2", "4,5,7,4,6,7", "7", "7", "7"],
    memory: ["100", "100", "100", "100", "100", "100", "100"],
    result: "2,1,0,3,4,5",
  },
];

let globalBrowser = {
  discardBrowser() {
    return true;
  },
};

add_task(function test_resource_sort_equivalence() {
  const dataset = [
    { id: "A", uniqueCount: 3, memory: 100, lastAccessed: 50, ordinal: 1 },
    { id: "B", uniqueCount: 3, memory: 90, lastAccessed: 40, ordinal: 2 },
    { id: "C", uniqueCount: 2, memory: 200, lastAccessed: 30, ordinal: 3 },
    { id: "D", uniqueCount: 2, memory: 200, lastAccessed: 20, ordinal: 4 },
    { id: "E", uniqueCount: 1, memory: 300, lastAccessed: 10, ordinal: 5 },
    { id: "F", uniqueCount: 1, memory: 50, lastAccessed: 60, ordinal: 6 },
  ];

  function legacyOrder(records) {
    const working = records.map(record => ({
      id: record.id,
      uniqueCount: record.uniqueCount,
      memory: record.memory,
      lastAccessed: record.lastAccessed,
      sortWeight: record.ordinal,
    }));

    working.sort((a, b) => b.uniqueCount - a.uniqueCount);
    let sortWeight = 0;
    for (const record of working) {
      record.sortWeight += ++sortWeight;
      if (record.uniqueCount > 1) {
        record.sortWeight -= record.uniqueCount - 1;
      }
    }

    working.sort((a, b) => b.memory - a.memory);
    sortWeight = 0;
    for (const record of working) {
      record.sortWeight += ++sortWeight;
    }

    working.sort((a, b) => {
      if (a.sortWeight != b.sortWeight) {
        return a.sortWeight - b.sortWeight;
      }
      return a.lastAccessed - b.lastAccessed;
    });

    return working.map(record => record.id);
  }

  function modernOrder(records) {
    const working = records.map(record => ({
      id: record.id,
      uniqueCount: record.uniqueCount,
      memory: record.memory,
      lastAccessed: record.lastAccessed,
      ordinal: record.ordinal,
      weight: 0,
    }));

    const uniqueFrequency = new Map();
    for (const record of working) {
      uniqueFrequency.set(
        record.uniqueCount,
        (uniqueFrequency.get(record.uniqueCount) || 0) + 1
      );
    }

    const uniqueRanks = new Map();
    let runningUnique = 0;
    for (const value of [...uniqueFrequency.keys()].sort((a, b) => b - a)) {
      uniqueRanks.set(value, runningUnique);
      runningUnique += uniqueFrequency.get(value);
    }

    const uniqueSeen = new Map();
    const recordsByUniqueRank = new Array(working.length);
    for (const record of working) {
      const seen = uniqueSeen.get(record.uniqueCount) || 0;
      const rank = 1 + (uniqueRanks.get(record.uniqueCount) || 0) + seen;
      uniqueSeen.set(record.uniqueCount, seen + 1);
      record.uniqueRank = rank;
      recordsByUniqueRank[rank - 1] = record;
    }

    const memoryFrequency = new Map();
    for (const record of working) {
      memoryFrequency.set(
        record.memory,
        (memoryFrequency.get(record.memory) || 0) + 1
      );
    }

    const memoryRanks = new Map();
    let runningMemory = 0;
    for (const value of [...memoryFrequency.keys()].sort((a, b) => b - a)) {
      memoryRanks.set(value, runningMemory);
      runningMemory += memoryFrequency.get(value);
    }

    const memorySeen = new Map();
    for (const record of recordsByUniqueRank) {
      const seen = memorySeen.get(record.memory) || 0;
      const base = memoryRanks.get(record.memory) || 0;
      record.memoryRank = 1 + base + seen;
      memorySeen.set(record.memory, seen + 1);
    }

    for (const record of working) {
      const penalty = record.uniqueCount > 1 ? record.uniqueCount - 1 : 0;
      record.weight =
        record.ordinal + record.uniqueRank + record.memoryRank - penalty;
    }

    working.sort((a, b) =>
      TabUnloaderTestSupport.compareTabResourceRecordsForTests(a, b)
    );

    return working.map(record => record.id);
  }

  Assert.deepEqual(
    modernOrder(dataset),
    legacyOrder(dataset),
    "Resource comparator preserves legacy ordering"
  );
});

add_task(async function doTests() {
  for (let test of unloadTests) {
    function* iterateTabs() {
      let tabs = test.tabs;
      for (let t = 0; t < tabs.length; t++) {
        let tab = {
          tab: {
            originalIndex: t,
            lastAccessed: Number(/^[0-9]+/.exec(tabs[t])[0]),
            keywords: tabs[t],
            process: "process" in test ? test.process[t] : "1",
          },
          memory: test.memory,
          gBrowser: globalBrowser,
        };
        yield tab;
      }
    }
    TestTabUnloaderMethods.iterateTabs = iterateTabs;

    let expectedOrder = "";
    const sortedTabs = await TabUnloader.getSortedTabs(
      0,
      TestTabUnloaderMethods
    );
    for (let tab of sortedTabs) {
      if (expectedOrder) {
        expectedOrder += ",";
      }
      expectedOrder += tab.tab.originalIndex;
    }

    Assert.equal(expectedOrder, test.result);
  }
});

add_task(async function test_adaptive_inactive_duration() {
  const originalIterateTabs = TestTabUnloaderMethods.iterateTabs;

  const browserStub = {
    preparedTabs: [],
    discardedTabs: [],
    async prepareDiscardBrowser(tab) {
      this.preparedTabs.push(tab);
    },
    discardBrowser(tab) {
      this.discardedTabs.push(tab);
      return true;
    },
  };

  function createTab(lastAccessed, id) {
    return {
      tab: {
        lastAccessed,
        keywords: id,
        process: "1",
        updateLastUnloadedByTabUnloader() {},
      },
      gBrowser: browserStub,
    };
  }

  function* iterateTabs() {
    yield createTab(90, "very-fresh");
    yield createTab(75, "fresh-but-eligible-later");
  }

  TestTabUnloaderMethods.iterateTabs = iterateTabs;

  TabUnloader._resetAdaptiveState();
  const minInactiveDuration = 40;

  let stats = {};
  let didUnload = await TabUnloader.unloadLeastRecentlyUsedTab(
    minInactiveDuration,
    TestTabUnloaderMethods,
    stats
  );
  Assert.ok(
    !didUnload,
    "No tab unloaded while only fresh tabs were available."
  );
  Assert.ok(
    stats.skippedFreshDiscardable,
    "Detected that fresh discardable tabs were skipped."
  );
  Assert.ok(
    !stats.hadDiscardableCandidate,
    "No discardable candidates were returned while tabs were still fresh."
  );
  Assert.equal(
    TabUnloader._adaptiveMinInactiveDuration,
    Math.floor(minInactiveDuration / 2),
    "Adaptive threshold should halve after a failed attempt."
  );

  browserStub.discardedTabs.length = 0;
  stats = {};
  didUnload = await TabUnloader.unloadLeastRecentlyUsedTab(
    minInactiveDuration,
    TestTabUnloaderMethods,
    stats
  );
  Assert.ok(didUnload, "Tab becomes unloadable after threshold relaxes.");
  Assert.equal(
    browserStub.discardedTabs[browserStub.discardedTabs.length - 1]?.keywords,
    "fresh-but-eligible-later",
    "The previously fresh tab was discarded."
  );

  TabUnloader.observe(null, "memory-pressure-stop");
  Assert.strictEqual(
    TabUnloader._adaptiveMinInactiveDuration,
    null,
    "Adaptive state resets after memory pressure ends."
  );
  Assert.ok(
    !TabUnloader._pressureEpisodeActive,
    "Pressure flag cleared after recovery."
  );

  TestTabUnloaderMethods.iterateTabs = originalIterateTabs;
  TabUnloader._resetAdaptiveState();
});

add_task(async function test_adaptive_relaxation_with_undiscardable_tabs() {
  const originalIterateTabs = TestTabUnloaderMethods.iterateTabs;

  const browserStub = {
    async prepareDiscardBrowser() {},
    discardBrowser() {
      return true;
    },
  };

  function createTab(lastAccessed, keywords) {
    return {
      tab: {
        lastAccessed,
        keywords,
        process: "1",
        updateLastUnloadedByTabUnloader() {},
      },
      gBrowser: browserStub,
    };
  }

  function* iterateTabsWithSelected() {
    yield createTab(90, "fresh-candidate");
    yield createTab(10, "selected anchor");
  }

  function* iterateTabsAfterRelax() {
    yield createTab(10, "fresh-candidate");
    yield createTab(5, "selected anchor");
  }

  try {
    TestTabUnloaderMethods.iterateTabs = iterateTabsWithSelected;

    TabUnloader._resetAdaptiveState();
    const minInactiveDuration = 40;

    let stats = {};
    let didUnload = await TabUnloader.unloadLeastRecentlyUsedTab(
      minInactiveDuration,
      TestTabUnloaderMethods,
      stats
    );

    Assert.ok(
      !didUnload,
      "Unload attempt fails while only undiscardable tabs remain."
    );
    Assert.ok(
      stats.skippedFreshDiscardable,
      "Fresh discardable tab was skipped despite an undiscardable tab being present."
    );
    Assert.ok(
      !stats.hadDiscardableCandidate,
      "No discardable candidates were available once filtering completed."
    );
    Assert.equal(
      TabUnloader._adaptiveMinInactiveDuration,
      Math.floor(minInactiveDuration / 2),
      "Adaptive threshold halves even when only undiscardable tabs remain."
    );

    TestTabUnloaderMethods.iterateTabs = iterateTabsAfterRelax;

    stats = {};
    didUnload = await TabUnloader.unloadLeastRecentlyUsedTab(
      minInactiveDuration,
      TestTabUnloaderMethods,
      stats
    );
    Assert.ok(
      didUnload,
      "Previously fresh tab becomes discardable after adaptive relaxation."
    );
  } finally {
    TestTabUnloaderMethods.iterateTabs = originalIterateTabs;
    TabUnloader._resetAdaptiveState();
  }
});

add_task(async function test_no_relax_when_candidate_remains() {
  const originalIterateTabs = TestTabUnloaderMethods.iterateTabs;

  const browserStub = {
    async prepareDiscardBrowser() {},
    discardBrowser() {
      return false;
    },
  };

  function createTab(lastAccessed, keywords) {
    return {
      tab: {
        lastAccessed,
        keywords,
        process: "1",
        updateLastUnloadedByTabUnloader() {},
      },
      gBrowser: browserStub,
    };
  }

  function* iterateTabs() {
    yield createTab(50, "eligible");
    yield createTab(95, "fresh-candidate");
  }

  try {
    TestTabUnloaderMethods.iterateTabs = iterateTabs;

    TabUnloader._resetAdaptiveState();
    const minInactiveDuration = 40;

    let stats = {};
    const didUnload = await TabUnloader.unloadLeastRecentlyUsedTab(
      minInactiveDuration,
      TestTabUnloaderMethods,
      stats
    );

    Assert.ok(
      !didUnload,
      "Unload attempt fails because the browser refuses to discard the tab."
    );
    Assert.ok(
      stats.skippedFreshDiscardable,
      "Fresh discardable tab was skipped during the attempt."
    );
    Assert.ok(
      stats.hadDiscardableCandidate,
      "A discardable candidate was present in the result set."
    );
    Assert.strictEqual(
      TabUnloader._adaptiveMinInactiveDuration,
      null,
      "Adaptive threshold is not relaxed when candidates were available."
    );
  } finally {
    TestTabUnloaderMethods.iterateTabs = originalIterateTabs;
    TabUnloader._resetAdaptiveState();
  }
});

add_task(async function test_proc_info_caching_within_episode() {
  const originalIterateTabs = TestTabUnloaderMethods.iterateTabs;
  const originalRequestProcInfo = ChromeUtils.requestProcInfo;

  const tabLabels = ["1", "2", "3"];
  const processes = ["1", "2", "3"];
  const memory = [100, 200, 300];

  function* iterateTabs() {
    for (let index = 0; index < tabLabels.length; index++) {
      yield {
        tab: {
          originalIndex: index,
          lastAccessed: index,
          keywords: tabLabels[index],
          process: processes[index],
        },
        memory,
        gBrowser: globalBrowser,
      };
    }
  }

  let callCount = 0;
  ChromeUtils.requestProcInfo = async () => {
    callCount++;
    return {
      children: [
        { pid: 1, memory: 100 },
        { pid: 2, memory: 200 },
        { pid: 3, memory: 300 },
      ],
    };
  };

  try {
    TestTabUnloaderMethods.iterateTabs = iterateTabs;

    TabUnloader._resetAdaptiveState();
    TabUnloader.observe(null, "memory-pressure", "low-memory");

    await TabUnloader.getSortedTabs(0, TestTabUnloaderMethods);
    await TabUnloader.getSortedTabs(0, TestTabUnloaderMethods);

    Assert.equal(
      callCount,
      1,
      "Proc info snapshot reused across back-to-back passes"
    );
  } finally {
    ChromeUtils.requestProcInfo = originalRequestProcInfo;
    TestTabUnloaderMethods.iterateTabs = originalIterateTabs;
    TabUnloader._resetAdaptiveState();
  }
});
