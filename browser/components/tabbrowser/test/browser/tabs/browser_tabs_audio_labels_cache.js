/* Any copyright is dedicated to the Public Domain.
   https://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

add_task(async function test_tab_audio_label_messages_cached() {
  const tab = await addMediaTab();
  await play(tab);

  const tabContainer = gBrowser.tabContainer;
  const originalFormatMessages =
    gBrowser.tabLocalization.formatMessagesSync;
  let callCount = 0;
  gBrowser.tabLocalization.formatMessagesSync = function (ids) {
    callCount++;
    return originalFormatMessages.call(this, ids);
  };

  try {
    ok(tab.audioButton, "The media tab should expose an audio button");

    Services.obs.notifyObservers(null, "intl:app-locales-changed");
    callCount = 0;
    tabContainer.updateTabSoundLabel(tab);
    is(
      callCount,
      1,
      "Formatting messages should occur once to populate the cache"
    );

    tabContainer.updateTabSoundLabel(tab);
    is(
      callCount,
      1,
      "Subsequent updates should reuse cached audio labels"
    );

    Services.obs.notifyObservers(null, "intl:app-locales-changed");
    tabContainer.updateTabSoundLabel(tab);
    is(
      callCount,
      2,
      "Locale changes should invalidate the cache and refetch messages"
    );
  } finally {
    gBrowser.tabLocalization.formatMessagesSync = originalFormatMessages;
    await BrowserTestUtils.removeTab(tab);
  }
});
