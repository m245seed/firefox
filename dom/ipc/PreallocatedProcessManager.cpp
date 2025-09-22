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
#include "mozilla/ProcInfo.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/Telemetry.h"
#include "mozilla/Maybe.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ScriptSettings.h"
#include "nsIPropertyBag2.h"
#include "nsPrintfCString.h"
#include "nsString.h"
#include "nsIXULRuntime.h"
#include "nsServiceManagerUtils.h"
#include "nsTArray.h"
#include "prsystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
  Maybe<double> SampleProcessCpuLoad(const TimeStamp& aSampleTime);
  uint32_t ComputeDynamicDelayMs() const;
  uint32_t GetMinDelayMs() const;
  uint32_t GetMaxDelayMs() const;
  struct HeuristicEvaluation {
    double mAverageDurationMs = 0.0;
    double mPreviousDelayMs = 0.0;
    bool mRecentMemoryPressure = false;
    bool mRecentHighCpu = false;
    bool mRecentOverlap = false;
    size_t mCheapCount = 0;
    size_t mContentionCount = 0;
    size_t mSampleCount = 0;
    Maybe<double> mLastCpuLoad;
    Maybe<TimeDuration> mGapSincePrevious;
  };
  void LogLaunchHeuristics(ContentParent* aParent, uint32_t aDelayMs,
                           const HeuristicEvaluation& aEvaluation) const;

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
    TimeDuration mDuration;
    bool mMemoryPressure;
    Maybe<double> mCpuLoad;
  };
  AutoTArray<CompletedPrelaunch, 8> mRecentLaunches;
  TimeStamp mLastMemoryPressureNotification;
  mutable uint32_t mLastComputedDelayMs;
  bool mHasCpuSample;
  uint64_t mLastCpuSampleMs;
  TimeStamp mLastCpuSampleTime;
  mutable HeuristicEvaluation mLastHeuristicEvaluation;
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
      mLastComputedDelayMs(StaticPrefs::dom_ipc_processPrelaunch_delayMs()),
      mHasCpuSample(false),
      mLastCpuSampleMs(0) {
  mLastHeuristicEvaluation.mPreviousDelayMs =
      static_cast<double>(mLastComputedDelayMs);
}

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

Maybe<double> PreallocatedProcessManagerImpl::SampleProcessCpuLoad(
    const TimeStamp& aSampleTime) {
  if (aSampleTime.IsNull()) {
    return Nothing();
  }

  uint64_t cpuTimeMs = 0;
  if (NS_FAILED(GetCpuTimeSinceProcessStartInMs(&cpuTimeMs))) {
    return Nothing();
  }

  if (!mHasCpuSample || mLastCpuSampleTime.IsNull()) {
    mHasCpuSample = true;
    mLastCpuSampleMs = cpuTimeMs;
    mLastCpuSampleTime = aSampleTime;
    return Nothing();
  }

  if (cpuTimeMs <= mLastCpuSampleMs || aSampleTime <= mLastCpuSampleTime) {
    mLastCpuSampleMs = cpuTimeMs;
    mLastCpuSampleTime = aSampleTime;
    return Nothing();
  }

  const uint64_t cpuDeltaMs = cpuTimeMs - mLastCpuSampleMs;
  const double wallDeltaMs = (aSampleTime - mLastCpuSampleTime).ToMilliseconds();
  mLastCpuSampleMs = cpuTimeMs;
  mLastCpuSampleTime = aSampleTime;

  if (wallDeltaMs <= 0.0) {
    return Nothing();
  }

  uint32_t processorCount = PR_GetNumberOfProcessors();
  processorCount = std::max(processorCount, 1u);

  const double normalizedLoad =
      std::clamp((static_cast<double>(cpuDeltaMs) / wallDeltaMs) /
                     static_cast<double>(processorCount),
                 0.0, 1.0);
  return Some(normalizedLoad);
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

  const TimeStamp cpuSampleTime = aReadyTime.IsNull() ? TimeStamp::Now() : aReadyTime;
  Maybe<double> cpuLoad = SampleProcessCpuLoad(cpuSampleTime);

  auto& record = *mRecentLaunches.AppendElement();
  record.mStartTime = aStartTime;
  record.mReadyTime = aReadyTime;
  record.mDuration =
      (!aReadyTime.IsNull() && !aStartTime.IsNull()) ? aReadyTime - aStartTime
                                                     : TimeDuration();
  record.mMemoryPressure = memoryPressure;
  record.mCpuLoad = cpuLoad;

  constexpr size_t kMaxRecentLaunches = 8;
  if (mRecentLaunches.Length() > kMaxRecentLaunches) {
    mRecentLaunches.RemoveElementsAt(0, mRecentLaunches.Length() -
                                            kMaxRecentLaunches);
  }

  Maybe<TimeDuration> sincePreviousReady;
  if (mRecentLaunches.Length() >= 2) {
    const auto& previous = mRecentLaunches[mRecentLaunches.Length() - 2];
    if (!previous.mReadyTime.IsNull() && !aStartTime.IsNull()) {
      sincePreviousReady = Some(aStartTime - previous.mReadyTime);
    }
  }

  const uint32_t delayMs = ComputeDynamicDelayMs();
  HeuristicEvaluation evaluation = mLastHeuristicEvaluation;
  evaluation.mGapSincePrevious = sincePreviousReady;

  LogLaunchHeuristics(aParent, delayMs, evaluation);

  const double lastDurationMs = record.mDuration.ToMilliseconds();
  const double clampedDurationMs = std::max(0.0, lastDurationMs);
  const uint32_t durationTelemetry = static_cast<uint32_t>(
      std::min(clampedDurationMs,
               static_cast<double>(std::numeric_limits<uint32_t>::max())));
  const bool contentionTelemetry = evaluation.mRecentMemoryPressure ||
                                   evaluation.mRecentHighCpu ||
                                   evaluation.mRecentOverlap ||
                                   evaluation.mContentionCount > 0;

  Telemetry::ScalarSet(Telemetry::ScalarID::DOM_IPC_PRELAUNCH_DYNAMIC_DELAY_MS,
                       delayMs);
  Telemetry::ScalarSet(
      Telemetry::ScalarID::DOM_IPC_PRELAUNCH_RECENT_LAUNCH_MS,
      durationTelemetry);
  Telemetry::ScalarSet(
      Telemetry::ScalarID::DOM_IPC_PRELAUNCH_RECENT_CONTENTION,
      contentionTelemetry);

  const double cpuPercent = record.mCpuLoad
                                ? (*record.mCpuLoad) * 100.0
                                : std::numeric_limits<double>::quiet_NaN();
  const double gapMs = evaluation.mGapSincePrevious
                           ? evaluation.mGapSincePrevious->ToMilliseconds()
                           : std::numeric_limits<double>::quiet_NaN();
  nsAutoCString cpuString;
  if (record.mCpuLoad) {
    cpuString = nsPrintfCString("%.1f%%", cpuPercent);
  } else {
    cpuString.AssignLiteral("n/a");
  }

  PROFILER_MARKER_TEXT(
      "Process", DOM, MarkerTiming::InstantAt(aReadyTime),
      nsPrintfCString(
          "Prealloc launch %.2fms (avg %.2fms, prev %.2fms, delay %ums, "
          "cheap=%zu/%zu contended=%zu cpu=%s memory=%d overlap=%d gap=%.2fms)",
          lastDurationMs, evaluation.mAverageDurationMs,
          evaluation.mPreviousDelayMs, delayMs, evaluation.mCheapCount,
          evaluation.mSampleCount, evaluation.mContentionCount,
          cpuString.get(), evaluation.mRecentMemoryPressure,
          evaluation.mRecentOverlap, gapMs));
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
    ContentParent* aParent, uint32_t aDelayMs,
    const HeuristicEvaluation& aEvaluation) const {
  const double lastDurationMs =
      mRecentLaunches.IsEmpty()
          ? 0.0
          : mRecentLaunches.LastElement().mDuration.ToMilliseconds();
  const double cpuPercent = aEvaluation.mLastCpuLoad
                                ? (*aEvaluation.mLastCpuLoad) * 100.0
                                : std::numeric_limits<double>::quiet_NaN();
  const double gapMs = aEvaluation.mGapSincePrevious
                           ? aEvaluation.mGapSincePrevious->ToMilliseconds()
                           : std::numeric_limits<double>::quiet_NaN();

  MOZ_LOG(ContentParent::GetLog(), LogLevel::Info,
          ("Prealloc launch=%p duration=%.2fms avg=%.2fms prev-delay=%.2fms "
           "next-delay=%ums cpu=%.1f%% memory-pressure=%d overlap=%d "
           "cheap=%zu/%zu contended=%zu high-cpu=%d gap=%.2fms",
           aParent, lastDurationMs, aEvaluation.mAverageDurationMs,
           aEvaluation.mPreviousDelayMs, aDelayMs, cpuPercent,
           aEvaluation.mRecentMemoryPressure, aEvaluation.mRecentOverlap,
           aEvaluation.mCheapCount, aEvaluation.mSampleCount,
           aEvaluation.mContentionCount, aEvaluation.mRecentHighCpu, gapMs));
}

uint32_t PreallocatedProcessManagerImpl::ComputeDynamicDelayMs() const {
  const uint32_t minDelay = GetMinDelayMs();
  const uint32_t maxDelay = GetMaxDelayMs();
  uint32_t prefDelay = StaticPrefs::dom_ipc_processPrelaunch_delayMs();
  prefDelay = std::clamp(prefDelay, minDelay, maxDelay);

  HeuristicEvaluation evaluation;
  if (!mRecentLaunches.IsEmpty()) {
    evaluation.mLastCpuLoad = mRecentLaunches.LastElement().mCpuLoad;
  }

  const double minDelayDouble = static_cast<double>(minDelay);
  const double maxDelayDouble = static_cast<double>(maxDelay);
  const double lastDelayDouble = mLastComputedDelayMs
                                     ? static_cast<double>(mLastComputedDelayMs)
                                     : static_cast<double>(prefDelay);
  evaluation.mPreviousDelayMs =
      std::clamp(lastDelayDouble, minDelayDouble, maxDelayDouble);

  auto finalize = [&](uint32_t aDelay) {
    mLastComputedDelayMs = aDelay;
    mLastHeuristicEvaluation = evaluation;
    return aDelay;
  };

  if (!StaticPrefs::dom_ipc_processPrelaunch_dynamicDelay_enabled()) {
    evaluation.mSampleCount =
        std::min<size_t>(mRecentLaunches.Length(), size_t(4));
    return finalize(prefDelay);
  }

  if (mRecentLaunches.IsEmpty()) {
    evaluation.mSampleCount = 0;
    evaluation.mAverageDurationMs = 0.0;
    evaluation.mRecentMemoryPressure = false;
    evaluation.mRecentHighCpu = false;
    evaluation.mRecentOverlap = false;
    evaluation.mCheapCount = 0;
    evaluation.mContentionCount = 0;
    return finalize(prefDelay);
  }

  const size_t sampleCount =
      std::min<size_t>(mRecentLaunches.Length(), size_t(4));
  evaluation.mSampleCount = sampleCount;

  AutoTArray<double, 4> durations;
  AutoTArray<bool, 4> pressures;
  AutoTArray<Maybe<double>, 4> cpuLoads;
  durations.SetCapacity(sampleCount);
  pressures.SetCapacity(sampleCount);
  cpuLoads.SetCapacity(sampleCount);

  TimeStamp previousReady;
  bool havePreviousReady = false;
  bool hadOverlap = false;

  for (size_t offset = mRecentLaunches.Length() - sampleCount;
       offset < mRecentLaunches.Length(); ++offset) {
    const auto& sample = mRecentLaunches[offset];
    durations.AppendElement(sample.mDuration.ToMilliseconds());
    pressures.AppendElement(sample.mMemoryPressure);
    cpuLoads.AppendElement(sample.mCpuLoad);
    if (havePreviousReady && !sample.mStartTime.IsNull() &&
        !previousReady.IsNull() && sample.mStartTime < previousReady) {
      hadOverlap = true;
    }
    if (!sample.mReadyTime.IsNull()) {
      previousReady = sample.mReadyTime;
      havePreviousReady = true;
    }
  }

  evaluation.mRecentOverlap = hadOverlap;

  double totalDurationMs = 0.0;
  for (double duration : durations) {
    totalDurationMs += duration;
  }
  evaluation.mAverageDurationMs =
      sampleCount ? (totalDurationMs / sampleCount) : 0.0;

  double currentDelay = evaluation.mPreviousDelayMs;
  const double cheapThreshold = currentDelay * 0.7;
  const double contendedThreshold = currentDelay * 1.25;

  bool anyMemoryPressure = false;
  bool highCpuObserved = false;
  size_t cheapCount = 0;
  size_t contendedCount = 0;

  for (size_t idx = 0; idx < sampleCount; ++idx) {
    const double duration = durations[idx];
    const bool pressure = pressures[idx];
    const Maybe<double>& cpu = cpuLoads[idx];

    anyMemoryPressure |= pressure;

    const bool highCpu = cpu && *cpu >= (pressure ? 0.7 : 0.85);
    highCpuObserved |= highCpu;

    if (!pressure && (!cpu || *cpu <= 0.6) && duration <= cheapThreshold) {
      cheapCount++;
    }

    if (pressure || highCpu || duration >= contendedThreshold) {
      contendedCount++;
    }
  }

  evaluation.mRecentMemoryPressure = anyMemoryPressure;
  evaluation.mRecentHighCpu = highCpuObserved;
  evaluation.mCheapCount = cheapCount;
  evaluation.mContentionCount = contendedCount;

  double desiredDelay = currentDelay;
  const bool experiencedContention =
      anyMemoryPressure || highCpuObserved || hadOverlap || contendedCount > 0;

  if (experiencedContention) {
    double severity = static_cast<double>(contendedCount);
    if (anyMemoryPressure) {
      severity += 0.5;
    }
    if (highCpuObserved) {
      severity += 0.5;
    }
    if (hadOverlap) {
      severity += 0.5;
    }
    const double scale = 1.0 + std::min(severity * 0.15, 1.0);
    const double durationTarget = evaluation.mAverageDurationMs > 0.0
                                      ? std::max(evaluation.mAverageDurationMs * 1.1,
                                                 currentDelay)
                                      : currentDelay;
    desiredDelay = std::max(currentDelay * scale, durationTarget);
  } else if (cheapCount >= sampleCount && sampleCount > 0) {
    desiredDelay = std::max(
        minDelayDouble,
        std::min(currentDelay * 0.6,
                 evaluation.mAverageDurationMs > 0.0
                     ? std::max(evaluation.mAverageDurationMs * 0.85,
                                minDelayDouble)
                     : currentDelay * 0.6));
  } else if (cheapCount * 2 >= sampleCount && sampleCount > 0) {
    desiredDelay = std::max(
        minDelayDouble,
        (currentDelay * 0.7) +
            (evaluation.mAverageDurationMs > 0.0
                 ? (evaluation.mAverageDurationMs * 0.3)
                 : 0.0));
  } else {
    desiredDelay = std::max(
        minDelayDouble,
        (currentDelay * 0.9) +
            (evaluation.mAverageDurationMs > 0.0
                 ? (evaluation.mAverageDurationMs * 0.1)
                 : 0.0));
  }

  desiredDelay = std::clamp(desiredDelay, minDelayDouble, maxDelayDouble);

  uint32_t computedDelay =
      static_cast<uint32_t>(std::lround(desiredDelay));
  computedDelay = std::clamp(computedDelay, minDelay, maxDelay);

  return finalize(computedDelay);
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
