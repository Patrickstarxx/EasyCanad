#pragma once
// Host tests are single threaded. The real AVR compile uses avr-libc's restore-state guard.
#define ATOMIC_RESTORESTATE 0
#define ATOMIC_BLOCK(mode) for (bool atomicOnce = true; atomicOnce; atomicOnce = false)
