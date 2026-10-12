// NeoGraph Example 79: two agents that collaborate through a link and a mailbox
//
// Two agents owned by different parties agree on a collaboration link before they exchange
// anything. The sender proposes it, the receiver accepts it with a consent token, and only
// then can envelopes be built and delivered. An envelope carries a sequence number and an
// idempotency key, so delivering the same one twice has no second effect, and delivering a
// different message under a used key is a conflict. The mailbox is transport neutral: over A2A
// an envelope travels as an ordinary message, which the last step shows. No server is started.
//
// smoke-expect: link: proposed
// smoke-expect: envelope before consent: refused
// smoke-expect: link: accepted
// smoke-expect: seq 1: accepted
// smoke-expect: seq 1 again: duplicate
// smoke-expect: seq 1 with other content: conflict
// smoke-expect: seq 2: accepted
// smoke-expect: applied progress: 40
// smoke-expect: wire format: neograph-a2a-collaboration-v1
// smoke-forbid: [neograph] lint

#include <neograph/a2a/collaboration.h>

#include <iostream>
#include <string>

using namespace neograph;
using namespace neograph::a2a;

int main() {
    try {
        // The sender's proposal: who talks to whom, about what, and which message kinds may cross.
        CollaborationLinkSpec spec;
        spec.link_id = "release-link";
        spec.sender_owner_scope = "team-a";
        spec.receiver_owner_scope = "team-b";
        spec.sender_agent_id = "planner";
        spec.receiver_agent_id = "builder";
        spec.task_scope = "release-1.4";
        spec.cancellation_rights = {"sender"};
        spec.message_kind_allowlist = {"progress", "completed"};
        spec.expires_at_unix_ms = 4102444800000ULL;  // far in the future (year 2100)
        const auto proposed = CollaborationLink::create(spec);
        std::cout << "link: " << to_string(proposed.state()) << "\n";

        // A proposed link carries no traffic.
        try {
            CollaborationEnvelope::create(proposed, "run-a", "run-b", "task-1", "context-1", "m1", "c1", 1,
                                          "progress", "key-1", json{{"percent", 10}});
        } catch (const std::invalid_argument&) {
            std::cout << "envelope before consent: refused\n";
        }

        // The receiver accepts and registers the accepted link in its own mailbox.
        const auto accepted = proposed.accept("builder", "consent-for-release-link");
        std::cout << "link: " << to_string(accepted.state()) << "\n";
        CollaborationMailbox mailbox("team-b", "builder");
        mailbox.accept_link(accepted, "consent-for-release-link");

        // The receiver applies a payload only when the mailbox says Accepted.
        int applied_progress = 0;
        auto deliver = [&](const std::string& label, const CollaborationEnvelope& envelope) {
            const auto result = mailbox.submit(envelope);
            std::cout << label << ": " << to_string(result) << "\n";
            if (result == CollaborationSubmitResult::Accepted)
                applied_progress += envelope.payload.at("percent").get<int>();
        };
        auto envelope = [&](std::uint64_t sequence, const std::string& key, int percent) {
            return CollaborationEnvelope::create(accepted, "run-a", "run-b", "task-1", "context-1",
                                                 "message-" + std::to_string(sequence), "correlation-1",
                                                 sequence, "progress", key, json{{"percent", percent}});
        };

        const auto first = envelope(1, "key-1", 10);
        deliver("seq 1", first);
        deliver("seq 1 again", first);                    // a redelivery: no second effect
        deliver("seq 1 with other content", envelope(1, "key-1", 99));  // same key, different message
        deliver("seq 2", envelope(2, "key-2", 30));
        std::cout << "applied progress: " << applied_progress << "\n";

        // Over A2A the envelope is an ordinary message with a data part.
        const auto message = collaboration_to_message(first);
        std::cout << "wire format: " << message.parts[0].data.at("format").get<std::string>() << "\n";
        const bool same = collaboration_from_message(message).content_hash() == first.content_hash();
        std::cout << "round trip keeps the envelope: " << std::boolalpha << same << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
