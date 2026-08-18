/* Compile-time guard that the test assert policy is active.
 *
 * The OtsarDB test suite uses assert() as executable test assertions, including
 * expressions with side effects (see CMakeLists.txt). CMAKE_BUILD_TYPE=Release
 * defines NDEBUG; CMakeLists.txt therefore undefines it for every target.
 * If that undefinition is ever lost or reordered for a test translation unit,
 * this file fails the build instead of silently disabling assertions.
 */
#include <assert.h>

#ifdef NDEBUG
#error "test assertions must stay active: NDEBUG is defined"
#endif

int main(void) {
    /* Provable at runtime only when assertions are compiled in. */
    assert(1 + 1 == 2);
    return 0;
}
