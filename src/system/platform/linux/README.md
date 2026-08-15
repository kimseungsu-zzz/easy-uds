# Linux platform boundary

This directory is the reserved home for Linux-specific implementation pieces.
The 0.7.1 layout commit intentionally does not extract or wrap syscalls yet;
that dependency inventory is the next phase. Keep new platform-specific code
here instead of introducing a cross-platform virtual transport interface.
