// The public header alone is enough to build the ToolService and destroy it:
// nothing here includes the generated neograph.grpc.pb.h or any grpc++ header,
// so the header has to bring what the returned pointer's destructor needs.
#include <neograph/grpc/tool_service.h>

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <utility>

TEST(GrpcToolServiceHeader, TheServiceIsBuiltAndDestroyedWithOnlyThePublicHeader) {
    std::unordered_map<std::string, neograph::grpc::ToolFn> tools;
    tools.emplace("echo", [](const std::string& arguments) { return arguments; });
    auto service = neograph::grpc::make_tool_service(std::move(tools));
    ASSERT_NE(service, nullptr);
    service.reset();
    EXPECT_EQ(service, nullptr);
}
