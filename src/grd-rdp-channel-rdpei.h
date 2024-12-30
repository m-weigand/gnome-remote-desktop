#ifndef GRD_RDP_RDPEI_H
#define GRD_RDP_RDPEI_H

#include <freerdp/server/rdpei.h>
#include <glib-object.h>
#include "grd-types.h"


#define GRD_TYPE_RDP_RDPEI (grd_rdp_rdpei_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpRdpei, grd_rdp_rdpei,
                      GRD, RDP_RDPEI, GObject)

void grd_rdp_rdpei_maybe_init (GrdRdpRdpei *rdpei);
GrdRdpRdpei *grd_rdp_rdpei_new (GrdSessionRdp *session_rdp,
                                        GrdRdpDvc     *rdp_dvc,
                                        HANDLE         vcm,
                                        rdpContext    *rdp_context);
void grd_rdp_rdpei_invoke_shutdown (GrdRdpRdpei *rdpei);
void rdpei_test();


#endif /* GRD_RDP_RDPEI */
