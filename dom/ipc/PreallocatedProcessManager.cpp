/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/PreallocatedProcessManager.h"

#include "ProcessPriorityManager.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/Maybe.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ScriptSettings.h"
#include "nsIPropertyBag2.h"
#include "nsPrintfCString.h"
#include "nsIXULRuntime.h"
#include "nsServiceManagerUtils.h"
#include "nsTArray.h"
#include "prsystem.h"

#include <algorithm>

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

  void RecordPendingLaunch(ContentParent* aParent,
                           const TimeStamp& aStartTime);
  void ForgetPendingLaunch(ContentParent* aParent);
  Maybe<TimeStamp> TakePendingLaunchStart(ContentParent* aParent);
  void RecordLaunchCompletion(ContentParent* aParent,
                              const TimeStamp& aStartTime,
                              const TimeStamp& aReadyTime);
  uint32_t ComputeDynamicDelayMs() const;
  uint32_t GetMinDelayMs() const;
  uint32_t GetMaxDelayMs() const;
  void LogLaunchHeuristics(ContentParent* aParent, uint32_t aDelayMs,
                           double aAverageDuration,
                           bool aMemoryPressure) const;

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
  // Even if we have multiple PreallocatedProcessManagerImpls, we'll have
  // one blocker counter
  static uint32_t sNumBlockers;
  TimeStamp mBlockingStartTime;
  struct PendingPrelaunch {
    RefPtr<ContentParent> mProcess;
    TimeStamp mStartTime;
  };
  AutoTArray<PendingPrelaunch, 4> mPendingPrelaunches;
  struct CompletedPrelaunch {
    TimeStamp mStartTime;
    TimeStamp mReadyTime;
    bool mMemoryPressure;
  };
  AutoTArray<CompletedPrelaunch, 8> mRecentLaunches;
  TimeStamp mLastMemoryPressureNotification;
  mutable uint32_t mLastComputedDelayMs;
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
      mLastComputedDelayMs(StaticPrefs::dom_ipc_processPrelaunch_delayMs()) {}

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
    mLastMemoryPressureNotification = TimeStamp::Now();
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
  uint32_t delay = ComputeDynamicDelayMs();
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Starting delayed process start, delay=%u", delay));
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

  TimeStamp launchStart = TimeStamp::Now();
  RecordPendingLaunch(process.get(), launchStart);

  process->WaitForLaunchAsync(PROCESS_PRIORITY_PREALLOC)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this},
           process = RefPtr{process.get()}](UniqueContentParentKeepAlive&&) {
            auto startTime = self->TakePendingLaunchStart(process);
            if (process->IsDead()) {
              self->Erase(process);
              // Process died in startup (before we could add it).  If it
              // dies after this, MarkAsDead() will Erase() this entry.
              // Shouldn't be in the sBrowserContentParents, so we don't need
              // RemoveFromList().  We won't try to kick off a new
              // preallocation here, to avoid possible looping if something is
              // causing them to consistently fail; if everything is ok on the
              // next allocation request we'll kick off creation.
            } else {
              if (startTime) {
                self->RecordLaunchCompletion(process, *startTime,
                                             TimeStamp::Now());
              }
              if (self->CanAllocate()) {
                // Continue prestarting processes if needed
                if (self->mPreallocatedProcesses.Length() <
                    self->mNumberPreallocs) {
                  self->AllocateOnIdle();
                }
              }
            }
          },
          [self = RefPtr{this}, process = RefPtr{process.get()}]() {
            self->ForgetPendingLaunch(process);
            self->Erase(process);
          });

  mPreallocatedProcesses.AppendElement(std::move(process));
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Debug,
          ("Preallocated = %lu of %d processes",
           (unsigned long)mPreallocatedProcesses.Length(), mNumberPreallocs));
}

void PreallocatedProcessManagerImpl::RecordPendingLaunch(
    ContentParent* aParent, const TimeStamp& aStartTime) {
  if (MOZ_UNLIKELY(!aParent)) {
    return;
  }
  auto& entry = *mPendingPrelaunches.AppendElement();
  entry.mProcess = aParent;
  entry.mStartTime = aStartTime;
}

void PreallocatedProcessManagerImpl::ForgetPendingLaunch(ContentParent* aParent) {
  if (MOZ_UNLIKELY(!aParent)) {
    return;
  }
  for (size_t idx = 0; idx < mPendingPrelaunches.Length(); ++idx) {
    if (mPendingPrelaunches[idx].mProcess == aParent) {
      mPendingPrelaunches.RemoveElementAt(idx);
      return;
    }
  }
}

Maybe<TimeStamp> PreallocatedProcessManagerImpl::TakePendingLaunchStart(
    ContentParent* aParent) {
  if (MOZ_UNLIKELY(!aParent)) {
    return Nothing();
  }
  for (size_t idx = 0; idx < mPendingPrelaunches.Length(); ++idx) {
    if (mPendingPrelaunches[idx].mProcess == aParent) {
      TimeStamp start = mPendingPrelaunches[idx].mStartTime;
      mPendingPrelaunches.RemoveElementAt(idx);
      return Some(start);
    }
  }
  return Nothing();
}

void PreallocatedProcessManagerImpl::RecordLaunchCompletion(
    ContentParent* aParent, const TimeStamp& aStartTime,
    const TimeStamp& aReadyTime) {
  bool memoryPressure = false;
  if (!mLastMemoryPressureNotification.IsNull() &&
      !aReadyTime.IsNull() && aReadyTime >= mLastMemoryPressureNotification) {
    const TimeDuration delta = aReadyTime - mLastMemoryPressureNotification;
    if (delta.ToSeconds() <= 10.0) {
      memoryPressure = true;
    }
  }

  auto& record = *mRecentLaunches.AppendElement();
  record.mStartTime = aStartTime;
  record.mReadyTime = aReadyTime;
  record.mMemoryPressure = memoryPressure;

  constexpr size_t kMaxRecentLaunches = 8;
  if (mRecentLaunches.Length() > kMaxRecentLaunches) {
    mRecentLaunches.RemoveElementsAt(0, mRecentLaunches.Length() -
                                            kMaxRecentLaunches);
  }

  double totalDurationMs = 0.0;
  bool recentMemoryPressure = false;
  const size_t sampleCount =
      std::min<size_t>(mRecentLaunches.Length(), size_t(3));
  for (size_t offset = mRecentLaunches.Length() - sampleCount;
       offset < mRecentLaunches.Length(); ++offset) {
    const auto& sample = mRecentLaunches[offset];
    totalDurationMs +=
        (sample.mReadyTime - sample.mStartTime).ToMilliseconds();
    recentMemoryPressure |= sample.mMemoryPressure;
  }
  const double averageDurationMs =
      sampleCount ? (totalDurationMs / sampleCount) : 0.0;

  const uint32_t delayMs = ComputeDynamicDelayMs();
  LogLaunchHeuristics(aParent, delayMs, averageDurationMs,
                      recentMemoryPressure);

  PROFILER_MARKER_TEXT(
      "Process", DOM, MarkerTiming::InstantAt(aReadyTime),
      nsPrintfCString("Prealloc launch %.2fms (avg %.2fms, delay %ums, pressure=%d)",
                      (aReadyTime - aStartTime).ToMilliseconds(),
                      averageDurationMs, delayMs, recentMemoryPressure));
}

uint32_t PreallocatedProcessManagerImpl::GetMinDelayMs() const {
  return StaticPrefs::dom_ipc_processPrelaunch_delay_minMs();
}

uint32_t PreallocatedProcessManagerImpl::GetMaxDelayMs() const {
  uint32_t minDelay = GetMinDelayMs();
  uint32_t maxDelay = StaticPrefs::dom_ipc_processPrelaunch_delay_maxMs();
  return std::max(minDelay, maxDelay);
}

void PreallocatedProcessManagerImpl::LogLaunchHeuristics(
    ContentParent* aParent, uint32_t aDelayMs, double aAverageDuration,
    bool aMemoryPressure) const {
  const double lastDurationMs = mRecentLaunches.IsEmpty()
                                    ? 0.0
                                    : (mRecentLaunches.LastElement().mReadyTime -
                                       mRecentLaunches.LastElement().mStartTime)
                                          .ToMilliseconds();
  MOZ_LOG(ContentParent::GetLog(), LogLevel::Info,
          ("Prealloc launch=%p duration=%.2fms avg=%.2fms memory-pressure=%d "
           "next-delay=%ums",
           aParent, lastDurationMs, aAverageDuration, aMemoryPressure, aDelayMs));
}

uint32_t PreallocatedProcessManagerImpl::ComputeDynamicDelayMs() const {
  uint32_t baseDelay = StaticPrefs::dom_ipc_processPrelaunch_delayMs();
  const uint32_t minDelay = GetMinDelayMs();
  const uint32_t maxDelay = GetMaxDelayMs();

  baseDelay = std::max(baseDelay, minDelay);
  baseDelay = std::min(baseDelay, maxDelay);

  if (!StaticPrefs::dom_ipc_processPrelaunch_dynamicDelay_enabled()) {
    mLastComputedDelayMs = baseDelay;
    return baseDelay;
  }

  if (mRecentLaunches.IsEmpty()) {
    mLastComputedDelayMs = baseDelay;
    return baseDelay;
  }

  const size_t sampleCount =
      std::min<size_t>(mRecentLaunches.Length(), size_t(3));
  double totalDurationMs = 0.0;
  bool memoryPressure = false;
  for (size_t offset = mRecentLaunches.Length() - sampleCount;
       offset < mRecentLaunches.Length(); ++offset) {
    const auto& sample = mRecentLaunches[offset];
    totalDurationMs +=
        (sample.mReadyTime - sample.mStartTime).ToMilliseconds();
    memoryPressure |= sample.mMemoryPressure;
  }
  const double averageDurationMs =
      sampleCount ? (totalDurationMs / sampleCount) : 0.0;

  double desiredDelay = static_cast<double>(baseDelay);
  if (memoryPressure) {
    desiredDelay *= 1.5;
  } else {
    constexpr double kCheapLaunchMs = 500.0;
    constexpr double kContendedLaunchMs = 1500.0;
    if (averageDurationMs <= kCheapLaunchMs) {
      desiredDelay *= 0.6;
    } else if (averageDurationMs >= kContendedLaunchMs) {
      desiredDelay *= 1.5;
    } else if (baseDelay > 0) {
      const double normalizedBase =
          static_cast<double>(std::max(baseDelay, 1u));
      desiredDelay *= averageDurationMs / normalizedBase;
    }
  }

  uint32_t computedDelay =
      static_cast<uint32_t>(std::max(0.0, desiredDelay));
  computedDelay = std::max(minDelay, computedDelay);
  computedDelay = std::min(maxDelay, computedDelay);

  mLastComputedDelayMs = computedDelay;
  return computedDelay;
}

void PreallocatedProcessManagerImpl::Disable() {
  if (!mEnabled) {
    return;
  }

  mEnabled = false;
  CloseProcesses();
}

void PreallocatedProcessManagerImpl::CloseProcesses() {
  // Drop our KeepAlives on these processes. This will automatically lead to the
  // processes being shut down when no keepalives are left.
  mPreallocatedProcesses.Clear();
  mPendingPrelaunches.Clear();
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
