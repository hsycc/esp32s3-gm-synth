#include "gm/PlaybackProgramSnapshot.h"
#include "sf2/Sf2PdtaReader.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"
#include "sf2/Sf2ResolvedZone.h"
#include "storage/RandomAccessReader.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::sample::SampleId;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::NativeZoneFlagStereoOrLinkedSample;
using gm_synth::sf2::NativeZoneFlagUnlinkedStereoSideFallback;
using gm_synth::sf2::PlaybackPreparePolicy;
using gm_synth::sf2::PlaybackPrepareRequest;
using gm_synth::sf2::PlaybackPrepareStatus;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::ResolveProgramResult;
using gm_synth::sf2::ResolveProgramStatus;
using gm_synth::sf2::Sf2PdtaReader;
using gm_synth::sf2::Sf2PlaybackProgramPreparer;
using gm_synth::sf2::Sf2ResolvedZoneResolver;
using gm_synth::storage::RandomAccessReader;

class FileReader final : public RandomAccessReader {
public:
    explicit FileReader(const char* path) : file_(path, std::ios::binary) {
        if (!file_) {
            return;
        }
        file_.seekg(0, std::ios::end);
        const std::streamoff end = file_.tellg();
        if (end >= 0) {
            size_ = static_cast<std::uint64_t>(end);
        }
        file_.clear();
    }

    bool valid() const { return file_.is_open() && size_ != 0; }
    std::uint64_t size() const override { return size_; }

    bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override {
        if (!valid() || destination == nullptr || offset > size_ || bytes > size_ - offset) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file_) {
            return false;
        }
        file_.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
        return file_.gcount() == static_cast<std::streamsize>(bytes);
    }

private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
};

}  // namespace

int main() {
    // FluidR3's checked-in deterministic subset intentionally preserves its
    // common shdr quirk: leftSample/rightSample records have sampleLink=0 and
    // are referenced independently by the instrument zones. Program 72
    // (Piccolo) is entirely built from those side records.
    FileReader reader("assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2");
    assert(reader.valid());

    Sf2PdtaReader pdta;
    assert(pdta.begin(reader));

    std::array<NativeResolvedZone, PlaybackProgramSnapshot::kMaxZones> native{};
    ResolveProgramResult resolved{};
    Sf2ResolvedZoneResolver resolver;
    assert(resolver.resolveProgram(
               pdta,
               0,
               72,
               native.data(),
               native.size(),
               &resolved) == ResolveProgramStatus::Ok);
    assert(resolved.zone_count == 144);

    std::size_t fallback_zones = 0;
    std::size_t left_panned = 0;
    std::size_t right_panned = 0;
    for (std::size_t index = 0; index < resolved.zone_count; ++index) {
        const NativeResolvedZone& zone = native[index];
        if ((zone.flags & NativeZoneFlagUnlinkedStereoSideFallback) == 0) {
            continue;
        }
        ++fallback_zones;
        assert((zone.flags & NativeZoneFlagStereoOrLinkedSample) == 0);
        assert((zone.sample_type & 0x7fffu) == 1u);
        assert(zone.sample_link == 0u);
        if (zone.panSf2Units() < 0) {
            ++left_panned;
        }
        if (zone.panSf2Units() > 0) {
            ++right_panned;
        }
    }
    assert(fallback_zones == resolved.zone_count);
    assert(left_panned != 0);
    assert(right_panned != 0);

    std::array<ResolvedZoneModulation, PlaybackProgramSnapshot::kMaxZones> modulation{};
    std::array<PlaybackZone, PlaybackProgramSnapshot::kMaxZones> playback{};
    std::array<SampleId, PlaybackProgramSnapshot::kMaxZones> dependencies{};
    PlaybackProgramSnapshot snapshot{};

    PlaybackPrepareRequest request{};
    request.channel = 0;
    request.bank = 0;
    request.program = 72;
    request.token = 1;
    request.bank_generation = 1;
    request.policy = PlaybackPreparePolicy::StrictQualified;
    request.native_scratch = native.data();
    request.native_capacity = native.size();
    request.modulation_scratch = modulation.data();
    request.modulation_capacity = modulation.size();
    request.playback_zones = playback.data();
    request.playback_capacity = playback.size();
    request.dependencies = dependencies.data();
    request.dependency_capacity = dependencies.size();
    request.snapshot = &snapshot;

    const auto prepared = Sf2PlaybackProgramPreparer{}.prepare(pdta, request);
    assert(prepared.status == PlaybackPrepareStatus::Ok);
    assert(prepared.native_zone_count == 144);
    assert(prepared.zone_count == 144);
    assert(prepared.stereo_pair_count == 0);
    assert(prepared.dependency_count == 16);
    assert(prepared.issue_union == 0);
    assert(snapshot.valid());
    assert(snapshot.zone_count == 144);

    // sample 0 is the unrelated mono Gun sample. The old dependency code would
    // incorrectly pull it in because every orphan stereo side carried link=0.
    for (std::size_t index = 0; index < prepared.dependency_count; ++index) {
        assert(dependencies[index] != 0u);
    }

    std::cout << "M7 unlinked stereo-side compatibility checks passed\n";
    return 0;
}
