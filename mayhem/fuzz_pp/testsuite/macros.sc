#define STR(x) #x
#define CAT(a, b) a ## b
#define VA(fmt, ...) f(fmt __VA_OPT__(,) __VA_ARGS__)
#if defined(FLAG) && (LEVEL > 1)
int enabled = CAT(le, vel);
#elif !defined(FLAG)
const char* s = STR(hello world);
#else
VA("x", 1, 2)
#endif
#include "inc.sh"
#include <self.sh>
int line = __LINE__; int c = __COUNTER__;
