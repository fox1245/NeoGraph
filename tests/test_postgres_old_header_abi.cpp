#include "fixtures/postgres_checkpoint_origin.h"

#include <cstdlib>
#include <stdexcept>

int main() {
    using neograph::graph::PostgresCheckpointStore;

    try {
        PostgresCheckpointStore invalid("unused", 0);
        return 1;
    } catch (const std::invalid_argument&) {
    }

    const char* dsn = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    if (!dsn || !*dsn) return 0;

    PostgresCheckpointStore store(dsn, 1);
    if (store.pool_size() != 1 || store.reconnect_count() != 0) return 2;
    (void)store.load_latest("old-header-new-library");
    return 0;
}
