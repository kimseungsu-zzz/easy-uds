# Installed Simple API consumer

This consumer intentionally uses only the installed `<easy_uds/simple.hpp>`
facade. It verifies constant and string-view handlers, a real `/echo` process
exchange, and the separation between `ResponseError` application failures and
Core transport errors.
