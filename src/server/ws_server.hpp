#pragma once
/// @file ws_server.hpp
/// @brief Boost.Beast WebSocket + HTTP server for real-time event streaming.

#include "tracker/block_metadata.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mmap_viz {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;

/// @brief Callback to get the current snapshot JSON string for new clients.
using SnapshotProvider = std::function<std::string()>;

/// @brief Callback invoked when a WebSocket client sends a text message.
using CommandHandler = std::function<void(const std::string &)>;

/// @brief A single WebSocket session (one connected browser client).
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
  explicit WsSession(tcp::socket socket, std::string web_root,
                     CommandHandler on_command);

  /// @brief Start the session: read HTTP upgrade request,
  ///        serve static files, or upgrade to WebSocket.
  void run();

  /// @brief Send a text message to this client (thread-safe via strand).
  void send(std::string message);

  /// @brief Check if the session is still alive.
  [[nodiscard]] auto is_open() const -> bool;

private:
  void on_accept(beast::error_code ec);
  void do_read();
  void on_read(beast::error_code ec, std::size_t bytes_transferred);
  void handle_http_request(http::request<http::string_body> req);
  auto serve_file(const std::string &path) -> http::response<http::string_body>;
  auto mime_type(const std::string &path) -> std::string;

  beast::flat_buffer buffer_;
  ws::stream<beast::tcp_stream> ws_;
  http::request<http::string_body> req_;
  bool is_websocket_ = false;
  std::string web_root_;
  CommandHandler on_command_;
};

/// @brief WebSocket + HTTP server that broadcasts AllocationEvents to all
/// clients.
class WsServer {
public:
  /// @brief Construct the server.
  /// @param port      TCP port to listen on.
  /// @param web_root  Path to the web/ directory for serving static files.
  /// @param provider  Callback to generate the snapshot JSON for new WebSocket
  /// clients.
  WsServer(unsigned short port, std::string web_root,
           SnapshotProvider provider);

  /// @brief Start accepting connections. Blocks on io_context.run().
  void run();

  /// @brief Stop the server.
  void stop();

  /// @brief Broadcast a JSON message to all connected WebSocket clients.
  void broadcast(const std::string &message);

  /// @brief Set or replace the snapshot provider.
  void set_snapshot_provider(SnapshotProvider provider);

  /// @brief Set the command handler for incoming WebSocket messages.
  void set_command_handler(CommandHandler handler);

  /// @brief Get the io_context (for posting work from other threads).
  auto get_io_context() -> net::io_context &;

private:
  void do_accept();

  net::io_context ioc_{1};
  tcp::acceptor acceptor_;
  std::string web_root_;
  SnapshotProvider snapshot_provider_;

  std::mutex sessions_mutex_;
  std::vector<std::shared_ptr<WsSession>> sessions_;
  CommandHandler command_handler_;
};

} // namespace mmap_viz
