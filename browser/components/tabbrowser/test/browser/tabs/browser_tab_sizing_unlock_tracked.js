/* Any copyright is dedicated to the Public Domain.
   https://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

add_task(async function test_unlock_tab_sizing_resets_tracked_tabs_only() {
  const pinnedTab = await addTab("about:blank");
  gBrowser.pinTab(pinnedTab);
  pinnedTab.style.maxWidth = "42px";

  const tab1 = await addTab("about:blank");
  const tab2 = await addTab("about:blank");

  const tabContainer = gBrowser.tabContainer;
  tabContainer._lockTabSizing(tab2);

  ok(
    tabContainer._lockedWidthTabs.size,
    "Locking tab sizing should record the tabs whose widths were modified"
  );

  tabContainer._unlockTabSizing();

  is(
    pinnedTab.style.maxWidth,
    "42px",
    "Pinned tabs outside the lock set should retain their max-width"
  );
  ok(
    !tab1.style.maxWidth && !tab2.style.maxWidth,
    "Tabs tracked during locking should have their widths reset"
  );
  is(
    tabContainer._lockedWidthTabs.size,
    0,
    "Unlocking should clear the internal tracking set"
  );

  await BrowserTestUtils.removeTab(tab2);
  await BrowserTestUtils.removeTab(tab1);
  gBrowser.unpinTab(pinnedTab);
  await BrowserTestUtils.removeTab(pinnedTab);
});
