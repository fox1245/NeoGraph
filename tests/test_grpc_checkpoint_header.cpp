// The public header alone is enough to build the CheckpointService and destroy
// it: nothing here includes the generated neograph.grpc.pb.h or any grpc++
// header, so the header has to bring what the returned pointer's destructor
// needs.
#include <neograph/grpc/grpc_checkpoint.h>

#include <gtest/gtest.h>

#include <memory>

TEST(GrpcCheckpointHeader, TheServiceIsBuiltAndDestroyedWithOnlyThePublicHeader) {
    auto service = neograph::grpc::make_checkpoint_service(
        std::make_shared<neograph::graph::InMemoryCheckpointStore>());
    ASSERT_NE(service, nullptr);
    service.reset();
    EXPECT_EQ(service, nullptr);
}
