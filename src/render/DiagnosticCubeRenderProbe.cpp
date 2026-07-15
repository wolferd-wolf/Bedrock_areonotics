#include "render/DiagnosticCubeRenderProbe.hpp"

#include "render/RenderInterpolation.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <system_error>

#include <sys/syscall.h>
#include <unistd.h>

namespace aeronautics::render {
namespace {

constexpr std::uint64_t sampleIntervalMilliseconds = 2000;

[[nodiscard]] std::int64_t steadyNanosecondsNow() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long raw = ::syscall(SYS_gettid);
    if (raw <= 0 || static_cast<unsigned long>(raw) >
            static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(raw);
}

[[nodiscard]] double micrometersToMeters(std::int64_t value) noexcept {
    return static_cast<double>(value) / 1'000'000.0;
}

}  // namespace

DiagnosticCubeRenderProbe::DiagnosticCubeRenderProbe(
    ll::mod::NativeMod& mod,
    bedrock::LevelRenderBus& renderBus,
    physics::PhysicsScheduler& physicsScheduler) noexcept
    : mMod(mod),
      mRenderBus(renderBus),
      mPhysicsScheduler(physicsScheduler) {}

DiagnosticCubeRenderProbe::~DiagnosticCubeRenderProbe() {
    stop();
}

bool DiagnosticCubeRenderProbe::start() {
    if (mRunning.load(std::memory_order_acquire)) {
        return true;
    }

    mStatusPath = mMod.getDataDir() / "diagnostic-cube-render-status.txt";
    mTimelinePath = mMod.getDataDir() / "diagnostic-cube-render-timeline.txt";
    mStopRequested.store(false, std::memory_order_release);
    mTotalRenderEvents.store(0, std::memory_order_relaxed);
    mPhysicsSnapshotReads.store(0, std::memory_order_relaxed);
    mIncoherentSnapshotReads.store(0, std::memory_order_relaxed);
    mFramesWithActiveWorld.store(0, std::memory_order_relaxed);
    mPhysicsStepTransitions.store(0, std::memory_order_relaxed);
    mLastPhysicsStep.store(0, std::memory_order_relaxed);
    mLastRenderEventSequence.store(0, std::memory_order_relaxed);
    mFirstThreadId.store(0, std::memory_order_relaxed);
    mLastThreadId.store(0, std::memory_order_relaxed);
    mOtherThreadCalls.store(0, std::memory_order_relaxed);
    mFirstRenderer.store(0, std::memory_order_relaxed);
    mLastRenderer.store(0, std::memory_order_relaxed);
    mRendererTransitions.store(0, std::memory_order_relaxed);
    mLastRenderContext.store(0, std::memory_order_relaxed);
    mLastViewRenderObject.store(0, std::memory_order_relaxed);
    mLastClientInstance.store(0, std::memory_order_relaxed);
    mNullRendererEvents.store(0, std::memory_order_relaxed);
    mNullContextEvents.store(0, std::memory_order_relaxed);
    mNullViewEvents.store(0, std::memory_order_relaxed);
    mNullClientEvents.store(0, std::memory_order_relaxed);
    mPreviousPositionYMicrometers.store(0, std::memory_order_relaxed);
    mCurrentPositionYMicrometers.store(0, std::memory_order_relaxed);
    mInterpolatedPositionYMicrometers.store(0, std::memory_order_relaxed);
    mInterpolationAlphaPartsPerMillion.store(0, std::memory_order_relaxed);
    mWorldGeneration.store(0, std::memory_order_relaxed);
    mActiveClientLevel.store(0, std::memory_order_relaxed);
    mGrounded.store(false, std::memory_order_relaxed);
    createTimelineHeader();

    if (!mRenderBus.subscribe(*this)) {
        writeStatus("subscription_failed");
        mMod.getLogger().error(
            "Diagnostic cube render probe could not subscribe to LevelRenderEvent bus");
        return false;
    }
    mSubscribed.store(true, std::memory_order_release);

    try {
        mWriter = std::thread([this] { writerLoop(); });
    } catch (const std::system_error& error) {
        mRenderBus.unsubscribe(*this);
        mSubscribed.store(false, std::memory_order_release);
        writeStatus("writer_start_failed");
        mMod.getLogger().error(
            "Diagnostic cube render telemetry writer failed: {}",
            error.what());
        return false;
    }

    mRunning.store(true, std::memory_order_release);
    writeStatus("waiting_for_level_render_events");
    mMod.getLogger().info(
        "Diagnostic cube render probe started; geometry_submission=disabled_until_render_callback_runtime_proof; interpolation=fixed_20hz_to_render_frames");
    return true;
}

void DiagnosticCubeRenderProbe::stop() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWriter.joinable()) {
        mWriter.join();
    }

    if (mSubscribed.exchange(false, std::memory_order_acq_rel)) {
        mRenderBus.unsubscribe(*this);
    }
    mRunning.store(false, std::memory_order_release);
    writeStatus("stopped");
}

void DiagnosticCubeRenderProbe::onLevelRender(
    const bedrock::LevelRenderEvent& event) noexcept {
    mTotalRenderEvents.fetch_add(1, std::memory_order_relaxed);
    mLastRenderEventSequence.store(event.sequence, std::memory_order_relaxed);

    const std::uint32_t threadId =
        event.threadId != 0 ? event.threadId : currentThreadId();
    mLastThreadId.store(threadId, std::memory_order_relaxed);
    if (threadId != 0) {
        std::uint32_t expected = 0;
        if (!mFirstThreadId.compare_exchange_strong(
                expected,
                threadId,
                std::memory_order_acq_rel,
                std::memory_order_acquire) &&
            expected != threadId) {
            mOtherThreadCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const std::uintptr_t renderer =
        reinterpret_cast<std::uintptr_t>(event.levelRendererCamera);
    const std::uintptr_t context =
        reinterpret_cast<std::uintptr_t>(event.baseActorRenderContext);
    const std::uintptr_t view =
        reinterpret_cast<std::uintptr_t>(event.viewRenderObject);
    const std::uintptr_t client =
        reinterpret_cast<std::uintptr_t>(event.clientInstance);

    if (renderer == 0) {
        mNullRendererEvents.fetch_add(1, std::memory_order_relaxed);
    }
    if (context == 0) {
        mNullContextEvents.fetch_add(1, std::memory_order_relaxed);
    }
    if (view == 0) {
        mNullViewEvents.fetch_add(1, std::memory_order_relaxed);
    }
    if (client == 0) {
        mNullClientEvents.fetch_add(1, std::memory_order_relaxed);
    }

    std::uintptr_t firstRenderer = 0;
    mFirstRenderer.compare_exchange_strong(
        firstRenderer,
        renderer,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    const std::uintptr_t previousRenderer =
        mLastRenderer.exchange(renderer, std::memory_order_acq_rel);
    if (previousRenderer != 0 && renderer != 0 && previousRenderer != renderer) {
        mRendererTransitions.fetch_add(1, std::memory_order_relaxed);
    }
    mLastRenderContext.store(context, std::memory_order_relaxed);
    mLastViewRenderObject.store(view, std::memory_order_relaxed);
    mLastClientInstance.store(client, std::memory_order_relaxed);

    const physics::PhysicsRenderSnapshot snapshot =
        mPhysicsScheduler.renderSnapshot();
    mPhysicsSnapshotReads.fetch_add(1, std::memory_order_relaxed);
    if (!snapshot.coherent) {
        mIncoherentSnapshotReads.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (snapshot.activeClientLevel != 0 && snapshot.simulationStep != 0) {
        mFramesWithActiveWorld.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t previousStep =
        mLastPhysicsStep.exchange(snapshot.simulationStep, std::memory_order_acq_rel);
    if (previousStep != 0 && snapshot.simulationStep != previousStep) {
        mPhysicsStepTransitions.fetch_add(1, std::memory_order_relaxed);
    }

    const InterpolatedVerticalTransform interpolated = RenderInterpolation::sample(
        snapshot.previousPositionYMicrometers,
        snapshot.currentPositionYMicrometers,
        steadyNanosecondsNow(),
        snapshot.lastPhysicsTickNanoseconds);

    mPreviousPositionYMicrometers.store(
        snapshot.previousPositionYMicrometers,
        std::memory_order_relaxed);
    mCurrentPositionYMicrometers.store(
        snapshot.currentPositionYMicrometers,
        std::memory_order_relaxed);
    mInterpolatedPositionYMicrometers.store(
        interpolated.positionYMicrometers,
        std::memory_order_relaxed);
    mInterpolationAlphaPartsPerMillion.store(
        interpolated.alphaPartsPerMillion,
        std::memory_order_relaxed);
    mWorldGeneration.store(snapshot.worldGeneration, std::memory_order_relaxed);
    mActiveClientLevel.store(snapshot.activeClientLevel, std::memory_order_relaxed);
    mGrounded.store(snapshot.grounded, std::memory_order_relaxed);
}

void DiagnosticCubeRenderProbe::writerLoop() {
    const auto startedAt = std::chrono::steady_clock::now();
    std::uint64_t sequence = 0;

    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(sampleIntervalMilliseconds));
        if (mStopRequested.load(std::memory_order_acquire)) {
            break;
        }

        ++sequence;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt);
        appendTimeline(sequence, static_cast<std::uint64_t>(elapsed.count()));
        writeStatus(
            mTotalRenderEvents.load(std::memory_order_relaxed) == 0
                ? "waiting_for_level_render_events"
                : "sampling_interpolated_cube_transform");
    }
}

void DiagnosticCubeRenderProbe::createTimelineHeader() noexcept {
    std::ofstream output(mTimelinePath, std::ios::trunc);
    if (output) {
        output << "schema=1\n"
               << "interval_ms=" << sampleIntervalMilliseconds << "\n"
               << "geometry_submission=disabled_in_render_callback_validation_build\n"
               << "interpolation=fixed_20hz_to_render_frames\n"
               << "columns=sequence,elapsed_ms,total_render_events,frames_with_active_world,physics_snapshot_reads,incoherent_snapshot_reads,physics_step_transitions,last_physics_step,world_generation,active_clientlevel,previous_y_um,current_y_um,interpolated_y_um,alpha_ppm,grounded,first_thread_id,last_thread_id,other_thread_calls,renderer_transitions\n";
    }
}

void DiagnosticCubeRenderProbe::appendTimeline(
    std::uint64_t sequence,
    std::uint64_t elapsedMs) noexcept {
    std::ofstream output(mTimelinePath, std::ios::app);
    if (!output) {
        return;
    }

    output << sequence << ','
           << elapsedMs << ','
           << mTotalRenderEvents.load(std::memory_order_relaxed) << ','
           << mFramesWithActiveWorld.load(std::memory_order_relaxed) << ','
           << mPhysicsSnapshotReads.load(std::memory_order_relaxed) << ','
           << mIncoherentSnapshotReads.load(std::memory_order_relaxed) << ','
           << mPhysicsStepTransitions.load(std::memory_order_relaxed) << ','
           << mLastPhysicsStep.load(std::memory_order_relaxed) << ','
           << mWorldGeneration.load(std::memory_order_relaxed) << ",0x"
           << std::hex << mActiveClientLevel.load(std::memory_order_relaxed)
           << std::dec << ','
           << mPreviousPositionYMicrometers.load(std::memory_order_relaxed) << ','
           << mCurrentPositionYMicrometers.load(std::memory_order_relaxed) << ','
           << mInterpolatedPositionYMicrometers.load(std::memory_order_relaxed) << ','
           << mInterpolationAlphaPartsPerMillion.load(std::memory_order_relaxed) << ','
           << (mGrounded.load(std::memory_order_relaxed) ? 1 : 0) << ','
           << mFirstThreadId.load(std::memory_order_relaxed) << ','
           << mLastThreadId.load(std::memory_order_relaxed) << ','
           << mOtherThreadCalls.load(std::memory_order_relaxed) << ','
           << mRendererTransitions.load(std::memory_order_relaxed) << '\n';
}

void DiagnosticCubeRenderProbe::writeStatus(std::string_view state) noexcept {
    std::ofstream output(mStatusPath, std::ios::trunc);
    if (!output) {
        return;
    }

    const std::int64_t interpolated =
        mInterpolatedPositionYMicrometers.load(std::memory_order_relaxed);

    output << "schema=1\n"
           << "state=" << state << '\n'
           << "probe=diagnostic_cube_render_transform\n"
           << "render_event_type=LevelRenderEvent\n"
           << "render_source=LevelRendererCamera::render\n"
           << "geometry_submission=disabled_in_render_callback_validation_build\n"
           << "visible_cube_expected=false\n"
           << "next_geometry_mode=world_space_wireframe_cube\n"
           << "planned_placement=camera_relative_5_blocks_forward_10_blocks_up\n"
           << "interpolation=fixed_20hz_to_render_frames\n"
           << "physics_step_ms=50\n"
           << "render_bus_listener_count=" << mRenderBus.listenerCount() << '\n'
           << "render_bus_published_events=" << mRenderBus.publishedEvents() << '\n'
           << "render_bus_delivered_callbacks=" << mRenderBus.deliveredCallbacks() << '\n'
           << "total_render_events="
           << mTotalRenderEvents.load(std::memory_order_relaxed) << '\n'
           << "frames_with_active_world="
           << mFramesWithActiveWorld.load(std::memory_order_relaxed) << '\n'
           << "physics_snapshot_reads="
           << mPhysicsSnapshotReads.load(std::memory_order_relaxed) << '\n'
           << "incoherent_snapshot_reads="
           << mIncoherentSnapshotReads.load(std::memory_order_relaxed) << '\n'
           << "physics_step_transitions="
           << mPhysicsStepTransitions.load(std::memory_order_relaxed) << '\n'
           << "last_physics_step="
           << mLastPhysicsStep.load(std::memory_order_relaxed) << '\n'
           << "world_generation="
           << mWorldGeneration.load(std::memory_order_relaxed) << '\n'
           << "active_clientlevel=0x" << std::hex
           << mActiveClientLevel.load(std::memory_order_relaxed) << std::dec << '\n'
           << "previous_position_y_um="
           << mPreviousPositionYMicrometers.load(std::memory_order_relaxed) << '\n'
           << "current_position_y_um="
           << mCurrentPositionYMicrometers.load(std::memory_order_relaxed) << '\n'
           << "interpolated_position_y_um=" << interpolated << '\n'
           << std::fixed << std::setprecision(6)
           << "interpolated_position_y_m=" << micrometersToMeters(interpolated) << '\n'
           << "interpolation_alpha_ppm="
           << mInterpolationAlphaPartsPerMillion.load(std::memory_order_relaxed) << '\n'
           << "grounded="
           << (mGrounded.load(std::memory_order_relaxed) ? "true" : "false") << '\n'
           << "first_thread_id="
           << mFirstThreadId.load(std::memory_order_relaxed) << '\n'
           << "last_thread_id="
           << mLastThreadId.load(std::memory_order_relaxed) << '\n'
           << "other_thread_calls="
           << mOtherThreadCalls.load(std::memory_order_relaxed) << '\n'
           << "first_renderer=0x" << std::hex
           << mFirstRenderer.load(std::memory_order_relaxed) << '\n'
           << "last_renderer=0x"
           << mLastRenderer.load(std::memory_order_relaxed) << std::dec << '\n'
           << "renderer_transitions="
           << mRendererTransitions.load(std::memory_order_relaxed) << '\n'
           << "last_render_context=0x" << std::hex
           << mLastRenderContext.load(std::memory_order_relaxed) << '\n'
           << "last_view_render_object=0x"
           << mLastViewRenderObject.load(std::memory_order_relaxed) << '\n'
           << "last_client_instance=0x"
           << mLastClientInstance.load(std::memory_order_relaxed) << std::dec << '\n'
           << "null_renderer_events="
           << mNullRendererEvents.load(std::memory_order_relaxed) << '\n'
           << "null_context_events="
           << mNullContextEvents.load(std::memory_order_relaxed) << '\n'
           << "null_view_events="
           << mNullViewEvents.load(std::memory_order_relaxed) << '\n'
           << "null_client_events="
           << mNullClientEvents.load(std::memory_order_relaxed) << '\n'
           << "timeline_file=diagnostic-cube-render-timeline.txt\n";
}

}  // namespace aeronautics::render
