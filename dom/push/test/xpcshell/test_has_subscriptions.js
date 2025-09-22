/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { PushService } = ChromeUtils.importESModule(
  "resource://gre/modules/PushService.sys.mjs"
);
const { PushServiceWebSocket } = ChromeUtils.importESModule(
  "resource://gre/modules/PushServiceWebSocket.sys.mjs"
);

add_task(async function test_has_subscriptions_queries_storage() {
  const db = PushServiceWebSocket.newPushDB();

  registerCleanupFunction(async () => {
    try {
      await db.drop();
    } catch (err) {
      // Ignore cleanup errors.
    }
    db.close();
  });

  await db.drop();

  Assert.equal(
    await PushService.hasSubscriptions(),
    false,
    "Push service should report no subscriptions when storage is empty"
  );

  await db.put({
    channelID: "f2a17a62-5f04-4d74-8f77-6b13a362f3bc",
    pushEndpoint: "https://example.com/push/endpoint",
    scope: "https://example.com/app",
    originAttributes: "",
    version: 1,
    pushCount: 0,
    lastPush: 0,
    quota: 16,
  });

  Assert.equal(
    await PushService.hasSubscriptions(),
    true,
    "Push service should report subscriptions when a record exists"
  );

  await db.drop();
});
