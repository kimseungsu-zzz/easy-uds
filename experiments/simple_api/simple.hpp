#pragma once

// Compatibility shim for the experiment. The audited adapter now lives at
// the public include path; this file keeps the prototype include command
// stable while the experiment remains separately buildable.
#include "easy_uds/simple.hpp"
