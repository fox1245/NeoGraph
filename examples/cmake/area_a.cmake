# Area A: Program API and harness examples (65 to 74).
#
# The Program examples need neograph::program; the ones that compile JavaScript also need the
# QuickJS control frontend. A missing component leaves the example out of the build.

if(NEOGRAPH_BUILD_PROGRAM AND NEOGRAPH_BUILD_QUICKJS_CONTROL)
    neograph_add_example(program_javascript 65_program_javascript.cpp LINK neograph::program)
    neograph_add_example(program_failure_routing 66_program_failure_routing.cpp LINK neograph::program)
endif()
