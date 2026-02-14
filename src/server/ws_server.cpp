/// @file ws_server.cpp
/// @brief Implementation of the Boost.Beast WebSocket + HTTP server.

#include "server/ws_server.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace mmap_viz {

// ─── WsSession ──────────────────────────────────────────────────────────

WsSession::WsSession(tcp::socket socket, std::string web_root,
                     CommandHandler on_command)
    : ws_{std::move(socket)}, web_root_{std::move(web_root)},
      on_command_{std::move(on_command)} {}

void WsSession::run() {
  // Read the initial HTTP request to decide: WebSocket upgrade or static file.
  http::async_read(
      ws_.next_layer(), buffer_, req_,
      [self = shared_from_this()](beast::error_code ec, std::size_t) {
        if (ec)
          return;

        // Check if this is a WebSocket upgrade request.
        if (ws::is_upgrade(self->req_)) {
          self->ws_.async_accept(self->req_, [self](beast::error_code ec2) {
            self->on_accept(ec2);
          });
        } else {
          self->handle_http_request(std::move(self->req_));
        }
      });
}

void WsSession::on_accept(beast::error_code ec) {
  if (ec)
    return;
  is_websocket_ = true;
  do_read();
}

void WsSession::do_read() {
  ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec,
                                                      std::size_t bytes) {
    self->on_read(ec, bytes);
  });
}

void WsSession::on_read(beast::error_code ec,
                        std::size_t /*bytes_transferred*/) {
  if (ec == ws::error::closed)
    return;
  if (ec)
    return;

  // Forward client messages to the command handler.
  if (on_command_) {
    auto msg = beast::buffers_to_string(buffer_.data());
    if (!msg.empty()) {
      on_command_(msg);
    }
  }
  buffer_.consume(buffer_.size());
  do_read();
}

void WsSession::send(std::string message) {
  if (!is_websocket_)
    return;

  auto msg = std::make_shared<std::string>(std::move(message));

  net::post(ws_.get_executor(), [self = shared_from_this(), msg]() {
    beast::error_code ec;
    self->ws_.text(true);
    self->ws_.write(net::buffer(*msg), ec);
  });
}

auto WsSession::is_open() const -> bool {
  return is_websocket_ && ws_.is_open();
}

void WsSession::handle_http_request(http::request<http::string_body> req) {
  auto target = std::string(req.target());
  if (target == "/")
    target = "/index.html";

  auto response = serve_file(target);
  response.set(http::field::server, "MemoryMapper/0.1");
  // Force close — we don't loop to handle additional HTTP requests on this
  // connection.  Without this, the browser thinks the socket is still
  // streaming and the page "hangs" waiting for more data.
  response.keep_alive(false);
  response.prepare_payload();

  beast::error_code ec;
  http::write(ws_.next_layer(), response, ec);
}

auto WsSession::serve_file(const std::string &path)
    -> http::response<http::string_body> {
  auto full_path = web_root_ + path;

  if (!std::filesystem::exists(full_path)) {
    http::response<http::string_body> res{http::status::not_found, 11};
    res.set(http::field::content_type, "text/plain");
    res.body() = "404 Not Found: " + path;
    return res;
  }

  std::ifstream file(full_path, std::ios::binary);
  std::ostringstream ss;
  ss << file.rdbuf();

  http::response<http::string_body> res{http::status::ok, 11};
  res.set(http::field::content_type, mime_type(path));
  // Allow cross-origin for development.
  res.set(http::field::access_control_allow_origin, "*");
  res.body() = ss.str();
  return res;
}

auto WsSession::mime_type(const std::string &path) -> std::string {
  auto ext = std::filesystem::path(path).extension().string();
  if (ext == ".html")
    return "text/html";
  if (ext == ".css")
    return "text/css";
  if (ext == ".js")
    return "application/javascript";
  if (ext == ".json")
    return "application/json";
  if (ext == ".png")
    return "image/png";
  if (ext == ".svg")
    return "image/svg+xml";
  return "application/octet-stream";
}

// ─── WsServer ───────────────────────────────────────────────────────────

WsServer::WsServer(unsigned short port, std::string web_root,
                   SnapshotProvider provider)
    : acceptor_{ioc_, tcp::endpoint{tcp::v4(), port}},
      web_root_{std::move(web_root)}, snapshot_provider_{std::move(provider)} {
  acceptor_.set_option(net::socket_base::reuse_address(true));
}

void WsServer::run() {
  std::cout << "[WsServer] Listening on http://localhost:"
            << acceptor_.local_endpoint().port() << "\n";
  std::cout << "[WsServer] WebSocket at ws://localhost:"
            << acceptor_.local_endpoint().port() << "/ws\n";

  do_accept();
  ioc_.run();
}

void WsServer::stop() { ioc_.stop(); }

void WsServer::do_accept() {
  acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
    if (ec)
      return;

    auto session = std::make_shared<WsSession>(std::move(socket), web_root_,
                                               command_handler_);

    {
      std::lock_guard lock(sessions_mutex_);
      sessions_.push_back(session);
    }

    session->run();

    // Send snapshot to new WebSocket client after a brief delay
    // (allow the upgrade to complete first).
    if (snapshot_provider_) {
      auto snapshot = snapshot_provider_();
      net::post(ioc_, [session, snapshot = std::move(snapshot)]() {
        session->send(snapshot);
      });
    }

    do_accept();
  });
}

void WsServer::broadcast(const std::string &message) {
  std::lock_guard lock(sessions_mutex_);

  // Remove dead sessions.
  std::erase_if(sessions_, [](const auto &s) { return !s->is_open(); });

  for (auto &session : sessions_) {
    session->send(message);
  }
}

void WsServer::set_snapshot_provider(SnapshotProvider provider) {
  snapshot_provider_ = std::move(provider);
}

void WsServer::set_command_handler(CommandHandler handler) {
  command_handler_ = std::move(handler);
}

auto WsServer::get_io_context() -> net::io_context & { return ioc_; }

} // namespace mmap_viz
