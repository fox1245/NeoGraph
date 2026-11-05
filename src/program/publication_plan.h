#pragma once

/**
 * @file publication_plan.h
 * @brief The rows a publication locks, named from the typed fields of its request.
 *
 * Store independent. A store that reads in ordered groups takes the locks of one group in the order of its ORDER BY
 * and the groups in the order of the members below: lineage heads, run heads, relation heads, relation rows. That is
 * the single lock order of a publication; a read the plan does not cover is a late lock.
 */

#include "transition_validator.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace neograph::program::detail::transition {

struct PublicationLockPlan {
    /// Group 1: lineage heads. Every vector is sorted ascending and free of duplicates.
    std::vector<std::string> lineage_ids;
    /// Group 2: run heads (each with its execution lease and lineage association).
    std::vector<std::string> run_ids;
    /// Group 3a: relation heads, by the logical id of the parent.
    std::vector<std::string> relation_parent_ids;
    /// Group 3b: relation rows, by parent and child.
    std::vector<std::pair<std::string, std::string>> relation_rows;
};

namespace plan_detail {

template <typename T>
void sort_unique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

}  // namespace plan_detail

/**
 * The rows the validator reads for `request` under a lock, from the typed parts of the publication only: its own run
 * and lineage, the fork and replay source lineages, and the parent and child that a relation part names.
 */
inline PublicationLockPlan plan_publication_locks(const TransitionRequest& request) {
    const auto&         publication = request.publication;
    const std::string_view owner    = request.owner_scope;
    PublicationLockPlan plan;

    if (publication.run_lineage) plan.lineage_ids.push_back(publication.run_lineage->lineage_id());
    if (publication.fork_source_lineage)
        plan.lineage_ids.push_back(publication.fork_source_lineage->lineage_id());
    if (publication.recorded_replay_source_lineage)
        plan.lineage_ids.push_back(publication.recorded_replay_source_lineage->lineage_id());

    plan.run_ids.push_back(publication.run_record.run_id());
    if (publication.recorded_replay_source) plan.run_ids.push_back(publication.recorded_replay_source->run_id);
    plan.relation_parent_ids.push_back(publication.run_record.logical_run_id());

    if (publication.child_relations) {
        if (const auto& spawn = publication.child_relations->spawn) {
            plan.lineage_ids.push_back(program_run_lineage_id(owner, spawn->parent_run_id));
            plan.run_ids.push_back(spawn->parent_run_id);
            plan.relation_parent_ids.push_back(spawn->parent_run_id);
            plan.relation_rows.emplace_back(spawn->parent_run_id, spawn->child.child_run_id);
        }
        if (const auto& completion = publication.child_relations->completion) {
            if (completion->child.terminal_generation != 0)
                plan.lineage_ids.push_back(program_run_lineage_id(owner, completion->child.child_run_id));
            plan.relation_parent_ids.push_back(completion->parent_run_id);
            plan.relation_rows.emplace_back(completion->parent_run_id, completion->child.child_run_id);
        }
    }

    plan_detail::sort_unique(plan.lineage_ids);
    plan_detail::sort_unique(plan.run_ids);
    plan_detail::sort_unique(plan.relation_parent_ids);
    plan_detail::sort_unique(plan.relation_rows);
    return plan;
}

}  // namespace neograph::program::detail::transition
