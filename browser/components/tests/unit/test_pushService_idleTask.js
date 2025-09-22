/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { MockRegistrar } = ChromeUtils.importESModule(
  "resource://testing-common/MockRegistrar.sys.mjs"
);

function registerMockPushService(state) {
  function MockPushService() {
    this.wrappedJSObject = this;
  }

  MockPushService.prototype = {
    QueryInterface: ChromeUtils.generateQI([
      "nsIPushService",
      "nsIPushQuotaManager",
      "nsIPushErrorReporter",
      "nsIObserver",
      "nsISupportsWeakReference",
    ]),

    pushTopic: "",
    subscriptionChangeTopic: "",
    subscriptionModifiedTopic: "",

    ensureReady() {
      state.ensureReadyCalls++;
    },

    async hasSubscriptions() {
      state.hasSubscriptionsCalls++;
      return state.hasSubscriptions;
    },

    subscribe() {},
    subscribeWithKey() {},
    unsubscribe() {},
    getSubscription() {},
    clearForDomain() {},
    clearForPrincipal() {},
    notificationForOriginShown() {},
    notificationForOriginClosed() {},
    reportDeliveryError() {},
    observe() {},
  };

  return MockRegistrar.register("@mozilla.org/push/Service;1", MockPushService);
}

add_task(async function test_push_idle_task_respects_subscription_state() {
  const state = {
    hasSubscriptions: false,
    hasSubscriptionsCalls: 0,
    ensureReadyCalls: 0,
  };

  const cid = registerMockPushService(state);
  registerCleanupFunction(() => {
    MockRegistrar.unregister(cid);
  });

  const { BrowserGlue } = ChromeUtils.importESModule(
    "resource:///modules/BrowserGlue.sys.mjs"
  );

  const browserGlue = new BrowserGlue();

  await browserGlue._maybeEnsurePushServiceReady();

  Assert.equal(
    state.hasSubscriptionsCalls,
    1,
    "Should check for push subscriptions"
  );
  Assert.equal(
    state.ensureReadyCalls,
    0,
    "Should not initialize push when there are no subscriptions"
  );

  state.hasSubscriptions = true;
  await browserGlue._maybeEnsurePushServiceReady();

  Assert.equal(
    state.ensureReadyCalls,
    1,
    "Should initialize push when subscriptions are present"
  );

  Assert.equal(
    state.hasSubscriptionsCalls,
    2,
    "Should re-check for subscriptions on subsequent runs"
  );
});
