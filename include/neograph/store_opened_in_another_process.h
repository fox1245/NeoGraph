/**
 * @file store_opened_in_another_process.h
 * @brief The error of an operation on a store that another process opened.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/configuration_failure.h>

#include <stdexcept>
#include <string_view>

namespace neograph {

/**
 * An operation on a store whose database connection was opened in another process: a child made by fork that uses
 * the copy of a store it inherited from its parent. The database libraries do not allow a connection to be used in a
 * process other than the one that opened it, so the copy refuses every operation before it touches the connection or
 * takes the store's lock, and destroying the copy leaves the parent's connection alone. The child opens a new store
 * of its own.
 *
 * It is a graph::ConfigurationFailure: running the operation again in the same process reproduces it, so it is never
 * retried and never routed to a failure handler.
 */
class NEOGRAPH_API StoreOpenedInAnotherProcess final : public std::runtime_error,
                                                      public graph::ConfigurationFailure {
public:
    /// `store` names the kind of store, for the message.
    explicit StoreOpenedInAnotherProcess(std::string_view store);
    ~StoreOpenedInAnotherProcess() override;
};

}  // namespace neograph
