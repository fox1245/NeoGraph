/**
 * @file a2a/agent_card_candidate.h
 * @brief Experimental, authority-free Agent Card compatibility candidates.
 *
 * An A2A Agent Card is an untrusted discovery document, not source code or a
 * delegation grant.  This PoC deliberately separates three stages:
 *
 *   1. collect one operator-selected public card without sending an A2A RPC;
 *   2. compile an immutable, unadmitted compatibility candidate with no copied
 *      identity, endpoint, credential, or executable source text; and
 *   3. materialize one local deterministic harness only after an independently
 *      supplied behavioral profile matches the collected-card digest.
 *
 * It does not crawl, follow card-provided endpoints, execute remote source,
 * obtain credentials, or establish behavioral equivalence from card text.
 */
#pragma once

#include <neograph/a2a/types.h>
#include <neograph/api.h>
#include <neograph/graph/node.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::a2a {

/** Provenance declared by the operator for one open-source Agent Card. */
struct NEOGRAPH_API AgentCardSourceProvenance {
    /// Exact origin fetched through the A2A well-known discovery path.
    std::string discovery_url;
    /// Immutable source location reviewed by the operator; it is never fetched here.
    std::string source_url;
    /// Immutable source revision, such as a Git commit SHA.
    std::string source_revision;
    /// SPDX-style source license declaration, such as "Apache-2.0".
    std::string source_license;
};

/** Collection limits for the deliberately narrow proof of concept. */
struct NEOGRAPH_API AgentCardCollectionPolicy {
    /// Size limit applied before the card becomes a durable collection record.
    std::size_t max_card_bytes = 64 * 1024;
    /// Test-only escape hatch for an explicitly selected loopback HTTP endpoint.
    bool allow_loopback_http = false;
};

/** One card record with an immutable content identity and reviewed provenance. */
struct NEOGRAPH_API CollectedAgentCard {
    AgentCard                 card;
    AgentCardSourceProvenance provenance;
    std::string               card_sha256;
    json                      raw_card;
};

/**
 * Fetches one explicit A2A well-known card.
 *
 * The collector accepts HTTPS origins by default.  It permits plain HTTP only
 * for an explicit loopback test endpoint, never follows redirects or card
 * interface URLs, and never attaches an Authorization header.
 */
class NEOGRAPH_API AgentCardCollector {
public:
    explicit AgentCardCollector(AgentCardCollectionPolicy policy = {});

    CollectedAgentCard collect(std::string               discovery_url,
                               AgentCardSourceProvenance provenance) const;

private:
    AgentCardCollectionPolicy policy_;
};

class AgentCardCandidateCompiler;

/**
 * Non-executable compatibility candidate compiled from one collection record.
 *
 * `descriptor` intentionally contains only bounded protocol facts, safe skill
 * identifiers, digest-pinned provenance, and explicit non-authority state. It
 * excludes the source card's free-form text, declared endpoint, provider,
 * security schemes, and all credentials.
 *
 * Instances are factory-only and expose const views; callers cannot rebind or
 * alter a compiled candidate into a different local advertisement.
 */
class NEOGRAPH_API AgentCardCompatibilityCandidate {
public:
    AgentCardCompatibilityCandidate(const AgentCardCompatibilityCandidate&)            = default;
    AgentCardCompatibilityCandidate(AgentCardCompatibilityCandidate&&)                 = default;
    AgentCardCompatibilityCandidate& operator=(const AgentCardCompatibilityCandidate&) = delete;
    AgentCardCompatibilityCandidate& operator=(AgentCardCompatibilityCandidate&&)      = delete;

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& source_card_sha256() const noexcept {
        return source_card_sha256_;
    }
    [[nodiscard]] const json& descriptor() const noexcept { return descriptor_; }

private:
    friend class AgentCardCandidateCompiler;

    AgentCardCompatibilityCandidate(std::string id,
                                    std::string source_card_sha256,
                                    json        descriptor);

    std::string id_;
    std::string source_card_sha256_;
    json        descriptor_;
};

/** Compiles a collected card into an unadmitted, text-only compatibility candidate. */
class NEOGRAPH_API AgentCardCandidateCompiler {
public:
    static AgentCardCompatibilityCandidate compile(const CollectedAgentCard& collected);
};

/** One independently observed development probe for a deterministic PoC template. */
struct NEOGRAPH_API CopyNinjaBehavioralProbe {
    std::string input;
    std::string expected_output;
};

/**
 * Behavioral evidence that is deliberately separate from an Agent Card.
 *
 * The PoC supports only `copy-ninja.hello-world-echo.v1`.  A card never
 * selects a template, injects code, or supplies a probe expectation.
 */
struct NEOGRAPH_API CopyNinjaBehavioralProfile {
    std::string                           source_card_sha256;
    std::string                           template_id;
    std::vector<CopyNinjaBehavioralProbe> development_probes;
};

/**
 * A local, separately materialized deterministic compatibility harness.
 *
 * It has no transport, credentials, or source-agent connection.  An embedding
 * application may expose its `respond()` function through a fresh A2A server
 * after normal admission and activation controls have accepted it.
 */
class NEOGRAPH_API CopyNinjaHarness {
public:
    const AgentCardCompatibilityCandidate& candidate() const noexcept;
    const CopyNinjaBehavioralProfile&      profile() const noexcept;

    /// Execute only the fixed, local template admitted by materialization.
    std::string respond(std::string_view input) const;

    /// Build a fresh, non-impersonating A2A card for a caller-owned endpoint.
    AgentCard agent_card(std::string endpoint) const;

private:
    friend CopyNinjaHarness materialize_copy_ninja(const AgentCardCompatibilityCandidate& candidate,
                                                   CopyNinjaBehavioralProfile             profile);

    CopyNinjaHarness(AgentCardCompatibilityCandidate candidate, CopyNinjaBehavioralProfile profile);

    AgentCardCompatibilityCandidate candidate_;
    CopyNinjaBehavioralProfile      profile_;
};

/**
 * @brief Local GraphNode adapter for a materialized Copy Ninja harness.
 *
 * This node has no transport, credentials, or remote-agent dependency. It
 * reads the `prompt` channel and overwrites the `response` channel with the
 * fixed locally materialized behavior.
 */
class NEOGRAPH_API CopyNinjaNode final : public graph::GraphNode {
public:
    CopyNinjaNode(std::string name, std::shared_ptr<const CopyNinjaHarness> harness);
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput input) override;

    std::string get_name() const override;

private:
    std::string                             name_;
    std::shared_ptr<const CopyNinjaHarness> harness_;
};

/**
 * Materialize a fixed local template after its independently supplied
 * development probes match.  The returned harness remains unadmitted.
 */
NEOGRAPH_API CopyNinjaHarness materialize_copy_ninja(
    const AgentCardCompatibilityCandidate& candidate, CopyNinjaBehavioralProfile profile);

}  // namespace neograph::a2a
