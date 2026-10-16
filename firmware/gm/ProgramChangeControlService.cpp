#include "ProgramChangeControlService.h"

#include "util/SaturatingCounter.h"

namespace gm_synth::gm {

std::size_t ProgramChangeControlService::drainRealtimeRequests() {
    const std::size_t drained = realtime_handoff_.drainToMailbox(mailbox_);
    util::saturatingAdd(stats_.realtime_requests_drained, drained);
    return drained;
}

bool ProgramChangeControlService::workspaceIsFree(
    std::uint8_t channel,
    const ProgramChangePrepareWorkspace& workspace) const {
    if (!workspace.structurallyValid() || channel >= 16) {
        return false;
    }

    const PlaybackProgramSnapshot* active = playback_exchange_.active(channel);
    const PlaybackProgramSnapshot* ready = playback_exchange_.ready(channel);
    return active != &workspace.snapshot && ready != &workspace.snapshot;
}

ProgramChangeServiceResult ProgramChangeControlService::serviceChannel(
    std::uint8_t channel,
    ProgramChangePrepareWorkspace& workspace) {
    ProgramChangeServiceResult result{};
    util::saturatingIncrement(stats_.service_attempts);

    if (channel >= 16 || !workspaceIsFree(channel, workspace)) {
        result.status = ProgramChangeServiceStatus::InvalidWorkspace;
        return result;
    }

    ProgramChangeRequest request{};
    if (!mailbox_.peek(channel, &request)) {
        result.status = ProgramChangeServiceStatus::NoPending;
        return result;
    }
    result.request = request;

    if (residency_exchange_.transitionInFlight(channel) ||
        playback_exchange_.transitionInFlight(channel)) {
        util::saturatingIncrement(stats_.busy_deferrals);
        result.status = ProgramChangeServiceStatus::TransitionBusy;
        return result;
    }

    const sample::ProgramSnapshot* active_residency = residency_exchange_.active(channel);
    if (active_residency == nullptr || active_residency->bank_generation == 0) {
        result.status = ProgramChangeServiceStatus::NoActiveResidencySnapshot;
        return result;
    }

    // Prepare the long-lived playback snapshot in caller-owned storage first.
    // Nothing is published yet, so a parser/materialization failure is fully
    // transactional and leaves the currently active program untouched.
    sf2::PlaybackPrepareRequest playback_request{};
    playback_request.channel = request.channel;
    playback_request.bank = request.bank;
    playback_request.program = request.program;
    playback_request.token = request.token;
    playback_request.bank_generation = active_residency->bank_generation;
    playback_request.policy = workspace.policy;
    playback_request.native_scratch = workspace.native_scratch;
    playback_request.native_capacity = workspace.native_capacity;
    playback_request.modulation_scratch = workspace.modulation_scratch;
    playback_request.modulation_capacity = workspace.modulation_capacity;
    playback_request.playback_zones = workspace.playback_zones;
    playback_request.playback_capacity = workspace.playback_capacity;
    playback_request.dependencies = workspace.dependencies;
    playback_request.dependency_capacity = workspace.dependency_capacity;
    playback_request.snapshot = &workspace.snapshot;

    result.playback_prepare = playback_preparer_.prepare(pdta_, playback_request);
    if (!result.playback_prepare.ok()) {
        util::saturatingIncrement(stats_.playback_prepare_failures);
        result.status = ProgramChangeServiceStatus::PlaybackPrepareFailed;
        return result;
    }

    // Residency uses the exact unique dependency list emitted by the playback
    // preparer, keeping sample ownership and playback metadata in lockstep.
    if (!residency_preparer_.prepareProgram(
            request.channel,
            request.bank,
            request.program,
            request.token,
            workspace.dependencies,
            result.playback_prepare.dependency_count)) {
        util::saturatingIncrement(stats_.residency_prepare_failures);
        result.residency_error = residency_preparer_.lastError();
        result.status = ProgramChangeServiceStatus::ResidencyPrepareFailed;
        return result;
    }

    // Under the single-control-owner contract this publish cannot fail after
    // the paired preflight above: residency READY alone is not committable by
    // ProgramRuntimeCommitCoordinator, playback had no in-flight transition,
    // and prepare() already validated the snapshot. Treat failure as an
    // invariant violation rather than silently activating mismatched metadata.
    if (!playback_exchange_.publishReady(&workspace.snapshot)) {
        util::saturatingIncrement(stats_.publish_invariant_failures);
        result.status = ProgramChangeServiceStatus::PlaybackPublishInvariantFailed;
        return result;
    }

    (void)mailbox_.clear(channel);
    util::saturatingIncrement(stats_.service_successes);
    result.status = ProgramChangeServiceStatus::Ok;
    return result;
}

ProgramChangeRetireAck ProgramChangeControlService::acknowledgeCommitted(
    std::uint8_t channel) {
    ProgramChangeRetireAck result{};
    if (channel >= 16) {
        util::saturatingIncrement(stats_.ack_failures);
        return result;
    }

    const std::uint32_t playback_token = playback_exchange_.committedToken(channel);
    const std::uint32_t residency_token = residency_exchange_.committedToken(channel);
    if (playback_token == 0 || playback_token != residency_token) {
        result.status = ProgramChangeServiceStatus::CommitNotReady;
        util::saturatingIncrement(stats_.ack_failures);
        return result;
    }

    const sample::ProgramCommitAck residency_ack =
        residency_preparer_.acknowledgeAndRetire(channel, residency_token);
    if (!residency_ack.valid()) {
        result.status = ProgramChangeServiceStatus::AcknowledgeFailed;
        util::saturatingIncrement(stats_.ack_failures);
        return result;
    }

    const PlaybackProgramCommitAck playback_ack =
        playback_exchange_.acknowledgeCommit(channel, playback_token);
    if (!playback_ack.valid()) {
        result.status = ProgramChangeServiceStatus::AcknowledgeFailed;
        util::saturatingIncrement(stats_.ack_failures);
        return result;
    }

    result.status = ProgramChangeServiceStatus::Ok;
    result.token = playback_token;
    result.retired_playback = playback_ack.retired;
    util::saturatingIncrement(stats_.commit_acks);
    return result;
}

}  // namespace gm_synth::gm
