# Installed package consumer

This consumer intentionally uses only the installed umbrella header and the
beginner `Server`/`Client`/`request()` path. It is built by CI after
`find_package(easy_uds CONFIG REQUIRED)` and run against a real server process;
it does not include repository-relative `src/` or `detail` headers.
