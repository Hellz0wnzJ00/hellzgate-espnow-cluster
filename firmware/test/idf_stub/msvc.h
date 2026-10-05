// only for the Windows host runner; /Zp1 supplies the packed layout.
#define __attribute__(x)
#define gmtime_r(t, out) (gmtime_s((out), (t)) == 0 ? (out) : NULL)
