#include <gio/gio.h>
#include "grd-rdp-channel-rdpei.h"
#include "grd-rdp-dvc.h"
#include "grd-session.h"
#include "grd-session-rdp.h"

typedef enum _ptStates
{
	PEN_TOUCH_STATE_OUTOFRANGE,
	PEN_TOUCH_STATE_HOVERING,
	PEN_TOUCH_STATE_ENGAGED
} PenTouchStates;

/*
 * Representation of the "Lifetime" state machine for touch and pen contacts
 */
struct touch_pen_contact_state_machine
{
	/*
	 * 0x01 - out of range
	 * 0x02 - hovering
	 * 0x03 - engaged
	 * */
	 PenTouchStates state;

	 /* store current position */
	 uint32_t x;
	 uint32_t y;

	 /* The EI touch contact */
	 struct ei_touch * touch;
};

struct _GrdRdpRdpei
{
  GObject parent;

  RdpeiServerContext *rdpei_context;
  gboolean channel_opened;
  gboolean channel_unavailable;
  gboolean in_shutdown;

  uint32_t channel_id;
  uint32_t dvc_subscription_id;
  gboolean subscribed_status;

  GrdSessionRdp *session_rdp;
  GrdRdpDvc *rdp_dvc;

  GThread *rdpei_thread;
  HANDLE stop_event;
  // GSource *channel_teardown_source;

  // TODO: this needs to be dynamically allocated when client is ready
  // and we have the number of touch states
  struct touch_pen_contact_state_machine contactStates[64];
};

G_DEFINE_TYPE (GrdRdpRdpei, grd_rdp_rdpei, G_TYPE_OBJECT)

/* this function updates the state machine, and, if required, calls
 * functions to signal libei that we have a change for the touch pointer
 * */
void modify_touch_contact(RdpeiServerContext* context, RDPINPUT_CONTACT_DATA * contact){
	int check1;
	int check2;
	int check3;
	int check4;
	UINT32 flags;

	struct touch_pen_contact_state_machine * fsm;
	GrdRdpRdpei * rdpei = context->user_data;
  	GrdSessionRdp *session_rdp = rdpei->session_rdp;
	GrdSession *session = GRD_SESSION(session_rdp);

	fsm = &rdpei->contactStates[contact->contactId];
	g_message("[RDP.RDPEI] FSM %i is at state: %i", contact->contactId, fsm->state);
	/* for lack of an idea on how to properly implement the state machine,
	 * use a cumbersome if-else structure
	 * */
	if (fsm->state == PEN_TOUCH_STATE_OUTOFRANGE){
		g_message("[RDP.RDPEI]: FSM contact state: OUT-OF-RANGE");
		flags = contact->contactFlags;
		check1 = ((
				flags | ~(
					RDPINPUT_CONTACT_FLAG_DOWN |
			  	    RDPINPUT_CONTACT_FLAG_INRANGE |
			  	    RDPINPUT_CONTACT_FLAG_INCONTACT
				)) == ~0);
		if (check1) {
			g_message("[RDP.RDPEI]: FSM transition from OOR -> ENGAGED");
			fsm->state = PEN_TOUCH_STATE_ENGAGED;
			fsm->x = contact->x;
			fsm->y = contact->y;

			// at this point we want to notify the compositor via libei
			if (fsm->touch != NULL){
				g_message("[RDP.RDPEI]:       WARNING: fsm->touch is not NULL!");
			}
			fsm->touch = rdpei_touch_ei_new_contact(session);
			if (fsm->touch != NULL)
				rdpei_touch_down_to_ei(fsm->touch, fsm->x, fsm->y);
		} else {
			// everything is should be ignored
			g_message("[RDP.RDPEI]: FSM ignoring invalid new state");
		}
	} else if (fsm->state == PEN_TOUCH_STATE_ENGAGED){
		g_message("[RDP.RDPEI]: FSM contact state: ENGAGED");
		flags = contact->contactFlags;
		// we have four possible transistions
		check1 = (flags & RDPINPUT_CONTACT_FLAG_UP);
		check2 = ((flags | ~(
				RDPINPUT_CONTACT_FLAG_UP |
				RDPINPUT_CONTACT_FLAG_CANCELED
			)) == ~0);
		check3 = ((flags | ~(
				RDPINPUT_CONTACT_FLAG_UPDATE |
				RDPINPUT_CONTACT_FLAG_INRANGE |
				RDPINPUT_CONTACT_FLAG_INCONTACT
			)) == ~0);
		check4 = ((flags | ~(
				RDPINPUT_CONTACT_FLAG_UP|
				RDPINPUT_CONTACT_FLAG_INRANGE
			)) == ~0);
		if (check1){
			// transition to Out-of-range
			g_message("[RDP.RDPEI]: FSM transition from ENGAGED -> OOR (check1)");
			fsm->state = PEN_TOUCH_STATE_OUTOFRANGE;
			fsm->x = 0;
			fsm->y = 0;
			// notify libei
			rdpei_touch_up_to_ei(fsm->touch);
			// note: the contact->touch should now be de-referenced
			fsm->touch = NULL;

		} else if (check2) {
			// transition to Out-of-range
			g_message("[RDP.RDPEI]: FSM transition from ENGAGED -> OOR (check2)");
			fsm->state = PEN_TOUCH_STATE_OUTOFRANGE;
			fsm->x = 0;
			fsm->y = 0;
			// notify libei
			rdpei_touch_up_to_ei(fsm->touch);
			// note: the contact->touch should now be de-referenced

		} else if (check3) {
			// update position, state stays
			g_message("[RDP.RDPEI]: FSM stay at ENGAGED (update x/y)");
			fsm->state = PEN_TOUCH_STATE_ENGAGED;
			if ((fsm->x != contact->x) | (fsm->y != contact->y)){
				g_message("[RDP.RDPEI]: FSM sending motion notion");
				fsm->x = contact->x;
				fsm->y = contact->y;
				rdpei_touch_motion_to_ei(fsm->touch, fsm->x, fsm->y);
			} else {
				fsm->x = contact->x;
				fsm->y = contact->y;
			}

		} else if (check4) {
			// transition to HOVERING
			// TODO for pen

		}

	} else if (fsm->state == PEN_TOUCH_STATE_HOVERING){
		// we ignore this state for now, will only be relevant for pen
	}

}


/* the thread that handles events */
static gpointer
rdpei_thread_func (gpointer data)
{
  GrdRdpRdpei *rdpei = data;
  /* HANDLE channel_event; */
  HANDLE events[32] = {};
  uint32_t n_events;

  g_message ("[RDP.RDPEI]: thead_func starting");

  n_events = 0;
  events[n_events++] = rdpei_server_get_event_handle(rdpei->rdpei_context);
  events[n_events++] = rdpei->stop_event;

  while (!rdpei->in_shutdown){
      g_message ("[RDP.RDPEI]: thead_func waiting for event");
      WaitForMultipleObjects (n_events, events, FALSE, INFINITE);
      g_message ("[RDP.RDPEI]: thead_func got event");
	  rdpei_server_handle_messages(rdpei->rdpei_context);
      g_message ("[RDP.RDPEI]: thead_func done handling event");
  }

  g_message ("[RDP.RDPEI]: thead_func post while loop");
  return NULL;
}


void grd_rdp_rdpei_maybe_init (GrdRdpRdpei *rdpei)
{
	if (rdpei->channel_opened || rdpei->channel_unavailable)
		return;
	g_message ("[RDP.RDPEI]: grd_rdp_rdpei_maybe_init");
	if (rdpei_server_init(rdpei->rdpei_context) != CHANNEL_RC_OK){
		 g_message("[RDP.RDPEI]: BAD: Server init failed");
		 rdpei->channel_unavailable = TRUE;
		 return;
	}

	rdpei->rdpei_thread = g_thread_new ("RDP RDPEI thread", rdpei_thread_func,
			rdpei);

    // RdpeiServerContext* rdpei_server_context_new;
	g_message("[RDP.RDPEI]: Sending SC-ready");
	// features is set to 0 here
	if (rdpei_server_send_sc_ready(rdpei->rdpei_context, RDPINPUT_PROTOCOL_V300, 0)){
		 g_message("[RDP.RDPEI]: BAD: Sending SC packet failed");
		 rdpei->channel_unavailable = TRUE;
		 return;
	}

	rdpei->channel_opened = TRUE;
}

static void
dvc_creation_status (gpointer user_data,
                     int32_t  creation_status)
{
  /* GrdRdpRdpei *rdpei = user_data; */

  g_message ("[RDP.RDPEI] dvc_creation_status");
  if (creation_status < 0)
    {
      g_warning ("[RDP.RDPEI] Failed to open channel "
                 "(CreationStatus %i). Terminating protocol", creation_status);
	  // TODO
      /* g_source_set_ready_time (audio_input->channel_teardown_source, 0); */
    }
}

static BOOL
rdpei_on_channel_id_assigned (RdpeiServerContext *rdpei_context,
                           uint32_t              channel_id)
{
  GrdRdpRdpei *rdpei = rdpei_context->user_data;

  // g_debug ("[RDP.RDPEI] DVC channel id assigned to id %u", channel_id);
  g_message ("[RDP.RDPEI] DVC channel id assigned to id %u", channel_id);
  rdpei->channel_id = channel_id;

  rdpei->dvc_subscription_id =
    grd_rdp_dvc_subscribe_dvc_creation_status (rdpei->rdp_dvc,
                                               channel_id,
                                               dvc_creation_status,
                                               rdpei);
  rdpei->subscribed_status = TRUE;

  return TRUE;
}

UINT onClientReady(RdpeiServerContext* context){
	g_message("[RDP.RDPEI]: onClientReady: clientVersion: 0x%x maxTouchPoints: %i",
			context->clientVersion,
			context->maxTouchPoints
		);
	return 0;
}

UINT onTouchEvent(RdpeiServerContext* context, const RDPINPUT_TOUCH_EVENT* touchEvent){
	int i;
	int j;
	int contactcount;
	UINT32 flags;
	GrdRdpRdpei * rdpei = context->user_data;
  	GrdSessionRdp *session_rdp = rdpei->session_rdp;
	GrdSession *session = GRD_SESSION(session_rdp);

	RDPINPUT_TOUCH_FRAME * tframe;
	RDPINPUT_CONTACT_DATA * contact;

	g_message("[RDP.RDPEI]: onTouchEvent");
	g_message("[RDP.RDPEI]: encode_time: %u frame-count: %u",
			touchEvent->encodeTime,
			touchEvent->frameCount
		);

	// first frame
	tframe = touchEvent->frames;
	for (i=0; i<touchEvent->frameCount; i++){
		g_message("[RDP.RDPEI]: Frame %i: frameoffset: %lu contactcount: %i",
			i,
			tframe->frameOffset,
			tframe->contactCount
		);
		contactcount = tframe->contactCount;
		contact = tframe->contacts;
		for (j=0;j<contactcount;j++){
			/*a contact point
			typedef struct
			{
					106     UINT32 contactId;
			 107     UINT16 fieldsPresent; /1* Mask of CONTACT_DATA_*_PRESENT values *1/ */
			/* 108     INT32 x; */
			/* 109     INT32 y; */
			/* 110     UINT32 contactFlags;     /1* See RDPINPUT_CONTACT_FLAG*  *1/ */
			/* 111     INT16 contactRectLeft;   /1* Present if CONTACT_DATA_CONTACTRECT_PRESENT *1/ */
			/* 112     INT16 contactRectTop;    /1* Present if CONTACT_DATA_CONTACTRECT_PRESENT *1/ */
			/* 113     INT16 contactRectRight;  /1* Present if CONTACT_DATA_CONTACTRECT_PRESENT *1/ */
			/* 114     INT16 contactRectBottom; /1* Present if CONTACT_DATA_CONTACTRECT_PRESENT *1/ */
			/* 115     UINT32 orientation; /1* Present if CONTACT_DATA_ORIENTATION_PRESENT, values in degree, [0-359] *1/ */
			/* 116     UINT32 pressure;    /1* Present if CONTACT_DATA_PRESSURE_PRESENT, normalized value [0-1024] *1/ */
			/* 117 } RDPINPUT_CONTACT_DATA; */
			g_message("[RDP.RDPEI]:     Frame: %i contact: %i id: %u x: %u y: %u",
					i, j,
					contact->contactId,
					contact->x,
					contact->y
			);
			UINT32 fieldsPresent;
			fieldsPresent = contact->fieldsPresent;
			if (fieldsPresent & CONTACT_DATA_CONTACTRECT_PRESENT)
			    g_message("[RDP.RDPEI]:          Field present: CONTACTRECT");
			if (fieldsPresent & CONTACT_DATA_ORIENTATION_PRESENT)
			    g_message("[RDP.RDPEI]:          Field present: Orientation");
			if (fieldsPresent & CONTACT_DATA_PRESSURE_PRESENT)
			    g_message("[RDP.RDPEI]:          Field present: Pressure");

			flags = contact->contactFlags;
			if (flags & RDPINPUT_CONTACT_FLAG_DOWN)
			    g_message("[RDP.RDPEI]:          RDPINPUT_CONTACT_FLAG_DOWN");
			if (flags & RDPINPUT_CONTACT_FLAG_UPDATE)
			    g_message("[RDP.RDPEI]:          RDPINPUT_CONTACT_FLAG_UPDATE");
			if (flags & RDPINPUT_CONTACT_FLAG_UP)
			    g_message("[RDP.RDPEI]:          RDPINPUT_CONTACT_FLAG_UP");
			if (flags & RDPINPUT_CONTACT_FLAG_INRANGE)
			    g_message("[RDP.RDPEI]:          RDPINPUT_CONTACT_FLAG_INRANGE");
			if (flags & RDPINPUT_CONTACT_FLAG_INCONTACT)
			    g_message("[RDP.RDPEI]:          RDPINPUT_CONTACT_FLAG_INCONTACT");
			if (flags & RDPINPUT_CONTACT_FLAG_CANCELED)
			    g_message("[RDP.RDPEI]:          RDPINPUT_CONTACT_FLAG_CANCELED");

			modify_touch_contact(
				context,
				contact);
			contact++;
		}
		// after processing all contacts, send the frame
		rdpei_touch_ei_send_frame(session);


		tframe++;
	}


	return 0;

}

UINT onPenEvent(RdpeiServerContext* context, const RDPINPUT_PEN_EVENT* penEvent){
	g_message("[RDP.RDPEI]: onPenEvent");

	/* rdpei_test_ses(); */


	/* GrdSessionPrivate *priv = grd_session_get_instance_private ( */
	/* 		context->user_data->session_rdp->parent); */

  /* if (!priv->ei_abs_pointer) */
    /* return; */

  /* ei_device_button_button (priv->ei_abs_pointer, button, state); */
  /* ei_device_frame (priv->ei_abs_pointer, g_get_monotonic_time ()); */
	return 0;

}

UINT onTouchReleased(RdpeiServerContext* context, BYTE contactId){
	g_message("[RDP.RDPEI]: onTouchReleased");
	return 0;

}


GrdRdpRdpei *grd_rdp_rdpei_new (GrdSessionRdp *session_rdp,
                                        GrdRdpDvc     *rdp_dvc,
                                        HANDLE         vcm,
                                        rdpContext    *rdp_context){
	  g_message ("[RDP.RDPEI]: grd_rdp_rdpei_new");
	  GrdRdpRdpei * rdpei;
	  RdpeiServerContext * rdpei_context;

	  rdpei = g_object_new(GRD_TYPE_RDP_RDPEI, NULL);
	  rdpei->in_shutdown = false;
	  if (!(rdpei->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL))){
		  g_error("[RDP.RDPEI] Failed to create stop event");
	  }
	  rdpei_context = rdpei_server_context_new(vcm);
	  if (!rdpei_context)
		  g_error("[RDP.RDPEI] Failed to create server context");

	  rdpei->rdpei_context = rdpei_context;
	  rdpei->session_rdp = session_rdp;
	  rdpei->rdp_dvc = rdp_dvc;
	  //

	  // set context callbacks (freerdp)
	  rdpei_context->onClientReady = onClientReady;
	  rdpei_context->onTouchEvent = onTouchEvent;
	  rdpei_context->onPenEvent = onPenEvent;
	  rdpei_context->onTouchReleased = onTouchReleased;

	  /* rdpei_context->onClientReady = NULL; */
	  /* rdpei_context->onTouchEvent = NULL; */
	  /* rdpei_context->onPenEvent = NULL; */
	  /* rdpei_context->onTouchReleased = NULL; */
	  rdpei_context->onChannelIdAssigned = rdpei_on_channel_id_assigned;

	  // not required for rdpei?
	  /* rdpei_context->rdp_context = rdp_context; */
	  rdpei_context->user_data = rdpei;


	  return rdpei;
}

void
grd_rdp_rdpei_invoke_shutdown (GrdRdpRdpei *rdpei)
{
	  g_message ("[RDP.RDPEI] invoking shutdown");
	  rdpei->in_shutdown = true;
	  (void)SetEvent(rdpei->stop_event);

	  g_clear_pointer(&rdpei->rdpei_thread, g_thread_join);
	  rdpei_server_context_reset(rdpei->rdpei_context);
	  rdpei_server_context_free(rdpei->rdpei_context);
}

void rdpei_test()
{

	  g_message ("[RDP.RDPEI] test");
}

static void
grd_rdp_rdpei_dispose (GObject *object)
{
  // 	GrdRdpRdpei *rdpei = GRD_RDP_RDPEI (object);
	/* TODO */
	  g_message ("[RDP.RDPEI] grd_rdp_rdpei_dispose");
}

static void
grd_rdp_rdpei_init (GrdRdpRdpei *rdpei)
{
	/* TODO */
	  g_message ("[RDP.RDPEI] grd_rdp_rdpei_init");
}

static void
grd_rdp_rdpei_class_init (GrdRdpRdpeiClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_rdpei_dispose;
}
