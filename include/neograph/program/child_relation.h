/**
 * @file program/child_relation.h
 * @brief Durable per-child relation rows of a Program run (#661).
 *
 * A parent run does not carry its children in its own record. Each child is one relation row keyed by
 * (owner scope, parent logical run id, child run id), and one small head per parent holds the relation epoch,
 * the counts and the permanent budget aggregate. A store changes a row and the head in one transaction, so a
 * change of one child never reads or rewrites the other children.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/result.h>
#include <neograph/program/run_record.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Canonical JSON of one child record, for hosts and publications that carry a row's content. */
NEOGRAPH_PROGRAM_API std::string serialize_program_child_record(const ProgramChildRecord& child);
NEOGRAPH_PROGRAM_API ProgramChildRecord parse_program_child_record(std::string_view stored_bytes);

/** One child of one parent run. */
struct ProgramChildRelationData {
    std::string owner_scope;
    /// The parent's logical run id: a replacement generation of the parent inherits its relations.
    std::string parent_run_id;
    /// Position in the order the children were attached, from 0; equals the head's count before the insert.
    std::uint64_t ordinal = 0;
    /// Identity, link, invocation and granted budget never change after the insert; the state, the terminal
    /// result and its generation follow the child.
    ProgramChildRecord child;
    /// 1 on insert, +1 on every update.
    std::uint64_t row_version = 1;
};

/** Canonical, content-addressed relation row. */
class NEOGRAPH_PROGRAM_API ProgramChildRelation final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ProgramChildRelation create(ProgramChildRelationData data);
    static ProgramChildRelation parse(std::string_view stored_bytes);

    const ProgramChildRelationData& data() const noexcept;
    const std::string&              owner_scope() const noexcept;
    const std::string&              parent_run_id() const noexcept;
    const std::string&              child_run_id() const noexcept;
    std::uint64_t                   ordinal() const noexcept;
    std::uint64_t                   row_version() const noexcept;
    const ProgramChildRecord&       child() const noexcept;
    /** True while the child can still change state: Publishing or Dispatched. */
    bool                            is_open() const noexcept;
    const std::string&              id() const noexcept;
    std::string                     serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramChildRelation(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ProgramChildRelationHeadData {
    std::string   owner_scope;
    std::string   parent_run_id;
    /// +1 on every relation insert, every relation update and the close; never decreases.
    std::uint64_t epoch      = 0;
    std::uint64_t count      = 0;
    /// Rows whose child is still Publishing or Dispatched.
    std::uint64_t open_count = 0;
    /// The sum over the rows of the child's permanent reservation: its granted wall time, model tokens,
    /// money, operations, Core steps and dynamic compiles, and its granted child count plus one.
    RunBudget committed_budget;
    /// Set by the parent's final publication: a closed head refuses new rows and the dispatch of a row that
    /// is still Publishing. A child that was already dispatched still records its terminal result.
    bool        closed = false;
    /// The journal record id of the parent publication that closed the head; empty while open.
    std::string closed_by;
};

/** Canonical, content-addressed head of one parent's relation rows. */
class NEOGRAPH_PROGRAM_API ProgramChildRelationHead final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    /** The head of a parent that has no rows: epoch 0, nothing counted, open. */
    static ProgramChildRelationHead empty(std::string_view owner_scope, std::string_view parent_run_id);
    static ProgramChildRelationHead create(ProgramChildRelationHeadData data);
    static ProgramChildRelationHead parse(std::string_view stored_bytes);

    const ProgramChildRelationHeadData& data() const noexcept;
    std::uint64_t                       epoch() const noexcept;
    std::uint64_t                       count() const noexcept;
    std::uint64_t                       open_count() const noexcept;
    const RunBudget&                    committed_budget() const noexcept;
    bool                                closed() const noexcept;
    const std::string&                  id() const noexcept;
    std::string                         serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramChildRelationHead(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Why a relation write was refused; None for an accepted write. */
enum class ProgramChildRelationRefusal : std::uint8_t {
    None,
    /// The parent run is not stored, or its lineage has no active generation.
    ParentAbsent,
    /// An insert or a dispatch needs a Running parent.
    ParentNotRunning,
    /// The writer's fence differs from the parent's stored fence: the parent was taken over.
    FenceMismatch,
    /// The relation head is closed: no insert and no dispatch of a Publishing row.
    Closed,
    /// The row exists with other identity, link, invocation or granted budget.
    RowConflict,
    /// An update names a row that is not stored.
    RowMissing,
    /// The expected row version differs from the stored one.
    RowVersionMismatch,
    /// The state change is not allowed (a terminal state is final) or its result does not fit the state.
    InvalidTransition,
    /// The child's permanent reservation does not fit the parent's remaining budget.
    BudgetExceeded,
    /// The terminal result does not bind the child's active generation.
    ResultUnbound,
    /// The row or the child record is malformed for this write.
    Invalid,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramChildRelationRefusal refusal) noexcept;

enum class ProgramChildRelationStatus : std::uint8_t {
    /// The write changed the row and the head.
    Applied,
    /// The row already holds exactly this write; nothing changed.
    AlreadyPresent,
    Refused,
};

/** The result of one standalone relation write, with the state the write left or found. */
struct ProgramChildRelationOutcome {
    ProgramChildRelationStatus                 status  = ProgramChildRelationStatus::Refused;
    ProgramChildRelationRefusal                refusal = ProgramChildRelationRefusal::Invalid;
    /// The row after an applied write, the stored row otherwise (absent when none is stored).
    std::optional<ProgramChildRelation>        relation;
    /// The head after an applied write, the stored head otherwise (absent when the parent has no rows).
    std::optional<ProgramChildRelationHead>    head;
};

/**
 * The record with the children of its run attached as its snapshot overlay (`ProgramRunRecord::snapshot_children`),
 * from the relation rows that a store holds for the run's logical run id, in the order the rows were attached. A
 * record read from a store carries no children of its own; every API that returns a record for a caller to look at
 * (`ProgramHandle::snapshot`, a host's read of a stored record) fills it this way.
 */
NEOGRAPH_PROGRAM_API ProgramRunRecord with_child_relations(const ProgramRunRecord& record,
                                                           const std::vector<ProgramChildRelation>& rows);

}  // namespace neograph::program
