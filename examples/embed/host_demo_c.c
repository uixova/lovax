/* Pure-C embedding demo (RFC-025): drives the Lovax VM through the C ABI only.
 * Proves a non-C++ host (or an ABI-stable plugin) can register natives, run a
 * script, read globals back, and call script functions.
 *
 * Build:
 *   g++ -std=c++17 -O2 -c src/embed/lovax.cpp -o lovax_capi.o
 *   gcc  -O2 examples/embed/host_demo_c.c lovax_capi.o -lstdc++ -o host_demo_c
 */
#include <stdio.h>
#include "../../src/embed/lovax.h"

/* a native the script can call: variadic integer sum */
static LovaxValue c_add(LovaxVM* vm, int argc, const LovaxValue* argv) {
    long long s = 0, t;
    int i;
    for (i = 0; i < argc; ++i) if (lovax_as_int(argv[i], &t)) s += t;
    return lovax_int(vm, s);
}

int main(void) {
    LovaxVM* vm = lovax_new();
    printf("== Lovax C ABI demo (abi v%d) ==\n", lovax_abi_version());

    lovax_register(vm, "c_add", c_add);

    const char* src =
        "say c_add(10, 20, 12)\n"          /* native call from script -> 42 */
        "set total = 0\n"
        "for i in range(1, 6):\n"
        "    total = total + i\n"          /* 1+2+3+4+5 = 15 */
        "fn double(x):\n"
        "    return x * 2\n";

    if (lovax_eval(vm, src) != 0) {
        printf("eval error: %s\n", lovax_last_error(vm));
        lovax_free(vm);
        return 1;
    }

    /* read a global the script set */
    LovaxValue g;
    long long tv = -1;
    if (lovax_get_global(vm, "total", &g) == 0 && lovax_as_int(g, &tv))
        printf("total (from script) = %lld\n", tv);

    /* host -> script call */
    LovaxValue arg = lovax_int(vm, 21), out;
    long long dv = -1;
    if (lovax_call(vm, "double", 1, &arg, &out) == 0 && lovax_as_int(out, &dv))
        printf("double(21) = %lld\n", dv);

    lovax_free(vm);
    return (tv == 15 && dv == 42) ? 0 : 2;
}
