/**
 * @file program/blocking.h
 * @brief Typed refusals of a Program call that would block a thread that must not block (#557).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/configuration_failure.h>

#include <stdexcept>
#include <string>

namespace neograph::program {

/**
 * Thrown by the ProgramRuntime constructor in a build without threads (NEOGRAPH_THREADS=OFF) when
 * RuntimeConfig::executor is not set: the runtime's own pools need threads that this build does not have. It is also
 * a graph::ConfigurationFailure: running it again reproduces it.
 */
class NEOGRAPH_PROGRAM_API ProgramExecutorRequired final : public std::runtime_error,
                                                           public graph::ConfigurationFailure {
public:
    ProgramExecutorRequired();
};

/**
 * Thrown by a synchronous call that would wait for a Program on a thread that cannot wait for it: the executor of
 * RuntimeConfig::executor is pumped by a host's event loop, the build has no threads, or the call is made from a
 * thread that runs that executor. Nothing is started or changed. alternative() names the awaitable form to use. It is
 * also a graph::ConfigurationFailure: running it again reproduces it.
 *
 * A Blocking...Store view (store_call.h) throws it too when its policy refuses to wait for a store that is not
 * synchronous. operation() then names the store operation, for example "ProgramTransitionStore::load", the store is not
 * touched, and alternative() is empty.
 */
class NEOGRAPH_PROGRAM_API ProgramBlockingCallRefused final : public std::runtime_error,
                                                              public graph::ConfigurationFailure {
public:
    ProgramBlockingCallRefused(std::string operation, std::string alternative, std::string reason);
    /// The refused call, for example "ProgramRuntime::start".
    const std::string& operation() const noexcept { return operation_; }
    /// The awaitable form that does the same without blocking, for example "ProgramRuntime::start_async".
    const std::string& alternative() const noexcept { return alternative_; }

private:
    std::string operation_;
    std::string alternative_;
};

}  // namespace neograph::program
