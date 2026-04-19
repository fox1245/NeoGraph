-- Provision a separate database for PostgresCheckpointStore integration
-- tests. Mounted into postgres:16-alpine via docker-compose's
-- /docker-entrypoint-initdb.d hook, so it runs once on first volume
-- creation. Idempotent for safety on re-runs.
--
-- Why a separate DB: the integration tests' SetUp calls drop_schema(),
-- which is destructive. Keeping them in `neograph_test` stops a
-- `ctest` invocation from wiping demo threads in the main `neograph`
-- DB.
SELECT 'CREATE DATABASE neograph_test'
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'neograph_test')
\gexec
