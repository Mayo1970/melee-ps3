// Aurora's header for the vector and matrix types, which the schema includes: the SDK declares them in mtx.h.
#include <dolphin/mtx.h>

// The schema names Aurora's tag; the SDK's Vec is an untagged struct of the same layout.
struct Vec {
    f32 x, y, z;
};
