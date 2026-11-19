#pragma once
// Horizon has no poll(2) with the POSIX shape Dawn expects; libnx provides poll(), so only the
// header indirection is missing. sys/poll.h is the legacy name some sources still include.

#include <poll.h>
