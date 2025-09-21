/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);
const { Services } = ChromeUtils.importESModule(
  "resource://gre/modules/Services.sys.mjs"
);
const { ctypes } = ChromeUtils.importESModule(
  "resource://gre/modules/ctypes.sys.mjs"
);

const libFile = Services.dirsvc.get("GreBinD", Ci.nsIFile);
libFile.append(ctypes.libraryName("xul"));
const lib = ctypes.open(libFile.path);

const setOverride = lib.declare(
  "XRE_SetProcessCountTestOverride",
  ctypes.default_abi,
  ctypes.void_t,
  ctypes.size_t,
  ctypes.uint64_t
);
const clearOverride = lib.declare(
  "XRE_ClearProcessCountTestOverride",
  ctypes.default_abi,
  ctypes.void_t
);

const GiB = 1024 * 1024 * 1024;

registerCleanupFunction(() => {
  try {
    clearOverride();
  } catch (ex) {
    // Ignore failures if the override was never installed.
  }
  Services.prefs.clearUserPref("dom.ipc.processCount");
  Services.prefs.clearUserPref("dom.ipc.multiOptOut");
  lib.close();
});

function withOverrides(cpuCount, totalMemoryBytes, callback) {
  setOverride(cpuCount, totalMemoryBytes);
  try {
    callback();
  } finally {
    clearOverride();
  }
}

function run_test() {
  Services.prefs.setIntPref("dom.ipc.multiOptOut", 0);
  Services.prefs.setIntPref("dom.ipc.processCount", 0);

  withOverrides(6, 64 * GiB, () => {
    Assert.equal(Services.appinfo.maxWebProcessCount, 6);
  });

  withOverrides(16, 3 * GiB, () => {
    Assert.equal(Services.appinfo.maxWebProcessCount, 1);
  });

  Services.prefs.setIntPref("dom.ipc.processCount", 5);
  withOverrides(16, 128 * GiB, () => {
    Assert.equal(Services.appinfo.maxWebProcessCount, 5);
  });
  Services.prefs.setIntPref("dom.ipc.processCount", 0);

  withOverrides(32, 256 * GiB, () => {
    const count = Services.appinfo.maxWebProcessCount;
    if (AppConstants.ASAN || AppConstants.TSAN) {
      Assert.equal(count, 4);
    } else {
      Assert.equal(count, 32);
    }
  });
}
