// Forced into the DWARF build: Aurora's GXVert.h has no GX_WG, which the port's pipe stores use.
#define GX_WG(T, x) (GXWGFifo.T = (x))
