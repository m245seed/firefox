/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Preferences.h"
#include "mozilla/Unused.h"
#include "gtest/gtest.h"

namespace mozilla {
uint32_t GetMaxWebProcessCount();
}  // namespace mozilla

extern "C" void XRE_SetProcessCountTestOverride(size_t aCpuCount,
                                                uint64_t aTotalMemory);
extern "C" void XRE_ClearProcessCountTestOverride();

namespace {

class AutoProcessCountOverride {
 public:
  AutoProcessCountOverride(size_t aCpuCount, uint64_t aTotalMemory) {
    XRE_SetProcessCountTestOverride(aCpuCount, aTotalMemory);
  }

  ~AutoProcessCountOverride() { XRE_ClearProcessCountTestOverride(); }
};

class AutoIntPref final {
 public:
  AutoIntPref(const char* aPrefName, int32_t aValue)
      : mPrefName(aPrefName),
        mHadUserValue(mozilla::Preferences::HasUserValue(aPrefName)) {
    if (mHadUserValue) {
      mOldValue = mozilla::Preferences::GetInt(aPrefName);
    }
    mozilla::Unused << mozilla::Preferences::SetInt(aPrefName, aValue);
  }

  ~AutoIntPref() {
    if (mHadUserValue) {
      mozilla::Unused << mozilla::Preferences::SetInt(mPrefName, mOldValue);
    } else {
      mozilla::Preferences::ClearUser(mPrefName);
    }
  }

 private:
  const char* mPrefName;
  bool mHadUserValue;
  int32_t mOldValue = 0;
};

}  // namespace

TEST(GetMaxWebProcessCount, UsesCpuWhenAuto)
{
  AutoIntPref multiOptOut("dom.ipc.multiOptOut", 0);
  AutoIntPref processCount("dom.ipc.processCount", 0);
  AutoProcessCountOverride overrides(6, 64ULL << 30);

  EXPECT_EQ(6u, mozilla::GetMaxWebProcessCount());
}

TEST(GetMaxWebProcessCount, UsesMemoryWhenAuto)
{
  AutoIntPref multiOptOut("dom.ipc.multiOptOut", 0);
  AutoIntPref processCount("dom.ipc.processCount", 0);
  // Limit the system to 3 GiB so the heuristic should fall back to a single
  // content process despite the large CPU count.
  AutoProcessCountOverride overrides(16, 3ULL << 30);

  EXPECT_EQ(1u, mozilla::GetMaxWebProcessCount());
}

TEST(GetMaxWebProcessCount, HonorsUserOverride)
{
  AutoIntPref multiOptOut("dom.ipc.multiOptOut", 0);
  AutoIntPref processCount("dom.ipc.processCount", 5);
  AutoProcessCountOverride overrides(16, 128ULL << 30);

  EXPECT_EQ(5u, mozilla::GetMaxWebProcessCount());
}

TEST(GetMaxWebProcessCount, SanitizerClampsAuto)
{
  AutoIntPref multiOptOut("dom.ipc.multiOptOut", 0);
  AutoIntPref processCount("dom.ipc.processCount", 0);
  AutoProcessCountOverride overrides(32, 256ULL << 30);

  uint32_t count = mozilla::GetMaxWebProcessCount();
#if defined(MOZ_ASAN) || defined(MOZ_TSAN)
  EXPECT_EQ(4u, count);
#else
  EXPECT_EQ(32u, count);
#endif
}
