/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/PreallocatedProcessManager.h"

#include <algorithm>

#include "ProcessPriorityManager.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Maybe.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProcInfo.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ScriptSettings.h"
#include "nsIPropertyBag2.h"
#include "nsIXULRuntime.h"
#include "nsMemoryPressure.h"
#include "nsPrintfCString.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsTArray.h"
#include "prsystem.h"

using namespace mozilla::hal;
using namespace mozilla::dom;

namespace mozilla {
/**
 * This singleton class implements the static methods on
 * PreallocatedProcessManager.
 */
class PreallocatedProcessManagerImpl final : public nsIObserver {
  friend class PreallocatedProcessManager;

 public:
  static PreallocatedProcessManagerImpl* Singleton();

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  // See comments on PreallocatedProcessManager for these methods.
  void AddBlocker(ContentParent* aParent);
  void RemoveBlocker(ContentParent* aParent);
  UniqueContentParentKeepAlive Take(const nsACString& aRemoteType);
  void Erase(ContentParent* aParent);

 private:
  static const char* const kObserverTopics[];

  static StaticRefPtr<PreallocatedProcessManagerImpl> sSingleton;

  PreallocatedProcessManagerImpl();
  ~PreallocatedProcessManagerImpl();
  PreallocatedProcessManagerImpl(const PreallocatedProcessManagerImpl&) =
      delete;

  const PreallocatedProcessManagerImpl& operator=(
      const PreallocatedProcessManagerImpl&) = delete;

  void Init();

  bool CanAllocate();
  void AllocateAfterDelay();
  void AllocateOnIdle();
  void AllocateNow();

  void RegisterLaunchStart(ContentParent* aProcess);
  void RecordLaunchComplete(ContentParent* aProcess, bool aSucceeded);
  Maybe<double> SampleCpuTimeMs(ContentParent* aProcess) const;
  MemoryPressureState GetCurrentMemoryPressureState() const;
  void UpdateDelayFromPrefs();
  void UpdateDelayForRecord(double aDurationMs, bool aContended);
  uint32_t GetScheduleDelayMs() const;

  void RereadPrefs();
  void Enable(uint32_t aProcesses);
  void Disable();
  void CloseProcesses();

  bool IsEmpty() const { return mPreallocatedProcesses.IsEmpty(); }
  static bool IsShutdown() {
    return AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed);
  }
  bool IsEnabled() { return mEnabled && !IsShutdown(); }

  bool mEnabled;
  uint32_t mNumberPreallocs;
  AutoTArray<UniqueContentParentKeepAlive, 3> mPreallocatedProcesses;
  struct PendingLaunchInfo {
    RefPtr<ContentParent> mProcess;
    TimeStamp mRequested;
  };
  AutoTArray<PendingLaunchInfo, 4> mPendingLaunches;
  struct LaunchRecord {
    TimeStamp mRequested;
    TimeStamp mCompleted;
    Maybe<double> mCpuTimeMs;
    MemoryPressureState mMemoryPressure = MemoryPressureState::NoPressure;
    bool mSucceeded = false;

    double DurationMs() const {
      return (mCompleted - mRequested).ToMilliseconds();
    }
  };
  AutoTArray<LaunchRecord, 8> mRecentLaunches;
  // Even if we have multiple PreallocatedProcessManagerImpls, we'll have
  // one blocker counter
  static uint32_t sNumBlockers;
  TimeStamp mBlockingStartTime;
  TimeStamp mLastMemoryPressure;
  uint32_t mBaseDelayMs;
  uint32_t mCurrentDelayMs;
  uint32_t mMinDelayMs;
  uint32_t mMaxDelayMs;
  uint32_t mCheapLaunchThresholdMs;
  uint32_t mContentionThresholdMs;
  uint32_t mAdjustmentStepMs;
  uint32_t mMemoryPressureWindowMs;
  uint32_t mCheapLaunchStreak;
  bool mDynamicDelayEnabled;
  bool mCaptureCpuTime;
};

/* static */
uint32_t PreallocatedProcessManagerImpl::sNumBlockers = 0;

const char* const PreallocatedProcessManagerImpl::kObserverTopics[] = {
    "memory-pressure",
    "profile-change-teardown",
    NS_XPCOM_SHUTDOWN_OBSERVER_ID,
};

/* static */
StaticRefPtr<PreallocatedProcessManagerImpl>
    PreallocatedProcessManagerImpl::sSingleton;

/* static */
PreallocatedProcessManagerImpl* PreallocatedProcessManagerImpl::Singleton() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!sSingleton) {
    sSingleton = new PreallocatedProcessManagerImpl;
    sSingleton->Init();
    ClearOnShutdown(&sSingleton);
  }
  return sSingleton;
  //  PreallocatedProcessManagers live until shutdown
}

NS_IMPL_ISUPPORTS(PreallocatedProcessManagerImpl, nsIObserver)

PreallocatedProcessManagerImpl::PreallocatedProcessManagerImpl()
    : mEnabled(false),
      mNumberPreallocs(1),
      mLastMemoryPressure(),
      mBaseDelayMs(StaticPrefs::dom_ipc_processPrelaunch_delayMs()),
      mCurrentDelayMs(mBaseDelayMs),
      mMinDelayMs(StaticPrefs::dom_ipc_processPrelaunch_delayMs()),
      mMaxDelayMs(StaticPrefs::dom_ipc_processPrelaunch_delayMs()),
      mCheapLaunchThresholdMs(StaticPrefs::dom_ipc_processPrelaunch_delayMs()),
      mContentionThresholdMs(StaticPrefs::dom_ipc_processPrelaunch_delayMs()),
      mAdjustmentStepMs(1),
      mMemoryPressureWindowMs(0),
      mCheapLaunchStreak(0),
      mDynamicDelayEnabled(false),
      mCaptureCpuTime(false) {}

PreallocatedProcessManagerImpl::~PreallocatedProcessManagerImpl() {
  // Note: mPreallocatedProcesses may not be null, but all processes should
  // be dead (IsDead==true).  We block Erase() when our observer sees
  // shutdown starting.
}

void PreallocatedProcessManagerImpl::Init() {
  Preferences::AddStrongObserver(this, "dom.ipc.processPrelaunch.enabled");
  // We have to respect processCount at all time. This is especially important
  // for testing.
  Preferences::AddStrongObserver(this, "dom.ipc.processCount");
  // A StaticPref, but we need to adjust the number of preallocated processes
  // if the value goes up or down, so we need to run code on change.
  Preferences::AddStrongObserver(this,
                                 "dom.ipc.processPrelaunch.fission.number");

  nsCOMPtr<nsIObserverService> os = services::GetObserverService();
  MOZ_ASSERT(os);
  for (auto topic : kObserverTopics) {
    os->AddObserver(this, topic, /* ownsWeak */ false);
  }
  UpdateDelayFromPrefs();
  RereadPrefs();
}

NS_IMETHODIMP
PreallocatedProcessManagerImpl::Observe(nsISupports* aSubject,
                                        const char* aTopic,
                                        const char16_t* aData) {
  if (!strcmp("nsPref:changed", aTopic)) {
    // The only other observer we registered was for our prefs.
    RereadPrefs();
  } else if (!strcmp(NS_XPCOM_SHUTDOWN_OBSERVER_ID, aTopic) ||
             !strcmp("profile-change-teardown", aTopic)) {
    Preferences::RemoveObserver(this, "dom.ipc.processPrelaunch.enabled");
    Preferences::RemoveObserver(this, "dom.ipc.processCount");
    Preferences::RemoveObserver(this,
                                "dom.ipc.processPrelaunch.fission.number");

    nsCOMPtr<nsIObserverService> os = services::GetObserverService();
    MOZ_ASSERT(os);
    for (auto topic : kObserverTopics) {
      os->RemoveObserver(this, topic);
    }
  } else if (!strcmp("memory-pressure", aTopic)) {
    mLastMemoryPressure = TimeStamp::Now();
    CloseProcesses();
  } else {
    MOZ_ASSERT_UNREACHABLE("Unknown topic");
  }

  return NS_OK;
}

void PreallocatedProcessManagerImpl::RereadPrefs() {
  if (mozilla::BrowserTabsRemoteAutostart() &&
      Preferences::GetBool("dom.ipc.processPrelaunch.enabled")) {
    int32_t number = 1;
    if (mozilla::FissionAutostart()) {
      number = StaticPrefs::dom_ipc_processPrelaunch_fission_number();
      // limit preallocated processes on low-mem machines
      PRUint64 bytes = PR_GetPhysicalMemorySize();
      if (bytes > 0 &&
          bytes <=
              StaticPrefs::dom_ipc_processPrelaunch_lowmem_mb() * 1024 * 1024) {
        number = 1;
      }
    }
    if (number >= 0) {
      UpdateDelayFromPrefs();
      Enable(number);
      // We have one prealloc queue for all types except File now
      if (static_cast<uint64_t>(number) < mPreallocatedProcesses.Length()) {
        CloseProcesses();
      }
    }
  } else {
    Disable();
  }
}

UniqueContentParentKeepAlive PreallocatedProcessManagerImpl::Take(
    const nsACString& aRemoteType) {
  if (!IsEnabled()) {
    return nullptr;
  }
  UniqueContentParentKeepAlive process;
  if (!IsEmpty()) {
    process = std::move(mPreallocatedProcesses.ElementAt(0));
    mPreallocatedProcesses.RemoveElementAt(0);

    // Don't set the priority to FOREGROUND here, since it may not have
    // finished starting

    // We took a preallocated process. Let's try to start up a new one
    // soon.
    ContentParent* last = mPreallocatedProcesses.SafeLastElement(nullptr).get();
    // There could be a launching process that isn't the last, but that's
    // ok (and unlikely)
    if (!last || !last->IsLaunching()) {
      AllocateAfterDelay();
    }
    MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
            ("Use prealloc process %p%s, %lu available", process.get(),
             process->IsLaunching() ? " (still launching)" : "",
             (unsigned long)mPreallocatedProcesses.Length()));
  }
  if (process && !process->IsLaunching()) {
    ProcessPriorityManager::SetProcessPriority(process.get(),
                                               PROCESS_PRIORITY_FOREGROUND);
  }  // else this will get set by the caller when they call InitInternal()

  return process;
}

void PreallocatedProcessManagerImpl::Erase(ContentParent* aParent) {
  (void)mPreallocatedProcesses.RemoveElement(aParent);
}

void PreallocatedProcessManagerImpl::Enable(uint32_t aProcesses) {
  mNumberPreallocs = aProcesses;
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Enabling preallocation: %u", aProcesses));
  if (mEnabled || IsShutdown()) {
    return;
  }

  mEnabled = true;
  AllocateAfterDelay();
}

void PreallocatedProcessManagerImpl::AddBlocker(ContentParent* aParent) {
  if (sNumBlockers == 0) {
    mBlockingStartTime = TimeStamp::Now();
  }
  sNumBlockers++;
}

void PreallocatedProcessManagerImpl::RemoveBlocker(ContentParent* aParent) {
  // This used to assert that the blocker existed, but preallocated
  // processes aren't blockers anymore because it's not useful and
  // interferes with async launch, and it's simpler if content
  // processes don't need to remember whether they were preallocated.

  MOZ_DIAGNOSTIC_ASSERT(sNumBlockers > 0);
  sNumBlockers--;
  if (sNumBlockers == 0) {
    MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
            ("Blocked preallocation for %fms",
             (TimeStamp::Now() - mBlockingStartTime).ToMilliseconds()));
    PROFILER_MARKER_TEXT("Process", DOM,
                         MarkerTiming::IntervalUntilNowFrom(mBlockingStartTime),
                         "Blocked preallocation");
    if (IsEmpty()) {
      AllocateAfterDelay();
    }
  }
}

bool PreallocatedProcessManagerImpl::CanAllocate() {
  return IsEnabled() && sNumBlockers == 0 &&
         mPreallocatedProcesses.Length() < mNumberPreallocs && !IsShutdown() &&
         (FissionAutostart() ||
          !ContentParent::IsMaxProcessCountReached(DEFAULT_REMOTE_TYPE));
}

void PreallocatedProcessManagerImpl::AllocateAfterDelay() {
  if (!IsEnabled()) {
    return;
  }
  uint32_t delay = GetScheduleDelayMs();
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Starting delayed process start, delay=%u (dynamic=%s)", delay,
           mDynamicDelayEnabled ? "on" : "off"));
  NS_DelayedDispatchToCurrentThread(
      NewRunnableMethod("PreallocatedProcessManagerImpl::AllocateOnIdle", this,
                        &PreallocatedProcessManagerImpl::AllocateOnIdle),
      delay);
}

void PreallocatedProcessManagerImpl::AllocateOnIdle() {
  if (!IsEnabled()) {
    return;
  }

  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Starting process allocate on idle"));
  NS_DispatchToCurrentThreadQueue(
      NewRunnableMethod("PreallocatedProcessManagerImpl::AllocateNow", this,
                        &PreallocatedProcessManagerImpl::AllocateNow),
      EventQueuePriority::Idle);
}

void PreallocatedProcessManagerImpl::AllocateNow() {
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Trying to start process now"));
  if (!CanAllocate()) {
    if (IsEnabled() && IsEmpty() && sNumBlockers > 0) {
      // If it's too early to allocate a process let's retry later.
      AllocateAfterDelay();
    }
    return;
  }

  UniqueContentParentKeepAlive process = ContentParent::MakePreallocProcess();
  if (!process) {
    // We fully failed to create a prealloc process. Don't try to kick off a new
    // preallocation here, to avoid possible looping.
    MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
            ("Failed to launch prealloc process"));
    return;
  }

  RegisterLaunchStart(process.get());
  process->WaitForLaunchAsync(PROCESS_PRIORITY_PREALLOC)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this},
           process = RefPtr{process.get()}](UniqueContentParentKeepAlive&&) {
            const bool success = !process->IsDead();
            self->RecordLaunchComplete(process, success);
            if (!success) {
              self->Erase(process);
              // Process died in startup (before we could add it).  If it
              // dies after this, MarkAsDead() will Erase() this entry.
              // Shouldn't be in the sBrowserContentParents, so we don't need
              // RemoveFromList().  We won't try to kick off a new
              // preallocation here, to avoid possible looping if something is
              // causing them to consistently fail; if everything is ok on the
              // next allocation request we'll kick off creation.
            } else if (self->CanAllocate()) {
              // Continue prestarting processes if needed
              if (self->mPreallocatedProcesses.Length() <
                  self->mNumberPreallocs) {
                self->AllocateOnIdle();
              }
            }
          },
          [self = RefPtr{this}, process = RefPtr{process.get()}]() {
            self->RecordLaunchComplete(process, /* aSucceeded */ false);
            self->Erase(process);
          });

  mPreallocatedProcesses.AppendElement(std::move(process));
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Preallocated = %lu of %d processes",
           (unsigned long)mPreallocatedProcesses.Length(), mNumberPreallocs));
}

void PreallocatedProcessManagerImpl::Disable() {
  if (!mEnabled) {
    return;
  }

  mEnabled = false;
  mPendingLaunches.Clear();
  mRecentLaunches.Clear();
  mCheapLaunchStreak = 0;
  mCurrentDelayMs = mBaseDelayMs;
  CloseProcesses();
}

void PreallocatedProcessManagerImpl::CloseProcesses() {
  // Drop our KeepAlives on these processes. This will automatically lead to the
  // processes being shut down when no keepalives are left.
  mPreallocatedProcesses.Clear();
}

void PreallocatedProcessManagerImpl::RegisterLaunchStart(
    ContentParent* aProcess) {
  if (!aProcess) {
    return;
  }

  PendingLaunchInfo info;
  info.mProcess = aProcess;
  info.mRequested = TimeStamp::Now();
  mPendingLaunches.AppendElement(std::move(info));
}

void PreallocatedProcessManagerImpl::RecordLaunchComplete(
    ContentParent* aProcess, bool aSucceeded) {
  if (!aProcess) {
    return;
  }

  TimeStamp completed = TimeStamp::Now();
  Maybe<PendingLaunchInfo> pending;
  for (size_t index = 0; index < mPendingLaunches.Length(); ++index) {
    if (mPendingLaunches[index].mProcess == aProcess) {
      pending.emplace(std::move(mPendingLaunches[index]));
      mPendingLaunches.RemoveElementAt(index);
      break;
    }
  }

  LaunchRecord record;
  record.mSucceeded = aSucceeded;
  record.mCompleted = completed;
  record.mMemoryPressure = GetCurrentMemoryPressureState();
  if (pending) {
    record.mRequested = pending->mRequested;
  } else {
    record.mRequested = completed;
  }

  if (mCaptureCpuTime && aSucceeded) {
    record.mCpuTimeMs = SampleCpuTimeMs(aProcess);
  }

  if (mRecentLaunches.Length() == mRecentLaunches.Capacity()) {
    mRecentLaunches.RemoveElementAt(0);
  }
  mRecentLaunches.AppendElement(record);

  double durationMs = record.DurationMs();
  bool contended =
      !aSucceeded || record.mMemoryPressure == MemoryPressureState::LowMemory;
  if (record.mCpuTimeMs && *record.mCpuTimeMs >= mContentionThresholdMs) {
    contended = true;
  }
  UpdateDelayForRecord(durationMs, contended);

  nsAutoCString cpuString;
  if (record.mCpuTimeMs) {
    cpuString = nsPrintfCString("%.2f", *record.mCpuTimeMs);
  } else {
    cpuString.AssignLiteral("n/a");
  }

  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Prealloc launch %s in %.2fms delay=%u cpu=%s memory=%u",
           aSucceeded ? "completed" : "failed", durationMs, mCurrentDelayMs,
           cpuString.get(), static_cast<unsigned>(record.mMemoryPressure)));
}

Maybe<double> PreallocatedProcessManagerImpl::SampleCpuTimeMs(
    ContentParent* aProcess) const {
  if (!mCaptureCpuTime || !aProcess) {
    return Nothing();
  }

  nsTArray<mozilla::WindowInfo> windows;
  nsTArray<mozilla::UtilityInfo> utilities;
  nsTArray<mozilla::ProcInfoRequest> requests;
  requests.EmplaceBack(aProcess->OtherPid(), mozilla::ProcType::Preallocated,
                       EmptyCString(), std::move(windows),
                       std::move(utilities));

  auto result = mozilla::GetProcInfoSync(std::move(requests));
  if (!result.IsResolve()) {
    return Nothing();
  }

  const auto& map = result.ResolveValue();
  if (const auto entry = map.lookup(aProcess->OtherPid())) {
    const mozilla::ProcInfo& info = entry->value();
    return Some(static_cast<double>(info.cpuTime) / 1000000.0);
  }
  return Nothing();
}

MemoryPressureState
PreallocatedProcessManagerImpl::GetCurrentMemoryPressureState() const {
  if (mMemoryPressureWindowMs == 0 || mLastMemoryPressure.IsNull()) {
    return MemoryPressureState::NoPressure;
  }

  TimeDuration since = TimeStamp::Now() - mLastMemoryPressure;
  if (since.ToMilliseconds() <= mMemoryPressureWindowMs) {
    return MemoryPressureState::LowMemory;
  }
  return MemoryPressureState::NoPressure;
}

void PreallocatedProcessManagerImpl::UpdateDelayFromPrefs() {
  mBaseDelayMs = StaticPrefs::dom_ipc_processPrelaunch_delayMs();
  mDynamicDelayEnabled =
      StaticPrefs::dom_ipc_processPrelaunch_dynamicDelay_enabled();
  mMinDelayMs = StaticPrefs::dom_ipc_processPrelaunch_dynamicDelay_minMs();
  mMaxDelayMs = StaticPrefs::dom_ipc_processPrelaunch_dynamicDelay_maxMs();
  if (mMaxDelayMs < mMinDelayMs) {
    mMaxDelayMs = mMinDelayMs;
  }
  mCheapLaunchThresholdMs = StaticPrefs::
      dom_ipc_processPrelaunch_dynamicDelay_cheapLaunchThresholdMs();
  mContentionThresholdMs = StaticPrefs::
      dom_ipc_processPrelaunch_dynamicDelay_contentionThresholdMs();
  if (mContentionThresholdMs < mCheapLaunchThresholdMs) {
    mContentionThresholdMs = mCheapLaunchThresholdMs;
  }
  mAdjustmentStepMs = std::max<uint32_t>(
      1, StaticPrefs::dom_ipc_processPrelaunch_dynamicDelay_adjustmentStepMs());
  mMemoryPressureWindowMs = StaticPrefs::
      dom_ipc_processPrelaunch_dynamicDelay_memoryPressureWindowMs();
  mCaptureCpuTime =
      StaticPrefs::dom_ipc_processPrelaunch_dynamicDelay_captureCpuTime();

  if (!mDynamicDelayEnabled) {
    mCurrentDelayMs = mBaseDelayMs;
  } else {
    if (mCurrentDelayMs == 0) {
      mCurrentDelayMs = mBaseDelayMs;
    }
    mCurrentDelayMs =
        std::max(mMinDelayMs, std::min(mCurrentDelayMs, mMaxDelayMs));
  }
}

void PreallocatedProcessManagerImpl::UpdateDelayForRecord(double aDurationMs,
                                                          bool aContended) {
  if (!mDynamicDelayEnabled) {
    return;
  }

  bool contended = aContended || aDurationMs >= mContentionThresholdMs;
  if (contended) {
    mCheapLaunchStreak = 0;
    uint32_t newDelay =
        std::min(mMaxDelayMs, mCurrentDelayMs + mAdjustmentStepMs);
    if (newDelay != mCurrentDelayMs) {
      MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
              ("Contended prelaunch (%.2fms) increasing delay %u -> %u",
               aDurationMs, mCurrentDelayMs, newDelay));
    }
    mCurrentDelayMs = newDelay;
    return;
  }

  bool cheap = aDurationMs <= mCheapLaunchThresholdMs;
  if (cheap) {
    if (mCurrentDelayMs > mMinDelayMs) {
      ++mCheapLaunchStreak;
      if (mCheapLaunchStreak >= 2) {
        uint32_t newDelay =
            std::max(mMinDelayMs, mCurrentDelayMs - mAdjustmentStepMs);
        if (newDelay != mCurrentDelayMs) {
          MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
                  ("Cheap prelaunch (%.2fms) decreasing delay %u -> %u",
                   aDurationMs, mCurrentDelayMs, newDelay));
        }
        mCurrentDelayMs = newDelay;
        mCheapLaunchStreak = 0;
      }
    }
    return;
  }

  mCheapLaunchStreak = 0;
  if (mCurrentDelayMs > mBaseDelayMs) {
    uint32_t delta = std::max<uint32_t>(1, mAdjustmentStepMs / 2);
    uint32_t newDelay = std::max(mMinDelayMs, mCurrentDelayMs - delta);
    if (newDelay != mCurrentDelayMs) {
      MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
              ("Relaxing delay downward %u -> %u toward base %u",
               mCurrentDelayMs, newDelay, mBaseDelayMs));
    }
    mCurrentDelayMs = newDelay;
  } else if (mCurrentDelayMs < mBaseDelayMs) {
    uint32_t delta = std::max<uint32_t>(1, mAdjustmentStepMs / 2);
    uint32_t newDelay = std::min(mMaxDelayMs, mCurrentDelayMs + delta);
    uint32_t clamped = std::min(mBaseDelayMs, newDelay);
    if (clamped != mCurrentDelayMs) {
      MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
              ("Relaxing delay upward %u -> %u toward base %u", mCurrentDelayMs,
               clamped, mBaseDelayMs));
    }
    mCurrentDelayMs = clamped;
  }
}

uint32_t PreallocatedProcessManagerImpl::GetScheduleDelayMs() const {
  if (!mDynamicDelayEnabled) {
    return StaticPrefs::dom_ipc_processPrelaunch_delayMs();
  }

  uint32_t delay = mCurrentDelayMs ? mCurrentDelayMs : mBaseDelayMs;
  delay = std::max(mMinDelayMs, std::min(delay, mMaxDelayMs));
  return delay;
}

inline PreallocatedProcessManagerImpl*
PreallocatedProcessManager::GetPPMImpl() {
  if (PreallocatedProcessManagerImpl::IsShutdown()) {
    return nullptr;
  }
  return PreallocatedProcessManagerImpl::Singleton();
}

/* static */
bool PreallocatedProcessManager::Enabled() {
  if (auto impl = GetPPMImpl()) {
    return impl->IsEnabled();
  }
  return false;
}

/* static */
void PreallocatedProcessManager::AddBlocker(const nsACString& aRemoteType,
                                            ContentParent* aParent) {
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("AddBlocker: %s %p (sNumBlockers=%d)",
           PromiseFlatCString(aRemoteType).get(), aParent,
           PreallocatedProcessManagerImpl::sNumBlockers));
  if (auto impl = GetPPMImpl()) {
    impl->AddBlocker(aParent);
  }
}

/* static */
void PreallocatedProcessManager::RemoveBlocker(const nsACString& aRemoteType,
                                               ContentParent* aParent) {
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("RemoveBlocker: %s %p (sNumBlockers=%d)",
           PromiseFlatCString(aRemoteType).get(), aParent,
           PreallocatedProcessManagerImpl::sNumBlockers));
  if (auto impl = GetPPMImpl()) {
    impl->RemoveBlocker(aParent);
  }
}

/* static */
UniqueContentParentKeepAlive PreallocatedProcessManager::Take(
    const nsACString& aRemoteType) {
  if (auto impl = GetPPMImpl()) {
    return impl->Take(aRemoteType);
  }
  return nullptr;
}

/* static */
void PreallocatedProcessManager::Erase(ContentParent* aParent) {
  if (auto impl = GetPPMImpl()) {
    impl->Erase(aParent);
  }
}

}  // namespace mozilla
