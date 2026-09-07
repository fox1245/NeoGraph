#include <neograph/program/program.h>

#include "program_chat.h"
#include <cppdotenv/dotenv.hpp>
#include <httplib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using evolving_chat::json;
namespace {
std::string tenant(const httplib::Request& request) {
    const auto token = request.get_header_value("Authorization");
    if (token == "Bearer alice-demo") return "alice";
    if (token == "Bearer bob-demo") return "bob";
    throw std::invalid_argument("Use a demo tenant token");
}
}  // namespace
int main(int argc, char** argv) {
    try {
        evolving_chat::Options options;
        std::string            env_file;
        bool                   no_env = false;
        unsigned               port   = 8768;
        std::string            script;
        bool                   crash_after_script = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg   = argv[i];
            auto        value = [&]() -> std::string {
                if (++i >= argc) throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (arg == "--live")
                options.mock = false;
            else if (arg == "--mock")
                options.mock = true;
            else if (arg == "--db")
                options.database = value();
            else if (arg == "--session")
                options.session = value();
            else if (arg == "--model")
                options.model = value();
            else if (arg == "--env-file")
                env_file = value();
            else if (arg == "--no-env")
                no_env = true;
            else if (arg == "--port")
                port = std::stoul(value());
            else if (arg == "--max-tokens")
                options.max_tokens = std::stoul(value());
            else if (arg == "--max-calls")
                options.max_calls = std::stoul(value());
            else if (arg == "--max-output-tokens")
                options.max_output_tokens = std::stoul(value());
            else if (arg == "--script")
                script = value();
            else if (arg == "--crash-after-script")
                crash_after_script = true;
            else if (arg == "--allow-loopback-provider")
                options.allow_loopback = true;
            else if (arg == "--help") {
                std::cout
                    << "program_chatbot [--mock|--live] [--db PATH] [--session NAME] [--port "
                       "8768]\n"
                       "OpenRouter: OPENROUTER_API_KEY + OPENROUTER_MODEL. PostgreSQL: "
                       "NEOGRAPH_CHAT_POSTGRES_URL.\n"
                       "--env-file PATH selects a dotenv file; otherwise discover .env from cwd.\n"
                       "--no-env disables discovery. Existing environment values take precedence.\n"
                       "--model overrides OPENROUTER_MODEL; live default: z-ai/glm-5.3-flash.\n"
                       "--max-output-tokens 2048 bounds each model completion.\n"
                       "--script JSON_FILE runs [{tenant,request_id,message,force_swap?}, ...].\n"
                       "--crash-after-script exits without cancelling runs for recovery testing.\n";
                return 0;
            } else
                throw std::invalid_argument("Unknown option: " + arg);
        }
        if (!port || port > 65535) throw std::invalid_argument("Invalid port");
        if (no_env && !env_file.empty())
            throw std::invalid_argument("Choose --env-file or --no-env");
        const auto      path = no_env             ? std::filesystem::path{}
                               : env_file.empty() ? cppdotenv::find_dotenv()
                                                  : std::filesystem::path(env_file);
        cppdotenv::Dict values;
        if (!path.empty()) {
            if (!std::filesystem::is_regular_file(path) ||
                std::filesystem::file_size(path) > 1024 * 1024)
                throw std::runtime_error("Dotenv file is missing, unreadable or too large");
            values = cppdotenv::dotenv_values(path);
        }
        const auto setting = [&](const char* key) -> std::string {
            if (const char* value = std::getenv(key)) return value;
            const auto found = values.find(key);
            return found == values.end() ? "" : found->second;
        };
        options.api_key = setting("OPENROUTER_API_KEY");
        if (options.model.empty()) options.model = setting("OPENROUTER_MODEL");
        if (!options.mock && options.model.empty()) options.model = "z-ai/glm-5.3-flash";
        options.postgres_url = setting("NEOGRAPH_CHAT_POSTGRES_URL");
        if (!setting("NEOGRAPH_CHAT_BASE_URL").empty())
            options.base_url = setting("NEOGRAPH_CHAT_BASE_URL");
        evolving_chat::Chat chat(options);
        if (!script.empty()) {
            std::ifstream file(script);
            const auto    commands = json::parse(file);
            for (const auto& c : commands) {
                chat.turn(c.at("tenant").get<std::string>(), c.at("request_id").get<std::string>(),
                          c.at("message").get<std::string>(), c.value("force_swap", false));
                std::cout << chat.state(c.at("tenant").get<std::string>()).dump() << std::endl;
            }
            if (crash_after_script)
                std::_Exit(0);  // Explicit process-loss test, all snapshots committed.
            return 0;
        }
        std::ifstream     page_file(CHAT_PAGE_PATH, std::ios::binary);
        const std::string page((std::istreambuf_iterator<char>(page_file)), {});
        if (page.empty()) throw std::runtime_error("Chat UI file missing");
        httplib::Server server;
        server.set_payload_max_length(12288);
        server.set_read_timeout(10);
        server.set_write_timeout(180);
        server.set_default_headers({{"Cache-Control", "no-store"},
                                    {"X-Content-Type-Options", "nosniff"},
                                    {"Content-Security-Policy",
                                     "default-src 'self'; script-src 'unsafe-inline'; style-src "
                                     "'unsafe-inline'; frame-ancestors 'none'"}});
        server.Get("/", [&](const auto&, auto& res) {
            res.set_content(page, "text/html; charset=utf-8");
        });
        auto api = [&](auto operation) {
            return [&, operation](const httplib::Request& req, httplib::Response& res) {
                std::string name;
                try {
                    name = tenant(req);
                } catch (...) {
                    res.status = 401;
                    res.set_content(json{{"error", "Unauthorized demo token"}}.dump(),
                                    "application/json");
                    return;
                }
                try {
                    res.set_content(operation(name, req).dump(), "application/json");
                } catch (const json::exception&) {
                    res.status = 400;
                    res.set_content(json{{"error", "Invalid JSON request"}}.dump(),
                                    "application/json");
                } catch (const std::invalid_argument& e) {
                    res.status = 400;
                    res.set_content(json{{"error", e.what()}}.dump(), "application/json");
                } catch (const std::exception& e) {
                    // Provider diagnostics may contain URLs/request details. Keep them off HTTP
                    // responses.
                    std::cerr << "Chat operation failed for " << name << ": " << e.what() << '\n';
                    res.status = 409;
                    res.set_content(json{{"error",
                                          "Turn could not complete. Inspect session status and "
                                          "local diagnostics; no automatic retry."}}
                                        .dump(),
                                    "application/json");
                }
            };
        };
        server.Get("/api/state",
                   api([&](const auto& name, const auto&) { return chat.state(name); }));
        server.Post("/api/turn", api([&](const auto& name, const auto& req) {
                        const json body = json::parse(req.body);
                        if (!body.is_object() || body.size() > 3 || !body.contains("request_id") ||
                            !body.contains("message"))
                            throw std::invalid_argument(
                                "Expected request_id, message and optional force_swap");
                        for (const auto& item : body.items())
                            if (item.first != "request_id" && item.first != "message" &&
                                item.first != "force_swap")
                                throw std::invalid_argument("Unknown request field");
                        return chat.turn(name, body.at("request_id").get<std::string>(),
                                         body.at("message").get<std::string>(),
                                         body.value("force_swap", false));
                    }));
        server.Post("/api/cancel", api([&](const auto& name, const auto&) {
                        chat.cancel(name);
                        return json{{"cancelled", true}};
                    }));
        std::cout << "NeoGraph Program chat: http://127.0.0.1:" << port << " ("
                  << (options.mock ? "DEMO" : "OpenRouter") << ")\n"
                  << std::flush;
        if (!server.listen("127.0.0.1", static_cast<int>(port)))
            throw std::runtime_error("Loopback port unavailable");
    } catch (const neograph::program::ProgramCompileError& e) {
        for (const auto& d : e.diagnostics())
            std::cerr << d.code << ": " << d.message << '\n';
        return 1;
    } catch (const neograph::program::ProgramAdmissionError& e) {
        for (const auto& d : e.diagnostics())
            std::cerr << d.code << ": " << d.message << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
