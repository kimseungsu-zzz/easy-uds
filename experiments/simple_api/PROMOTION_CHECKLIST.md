# Simple API v1 promotion checklist

- [x] Response/application error semantics fixed with `ResponseError`
- [x] Transport/protocol failures remain `easy_uds::Error`
- [x] Temporary-only route proxy; retained proxy fails to compile
- [x] Null C-string result has deterministic error text
- [x] `()` and `(std::string_view)` signatures are the only accepted inputs
- [x] GCC and Clang diagnostic probes check key phrases without golden files
- [x] Core escape hatch shares the same server lifecycle and registry
- [x] Duplicate routes follow Core rejection semantics
- [x] Simple/Core c1, c8, c32 A/B benchmark recorded
- [x] Allocation and RSS comparison recorded
- [x] Installed package consumer builds and runs `/ping`, `/echo`, and an
      application error
- [x] Lifecycle, handler exception, null result, and Core coexistence tests
- [x] Beginner guide and Core migration boundary documented
- [x] v1 scope frozen; no Simple Session/stream/typed RPC/retry/reconnect

The header is now installed as `<easy_uds/simple.hpp>`. This promotion is a
source-level API addition before 1.0, not a protocol or runtime-engine change.
