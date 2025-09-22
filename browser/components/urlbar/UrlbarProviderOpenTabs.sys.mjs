/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This module exports a provider, returning open tabs matches for the urlbar.
 * It is also used to register and unregister open tabs.
 */

import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";
import { Services } from "resource://gre/modules/Services.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  ProvidersManager:
    "moz-src:///browser/components/urlbar/UrlbarProvidersManager.sys.mjs",
  UrlbarResult: "moz-src:///browser/components/urlbar/UrlbarResult.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  UrlbarUtils.getLogger({ prefix: "Provider.OpenTabs" })
);

const PRIVATE_USER_CONTEXT_ID = -1;

/**
 * Maps the open tabs by userContextId, then by groupId.
 * It is a nested map structure as follows:
 *   Map(userContextId => Map(groupId | null => Map(url => count)))
 */
var gOpenTabUrls = new Map();

const MEMORY_TABLE_FLUSH_DELAY_MS = 200;
const MEMORY_TABLE_IDLE_TIMEOUT_MS = 1000;
const SQLITE_MAX_VARIABLES = 999;
const SQLITE_VARIABLES_PER_ENTRY = 4;
const MAX_UPDATES_PER_STATEMENT = Math.floor(
  SQLITE_MAX_VARIABLES / SQLITE_VARIABLES_PER_ENTRY
);

let gPendingMemoryTableDeltas = new Map();
let gMemoryTableFlushTask = null;
let gIdleFlushDispatched = false;
let gOngoingFlushPromise = null;

function ensureMemoryTableFlushTask() {
  if (!gMemoryTableFlushTask) {
    gMemoryTableFlushTask = new lazy.DeferredTask(
      () => flushPendingMemoryTableUpdates(),
      MEMORY_TABLE_FLUSH_DELAY_MS,
      MEMORY_TABLE_IDLE_TIMEOUT_MS
    );
  }
  return gMemoryTableFlushTask;
}

function scheduleIdleFlush() {
  if (gIdleFlushDispatched) {
    return;
  }
  gIdleFlushDispatched = true;
  Services.tm.idleDispatchToMainThread(async () => {
    gIdleFlushDispatched = false;
    await flushPendingMemoryTableUpdates();
  });
}

function scheduleMemoryTableFlush() {
  if (!UrlbarProviderOpenTabs.memoryTableInitialized) {
    return;
  }
  if (!gPendingMemoryTableDeltas.size) {
    if (gMemoryTableFlushTask?.isArmed) {
      gMemoryTableFlushTask.disarm();
    }
    return;
  }
  ensureMemoryTableFlushTask().arm();
  scheduleIdleFlush();
}

function getMemoryTableKey(url, userContextId, groupId) {
  return `${userContextId}\n${groupId ?? ""}\n${url}`;
}

function queueMemoryTableDelta(url, userContextId, groupId, delta) {
  if (!UrlbarProviderOpenTabs.memoryTableInitialized || delta === 0) {
    return;
  }

  let key = getMemoryTableKey(url, userContextId, groupId);
  let update = gPendingMemoryTableDeltas.get(key);
  let newDelta = (update?.delta ?? 0) + delta;
  if (newDelta === 0) {
    gPendingMemoryTableDeltas.delete(key);
  } else {
    gPendingMemoryTableDeltas.set(key, {
      url,
      userContextId,
      groupId,
      delta: newDelta,
    });
  }
  scheduleMemoryTableFlush();
}

async function flushPendingMemoryTableUpdates() {
  if (gOngoingFlushPromise) {
    return gOngoingFlushPromise;
  }
  if (
    !UrlbarProviderOpenTabs.memoryTableInitialized ||
    !gPendingMemoryTableDeltas.size
  ) {
    return;
  }

  let updatesToFlush = gPendingMemoryTableDeltas;
  gPendingMemoryTableDeltas = new Map();

  let flushPromise = (async () => {
    try {
      let updatesArray = Array.from(updatesToFlush.values());
      if (!updatesArray.length) {
        return;
      }
      await lazy.ProvidersManager.runInCriticalSection(async () => {
        let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
        for (let i = 0; i < updatesArray.length; i += MAX_UPDATES_PER_STATEMENT) {
          let chunk = updatesArray.slice(i, i + MAX_UPDATES_PER_STATEMENT);
          let valuesSql = chunk
            .map(
              (update, index) =>
                `(:url${index}, :userContextId${index}, IFNULL(:groupId${index}, ''), :delta${index})`
            )
            .join(", ");
          let params = {};
          for (let [index, update] of chunk.entries()) {
            params[`url${index}`] = update.url;
            params[`userContextId${index}`] = update.userContextId;
            params[`groupId${index}`] = update.groupId;
            params[`delta${index}`] = update.delta;
          }
          await conn.executeCached(
            `INSERT INTO moz_openpages_temp (url, userContextId, groupId, open_count)
             VALUES ${valuesSql}
             ON CONFLICT(url, userContextId, groupId)
             DO UPDATE SET open_count = open_count + excluded.open_count`,
            params
          );
        }
      });
    } catch (error) {
      // Restore the updates so that they can be retried on the next flush.
      for (let [key, update] of updatesToFlush.entries()) {
        let existing = gPendingMemoryTableDeltas.get(key);
        let delta = (existing?.delta ?? 0) + update.delta;
        if (delta === 0) {
          gPendingMemoryTableDeltas.delete(key);
        } else {
          gPendingMemoryTableDeltas.set(key, {
            url: update.url,
            userContextId: update.userContextId,
            groupId: update.groupId,
            delta,
          });
        }
      }
      throw error;
    }
  })();

  gOngoingFlushPromise = flushPromise;
  try {
    await flushPromise;
  } catch (error) {
    console.error(error);
  } finally {
    gOngoingFlushPromise = null;
    if (gPendingMemoryTableDeltas.size) {
      scheduleMemoryTableFlush();
    }
  }
  return flushPromise;
}

lazy.AsyncShutdown.profileBeforeChange.addBlocker(
  "UrlbarProviderOpenTabs: flush open tabs memory table",
  async () => {
    if (!UrlbarProviderOpenTabs.memoryTableInitialized) {
      return;
    }
    if (gMemoryTableFlushTask?.isArmed) {
      gMemoryTableFlushTask.disarm();
    }
    if (gMemoryTableFlushTask && !gMemoryTableFlushTask.isFinalized) {
      try {
        await gMemoryTableFlushTask.finalize();
      } catch (error) {
        console.error(error);
      }
    }
    await flushPendingMemoryTableUpdates();
  }
);

/**
 * Class used to create the provider.
 */
export class UrlbarProviderOpenTabs extends UrlbarProvider {
  constructor() {
    super();
  }

  /**
   * @returns {Values<typeof UrlbarUtils.PROVIDER_TYPE>}
   */
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  /**
   * Whether this provider should be invoked for the given context.
   * If this method returns false, the providers manager won't start a query
   * with this provider, to save on resources.
   */
  async isActive() {
    // For now we don't actually use this provider to query open tabs, instead
    // we join the temp table in UrlbarProviderPlaces.
    return false;
  }

  /**
   * Tracks whether the memory tables have been initialized yet. Until this
   * happens tabs are only stored in openTabs and later copied over to the
   * memory table.
   */
  static memoryTableInitialized = false;

  /**
   * Return unique urls that are open for given user context id.
   *
   * @param {number|string} userContextId Containers user context id
   * @param {boolean} [isInPrivateWindow] In private browsing window or not
   * @returns {Array} [url, userContextId, groupId | null]
   */
  static getOpenTabUrlsForUserContextId(
    userContextId,
    isInPrivateWindow = false
  ) {
    // It's fairly common to retrieve the value from an HTML attribute, that
    // means we're getting sometimes a string, sometimes an integer. As we're
    // using this as key of a Map, we must treat it consistently.
    userContextId = parseInt(`${userContextId}`);
    userContextId = UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
      userContextId,
      isInPrivateWindow
    );

    let groupEntries = gOpenTabUrls.get(userContextId);
    if (!groupEntries) {
      return [];
    }

    let result = new Set();
    groupEntries.forEach((urls, groupId) => {
      for (let url of urls.keys()) {
        result.add([url, userContextId, groupId]);
      }
    });
    return Array.from(result);
  }

  /**
   * Return unique urls that are open, along with their user context id and group id.
   *
   * @param {boolean} [isInPrivateWindow] Whether it's for a private browsing window
   * @returns {Map} { url => Set([userContextId, groupId]) }
   */
  static getOpenTabUrls(isInPrivateWindow = false) {
    let uniqueUrls = new Map();
    if (isInPrivateWindow) {
      let urlInfo = UrlbarProviderOpenTabs.getOpenTabUrlsForUserContextId(
        PRIVATE_USER_CONTEXT_ID,
        true
      );
      for (let [url, contextId, groupId] of urlInfo) {
        uniqueUrls.set(url, new Set([[contextId, groupId]]));
      }
    } else {
      gOpenTabUrls.forEach((groups, userContextId) => {
        if (userContextId == PRIVATE_USER_CONTEXT_ID) {
          return;
        }

        groups.forEach((urls, groupId) => {
          for (let url of urls.keys()) {
            let userContextAndGroupIds = uniqueUrls.get(url);
            if (!userContextAndGroupIds) {
              userContextAndGroupIds = new Set();
              uniqueUrls.set(url, userContextAndGroupIds);
            }
            userContextAndGroupIds.add([userContextId, groupId]);
          }
        });
      });
    }
    return uniqueUrls;
  }

  /**
   * Return urls registered in the memory table.
   * This is mostly for testing purposes.
   *
   * @returns {Promise<{url: string, userContextId: number, groupId: string | null, count: number}[]>}
   */
  static async getDatabaseRegisteredOpenTabsForTests() {
    await UrlbarProviderOpenTabs.promiseDBPopulated;
    await flushPendingMemoryTableUpdates();
    let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    let rows = await conn.execute(
      "SELECT url, userContextId, NULLIF(groupId, '') groupId, open_count" +
        " FROM moz_openpages_temp ORDER BY url, userContextId, groupId"
    );
    return rows.map(r => ({
      url: r.getResultByName("url"),
      userContextId: r.getResultByName("userContextId"),
      tabGroup: r.getResultByName("groupId"),
      count: r.getResultByName("open_count"),
    }));
  }

  static async flushPendingMemoryTableUpdatesForTests() {
    await UrlbarProviderOpenTabs.promiseDBPopulated;
    if (gMemoryTableFlushTask?.isArmed) {
      gMemoryTableFlushTask.disarm();
    }
    await flushPendingMemoryTableUpdates();
  }

  /**
   * Return userContextId that is used in the moz_openpages_temp table and
   * returned as part of the payload. It differs only for private windows.
   *
   * @param {number} userContextId Containers user context id
   * @param {boolean} isInPrivateWindow In private browsing window or not
   * @returns {number} userContextId
   */
  static getUserContextIdForOpenPagesTable(userContextId, isInPrivateWindow) {
    return isInPrivateWindow ? PRIVATE_USER_CONTEXT_ID : userContextId;
  }

  /**
   * Return whether the provided userContextId is for a non-private tab.
   *
   * @param {number} userContextId the userContextId to evaluate
   * @returns {boolean}
   */
  static isNonPrivateUserContextId(userContextId) {
    return userContextId != PRIVATE_USER_CONTEXT_ID;
  }

  /**
   * Return whether the provided userContextId is for a container.
   *
   * @param {number} userContextId the userContextId to evaluate
   * @returns {boolean}
   */
  static isContainerUserContextId(userContextId) {
    return userContextId > 0;
  }

  /**
   * Copy over cached open tabs to the memory table once the Urlbar
   * connection has been initialized.
   */
  static promiseDBPopulated =
    lazy.PlacesUtils.largeCacheDBConnDeferred.promise.then(async () => {
      // Must be set before populating.
      UrlbarProviderOpenTabs.memoryTableInitialized = true;
      // Populate the table with the current cached tabs.
      for (let [userContextId, groupEntries] of gOpenTabUrls) {
        for (let [groupId, entries] of groupEntries) {
          for (let [url, count] of entries) {
            queueMemoryTableDelta(url, userContextId, groupId, count);
          }
        }
      }
      await flushPendingMemoryTableUpdates();
    });

  /**
   * Registers a tab as open.
   *
   * @param {string} url Address of the tab
   * @param {number|string} userContextId Containers user context id
   * @param {?string} groupId The id of the group the tab belongs to
   * @param {boolean} isInPrivateWindow In private browsing window or not
   */
  static async registerOpenTab(url, userContextId, groupId, isInPrivateWindow) {
    // It's fairly common to retrieve the value from an HTML attribute, that
    // means we're getting sometimes a string, sometimes an integer. As we're
    // using this as key of a Map, we must treat it consistently.
    userContextId = parseInt(`${userContextId}`);
    groupId = groupId ?? null;
    if (!Number.isInteger(userContextId)) {
      lazy.logger.error("Invalid userContextId while registering openTab: ", {
        url,
        userContextId,
        isInPrivateWindow,
      });
      return;
    }
    lazy.logger.info("Registering openTab: ", {
      url,
      userContextId,
      groupId,
      isInPrivateWindow,
    });
    userContextId = UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
      userContextId,
      isInPrivateWindow
    );

    let contextEntries = gOpenTabUrls.get(userContextId);
    if (!contextEntries) {
      contextEntries = new Map();
      gOpenTabUrls.set(userContextId, contextEntries);
    }

    let groupEntries = contextEntries.get(groupId);
    if (!groupEntries) {
      groupEntries = new Map();
      contextEntries.set(groupId, groupEntries);
    }

    groupEntries.set(url, (groupEntries.get(url) ?? 0) + 1);
    queueMemoryTableDelta(url, userContextId, groupId, 1);
  }

  /**
   * Unregisters a previously registered open tab.
   *
   * @param {string} url Address of the tab
   * @param {number|string} userContextId Containers user context id
   * @param {?string} groupId The id of the group the tab belongs to
   * @param {boolean} isInPrivateWindow In private browsing window or not
   */
  static async unregisterOpenTab(
    url,
    userContextId,
    groupId,
    isInPrivateWindow
  ) {
    // It's fairly common to retrieve the value from an HTML attribute, that
    // means we're getting sometimes a string, sometimes an integer. As we're
    // using this as key of a Map, we must treat it consistently.
    userContextId = parseInt(`${userContextId}`);
    groupId = groupId ?? null;
    lazy.logger.info("Unregistering openTab: ", {
      url,
      userContextId,
      groupId,
      isInPrivateWindow,
    });
    userContextId = UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
      userContextId,
      isInPrivateWindow
    );

    let contextEntries = gOpenTabUrls.get(userContextId);
    if (contextEntries) {
      let groupEntries = contextEntries.get(groupId);
      if (groupEntries) {
        let oldCount = groupEntries.get(url);
        if (oldCount == 0) {
          console.error("Tried to unregister a non registered open tab");
          return;
        }
        if (oldCount == 1) {
          groupEntries.delete(url);
          // Note: `groupEntries` might be an empty Map now, though we don't remove it
          // from `gOpenTabUrls` as it's likely to be reused later.
        } else {
          groupEntries.set(url, oldCount - 1);
        }
        queueMemoryTableDelta(url, userContextId, groupId, -1);
      }
    }
  }

  /**
   * Starts querying.
   *
   * @param {object} queryContext The query context object
   * @param {Function} addCallback Callback invoked by the provider to add a new
   *        match.
   * @returns {Promise} resolved when the query stops.
   */
  async startQuery(queryContext, addCallback) {
    // Note: this is not actually expected to be used as an internal provider,
    // because normal history search will already coalesce with the open tabs
    // temp table to return proper frecency.
    // TODO:
    //  * properly search and handle tokens, this is just a mock for now.
    let instance = this.queryInstance;
    let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    await UrlbarProviderOpenTabs.promiseDBPopulated;
    await conn.executeCached(
      `
      SELECT url, userContextId, NULLIF(groupId, '') groupId
      FROM moz_openpages_temp
    `,
      {},
      (row, cancel) => {
        if (instance != this.queryInstance) {
          cancel();
          return;
        }
        addCallback(
          this,
          new lazy.UrlbarResult(
            UrlbarUtils.RESULT_TYPE.TAB_SWITCH,
            UrlbarUtils.RESULT_SOURCE.TABS,
            {
              url: row.getResultByName("url"),
              userContextId: row.getResultByName("userContextId"),
              tabGroup: row.getResultByName("groupId"),
            }
          )
        );
      }
    );
  }
}

