#include "../../firmware/gm/ProgramChangeMailbox.h"
#include "../../firmware/sample/ProgramSnapshot.h"

#include <cstdint>
#include <cstdio>

using gm_synth::gm::ProgramChangeMailbox;
using gm_synth::gm::ProgramChangeRequest;
using gm_synth::sample::ProgramSnapshot;
using gm_synth::sample::ProgramSnapshotExchange;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

ProgramChangeRequest request(
    std::uint8_t channel,
    std::uint8_t program,
    std::uint32_t token) {
    ProgramChangeRequest out{};
    out.channel = channel;
    out.bank = 0;
    out.program = program;
    out.token = token;
    return out;
}

ProgramSnapshot snapshot(
    std::uint8_t channel,
    std::uint8_t program,
    std::uint32_t token) {
    ProgramSnapshot out{};
    out.channel = channel;
    out.bank = 0;
    out.program = program;
    out.token = token;
    out.bank_generation = 1;
    out.dependency_count = 1;
    out.sample_ids[0] = program;
    return out;
}

void test_latest_request_supersedes_intermediate_requests() {
    ProgramChangeMailbox mailbox;

    CHECK(mailbox.submit(request(0, 16, 100)));
    CHECK(mailbox.submit(request(0, 48, 101)));
    CHECK(mailbox.submit(request(0, 56, 102)));

    ProgramChangeRequest pending{};
    CHECK(mailbox.peek(0, &pending));
    CHECK(pending.program == 56);
    CHECK(pending.token == 102);

    CHECK(mailbox.take(0, &pending));
    CHECK(pending.program == 56);
    CHECK(pending.token == 102);
    CHECK(!mailbox.hasPending(0));
    CHECK(!mailbox.take(0, &pending));

    const auto stats = mailbox.stats();
    CHECK(stats.submitted == 3);
    CHECK(stats.superseded == 2);
    CHECK(stats.taken == 1);
    CHECK(stats.rejected == 0);
}

void test_channels_coalesce_independently() {
    ProgramChangeMailbox mailbox;
    CHECK(mailbox.submit(request(0, 8, 200)));
    CHECK(mailbox.submit(request(1, 24, 201)));
    CHECK(mailbox.submit(request(0, 32, 202)));

    ProgramChangeRequest ch0{};
    ProgramChangeRequest ch1{};
    CHECK(mailbox.take(0, &ch0));
    CHECK(mailbox.take(1, &ch1));
    CHECK(ch0.program == 32);
    CHECK(ch1.program == 24);
    CHECK(ch0.token == 202);
    CHECK(ch1.token == 201);
}

void test_invalid_request_is_rejected_without_overwriting_pending() {
    ProgramChangeMailbox mailbox;
    CHECK(mailbox.submit(request(2, 40, 300)));

    ProgramChangeRequest invalid = request(2, 41, 0);
    CHECK(!mailbox.submit(invalid));

    ProgramChangeRequest pending{};
    CHECK(mailbox.take(2, &pending));
    CHECK(pending.program == 40);
    CHECK(pending.token == 300);
    CHECK(mailbox.stats().rejected == 1);
}

void test_busy_exchange_keeps_only_latest_desired_identity() {
    ProgramSnapshotExchange exchange;
    ProgramChangeMailbox mailbox;

    const ProgramSnapshot active = snapshot(0, 0, 400);
    const ProgramSnapshot ready = snapshot(0, 16, 401);
    CHECK(exchange.setInitialActive(&active));
    CHECK(exchange.publishReady(&ready));
    CHECK(exchange.transitionInFlight(0));

    // While Organ is being committed, user/MIDI rapidly selects Strings, Pad,
    // then Brass. The mailbox stores identity only; dependencies are resolved
    // later after the in-flight transition is acknowledged.
    CHECK(mailbox.submit(request(0, 48, 402)));
    CHECK(mailbox.submit(request(0, 88, 403)));
    CHECK(mailbox.submit(request(0, 56, 404)));

    CHECK(exchange.commitAtBlockBoundary(0));
    CHECK(exchange.acknowledgeCommit(0, 401).valid());
    CHECK(!exchange.transitionInFlight(0));

    ProgramChangeRequest next{};
    CHECK(mailbox.take(0, &next));
    CHECK(next.program == 56);
    CHECK(next.token == 404);
    CHECK(mailbox.stats().superseded == 2);
}

}  // namespace

int main() {
    test_latest_request_supersedes_intermediate_requests();
    test_channels_coalesce_independently();
    test_invalid_request_is_rejected_without_overwriting_pending();
    test_busy_exchange_keeps_only_latest_desired_identity();

    if (failures != 0) {
        std::fprintf(stderr, "%d program-change mailbox checks failed\n", failures);
        return 1;
    }
    std::printf("program-change mailbox host checks passed\n");
    return 0;
}
