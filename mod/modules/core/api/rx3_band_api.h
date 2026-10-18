/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_BAND_API_H
#define RX3_BAND_API_H
/* Which frequency bands of a deck the effects act on, like the LOW / MID / HI
 * buttons of a DJM's Beat FX. One provider sets the masks (the isolator's
 * panel); any effect reads them. With no provider, or a mask of zero, every
 * band is in. Atomic words: safe from any thread, no waiting.
 */
#define RX3_BAND_LOW  1u
#define RX3_BAND_MID  2u
#define RX3_BAND_HIGH 4u
#define RX3_BAND_ALL  7u
struct rx3_band_service {
    int (*provide)(const void *owner);          /* one provider; a second is refused */
    void (*withdraw)(const void *owner);
    void (*set)(const void *owner, unsigned int deck, unsigned int mask);
    unsigned int (*mask)(unsigned int deck);    /* RX3_BAND_ALL when nobody provides */
};
extern const struct rx3_band_service rx3_bands;
#endif
