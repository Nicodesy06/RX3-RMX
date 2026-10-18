/* SPDX-License-Identifier: MPL-2.0 */
/* Per-deck effect band masks: one provider writes, everybody reads. */
#include "../api/rx3_platform.h"
#include "../api/rx3_band_api.h"
static const void *provider;
static unsigned int masks[2] = {RX3_BAND_ALL, RX3_BAND_ALL};
static int provide(const void *owner)
{
    const void *expected = 0;
    return owner && __atomic_compare_exchange_n(&provider, &expected, owner, 0,
                                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static void withdraw(const void *owner)
{
    const void *expected = owner;
    if (owner && __atomic_compare_exchange_n(&provider, &expected, 0, 0,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        for (unsigned int deck = 0; deck < 2u; deck++)
            __atomic_store_n(&masks[deck], RX3_BAND_ALL, __ATOMIC_SEQ_CST);
}
static void set(const void *owner, unsigned int deck, unsigned int mask)
{
    if (deck < 2u && owner && __atomic_load_n(&provider, __ATOMIC_SEQ_CST) == owner)
        __atomic_store_n(&masks[deck], (mask & RX3_BAND_ALL) ? (mask & RX3_BAND_ALL) : RX3_BAND_ALL,
                         __ATOMIC_SEQ_CST);
}
static unsigned int mask(unsigned int deck)
{
    if (deck >= 2u || !__atomic_load_n(&provider, __ATOMIC_SEQ_CST)) return RX3_BAND_ALL;
    return __atomic_load_n(&masks[deck], __ATOMIC_SEQ_CST);
}
const struct rx3_band_service rx3_bands = {provide, withdraw, set, mask};
