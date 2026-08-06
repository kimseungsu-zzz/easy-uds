# easy-uds

A lightweight Unix Domain Socket library for C++17.

## Features
- Simple request-response model
- No external dependencies
- Multi-threaded server

## API Reference
### Server
- `Server(std::string socket_path)`: creates a server.
- `void on(std::string route, std::function<Response(const Request&)> handler)`: registers a route handler.
- `void run()`: starts the server (blocking).
- `void stop()`: stops the server gracefully.

### Client
- `Client(std::string socket_path)`: creates a client.
- `Response request(std::string route, std::string body = "")`: sends a request and returns the response.

### Request/Response
- `Request`: has `route` and `body` fields.
- `Response`: has `status_code` and `body` fields.

## Example
See `examples/` directory.

## Build
```bash
mkdir build && cd build
cmake ..
make