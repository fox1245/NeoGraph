// Entry-point for the asio smoke binary. Kept tiny so we can confirm
// the wiring in isolation without pulling in any neograph::* headers.

namespace neograph::async { int run_smoke(); }

int main() { return neograph::async::run_smoke(); }
