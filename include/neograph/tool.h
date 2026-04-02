#pragma once

#include <neograph/types.h>
#include <string>

namespace neograph {

class Tool {
  public:
    virtual ~Tool() = default;
    virtual ChatTool get_definition() const = 0;
    virtual std::string execute(const json& arguments) = 0;
    virtual std::string get_name() const = 0;
};

} // namespace neograph
