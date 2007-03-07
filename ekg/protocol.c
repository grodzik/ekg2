/* $Id$ */

/*
 *  (C) Copyright 2003 Wojtek Kaniewski <wojtekka@irc.pl>
 *		  2004 Piotr Kupisiewicz <deli@rzepaknet.us>
 *		  2004 Adam Mikuta <adammikuta@poczta.onet.pl>
 *		  2005 Leszek Krupi�ski <leafnode@wafel.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ekg2-config.h"
#include "win32.h"

#include <stdio.h>
#include <sys/types.h>

#ifndef NO_POSIX_SYSTEM
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <string.h>

#include "debug.h"
#include "dynstuff.h"
#include "xmalloc.h"

#include "commands.h"
#include "emoticons.h"
#include "objects.h"
#include "userlist.h"
#include "windows.h"

#include "log.h"
#include "msgqueue.h"
#include "protocol.h"
#include "stuff.h"
#include "themes.h"

#include "queries.h"

static int auto_find_limit = 100; /* counter of persons who we were looking for when autofind */
list_t dccs = NULL;

static QUERY(protocol_disconnected);
static QUERY(protocol_connected);
static QUERY(protocol_message_ack);
static QUERY(protocol_status);
static QUERY(protocol_message);
static QUERY(protocol_xstate);

/**
 * protocol_init()
 *
 * Init communication between core and PROTOCOL plugins<br>
 * <br>
 * Here, we register <b>main</b> <i>communication channels</i> like:<br>
 * 	- status changes: 			<i>PROTOCOL_STATUS</i><br>
 * 	- message I/O:				<i>PROTOCOL_MESSAGE</i><br>
 * 	- acknowledge of messages: 		<i>PROTOCOL_MESSAGE_ACK</i><br>
 * 	- misc user events like typing notifies:<i>PROTOCOL_XSTATE</i><br>
 * 	- session connection/disconnection: 	<i>PROTOCOL_CONNECTED</i> and <i>PROTOCOL_DISCONNECTED</i>
 *
 * @sa query_connect()	- Function to add listener on specified events.
 * @sa query_emit()	- Function to emit specified events.
 */

void protocol_init() {
	query_connect_id(NULL, PROTOCOL_STATUS, protocol_status, NULL);
	query_connect_id(NULL, PROTOCOL_MESSAGE, protocol_message, NULL);
	query_connect_id(NULL, PROTOCOL_MESSAGE_ACK, protocol_message_ack, NULL);
	query_connect_id(NULL, PROTOCOL_XSTATE, protocol_xstate, NULL);

	query_connect_id(NULL, PROTOCOL_CONNECTED, protocol_connected, NULL);
	query_connect_id(NULL, PROTOCOL_DISCONNECTED, protocol_disconnected, NULL);
}


/**
 * protocol_reconnect_handler()
 *
 * Handler of reconnect timer created by protocol_disconnected()<br>
 *
 * @param type - 	0 - If timer should do his job<br>
 * 			1 - If timer'll be destroy, and handler should free his data
 * @param data - session uid to reconnect
 *
 * @return -1 [TEMPORARY TIMER]
 */

static TIMER(protocol_reconnect_handler) {
	char *session = (char*) data;
	session_t *s;

	if (type == 1) {
		xfree(session);
		return 0;
	}

	s = session_find(session);

        if (!s || session_connected_get(s) == 1)
                return -1;

	debug("reconnecting session %s\n", session);

	command_exec(NULL, s, ("/connect"), 0);
	return -1;
}

/**
 * protocol_disconnected()
 *
 * Handler for <i>PROTOCOL_DISCONNECTED</i><br>
 * When session notify core about disconnection we do here:<br>
 * 	- clear <b>all</b> user status, presence, resources details. @sa userlist_clear_status()<br>
 * 	- check if disconnect @a type was either <i>EKG_DISCONNECT_NETWORK:</i> or <i>EKG_DISCONNECT_FAILURE:</i> and
 * 		if yes, create reconnect timer (if user set auto_reconnect variable)<br>
 * 	- display notify through UI-plugin
 *
 * @note About different types [@a type] of disconnections:<br>
 * 		- <i>EKG_DISCONNECT_USER</i> 	- when user do /disconnect [<b>with reason</b>, in @a reason we should have param of /disconnect command][<b>without reconnection</b>]<br>
 * 		- <i>EKG_DISCONNECT_NETWORK</i>	- when smth is wrong with network... (read: when recv() fail, or send() or SSL wrappers for rcving/sending data fail with -1 and with bad errno)
 * 							[<b>without reason</b>][<b>without reconnection</b>]<br>
 *		- <i>EKG_DISCONNECT_FORCED</i>	- when server force us to disconnection. [<b>without reason</b>][<b>without reconnection</b>]<br>
 *		- <i>EKG_DISCONNECT_FAILURE</i> - when we fail to connect to server (read: when we fail connect session, after /connect) 
 *						[<b>with reason</b> describiny why we fail (strerror() is good here)][<b>with reconnection</b>]<br>
 *		- <i>EKG_DISCONNECT_STOPPED</i> - when user do /disconnect during connection [<b>without reason</b>] [<b>without reconnection</b>]<br>
 *
 * @todo Before creating reconnect timer, check if another one with the same name exists.
 *
 * @param ap 1st param: <i>(char *) </i><b>session</b> - session uid which goes disconnect
 * @param ap 2nd param: <i>(char *) </i><b>reason</b>  - reason why session goes disconnect.. It's reason specifed by user if EKG_DISCONNECT_USER, else 
 * 								string with error description like from: strerror().. [if EKG_DISCONNECT_FAILURE]
 * @param ap 3rd param: <i>(int) </i><b>type</b> - type of disconnection one of: 
 * 					[EKG_DISCONNECT_USER, EKG_DISCONNECT_NETWORK, EKG_DISCONNECT_FORCED, EKG_DISCONNECT_FAILURE, EKG_DISCONNECT_STOPPED]
 *
 * @param data NULL
 *
 * @return 0
 *
 */

static QUERY(protocol_disconnected)
{
	char *session	= *(va_arg(ap, char **));
	char *reason	= *(va_arg(ap, char **));
	int type	= *(va_arg(ap, int*));

	session_t *s	= session_find(session);

	userlist_clear_status(s, NULL);

	switch (type) {
		case EKG_DISCONNECT_NETWORK:
		case EKG_DISCONNECT_FAILURE:
		{
			int tmp;

			if (type == EKG_DISCONNECT_NETWORK)
				print("conn_broken", session_name(s));
			else
				print("conn_failed", reason, session_name(s));

			if (s && (tmp = session_int_get(s, "auto_reconnect")) && tmp != -1)
				timer_add(s->plugin, "reconnect", tmp, 0, protocol_reconnect_handler, xstrdup(session));

			break;
		}

		case EKG_DISCONNECT_USER:
			if (reason)
				print("disconnected_descr", reason, session_name(s));
			else
				print("disconnected", session_name(s));
			break;

		case EKG_DISCONNECT_FORCED:
			print("conn_disconnected", session_name(s));
			break;
			
		case EKG_DISCONNECT_STOPPED:
			print("conn_stopped", session_name(s));
			break;

		default:
			print("generic_error", "protocol_disconnect internal error, report to authors");
			break;
	}

	return 0;
}

/**
 * protocol_connected()
 *
 * Handler for <i>PROTOCOL_CONNECTED</i><br>
 * When session notify core about connection we do here:<br>
 * 	- If we have ourselves on the userlist. It update status and description<br>
 * 	- Display notify through UI-plugin<br>
 * 	- If we have messages in session queue, than send it and display info.
 *
 * @param ap 1st param: <i>(char *) </i><b>session</b> - session uid which goes connected.
 * @param data NULL
 *
 * @return 0
 */

static QUERY(protocol_connected) {
	char **session = va_arg(ap, char**);
	session_t *s = session_find(*session);
	const char *descr = session_descr_get(s);
	
        ekg_update_status(s);

	if (descr)
		print("connected_descr", descr, session_name(s));
	else
		wcs_print("connected", session_name(s));

	if (!msg_queue_flush(*session))
		wcs_print("queue_flush", session_name(s));

	return 0;
}

/*
 * protocol_status()
 *
 * obs�uga zapytania "protocol-status" wysy�anego przez pluginy protoko��w.
 */
static QUERY(protocol_status)
{
	char **__session	= va_arg(ap, char**), *session = *__session;
	char **__uid		= va_arg(ap, char**), *uid = *__uid;
	int status		= *(va_arg(ap, int*));
	char **__descr		= va_arg(ap, char**), *descr = *__descr;
	char *host		= *(va_arg(ap, char**));
	int port		= *(va_arg(ap, int*));
	time_t when		= *(va_arg(ap, time_t*));
	ekg_resource_t *r	= NULL;
	userlist_t *u;
	session_t *s;

	int st;				/* status	u->status || r->status */
	char *de;			/* descr 	u->descr  || r->descr  */

	int ignore_level;
        int ignore_status, ignore_status_descr, ignore_events, ignore_notify;
	int sess_notify;

	debug("protocol-status: %d\n", status);
	if (!(s = session_find(session)))
		return 0;
	
	sess_notify = session_int_get(s, "display_notify");
	/* we are checking who user we know */
	if (!(u = userlist_find(s, uid))) {
		if (config_auto_user_add) u = userlist_add(s, uid, uid);
		if (!u) {
			if ((sess_notify == -1 ? config_display_notify : sess_notify) & 4) {
				const char *format = ekg_status_label(status, descr, "status_");
				print_window(uid, s, 0, format, format_user(s, uid), NULL, session_name(s), descr);
			}
			return 0;
		}
	}
	ignore_level = ignored_check(s, uid);

	ignore_status = ignore_level & IGNORE_STATUS;
	ignore_status_descr = ignore_level & IGNORE_STATUS_DESCR;
	ignore_events = ignore_level & IGNORE_EVENTS;
	ignore_notify = ignore_level & IGNORE_NOTIFY;

	/* znajdz resource... */
	if (u->resources) {
		char *res = xstrchr(uid, '/');	/* resource ? */
		if (res) r = userlist_resource_find(u, res+1); 
	}

	/* status && descr ? */
	if (r) {		/* resource status && descr */
		st = r->status;
		de = r->descr;
	} else {		/* global status && descr */
		st = u->status;
		de = u->descr;
	}

	/* zapisz adres IP i port */
	u->ip = (host) ? inet_addr(host) : 0;
	u->port = port;

	if (host)
		u->last_ip = inet_addr(host);
	if (port)
		u->last_port = port;

	/* je�li te same stany...  i te same opisy (lub brak opisu), ignoruj */
	if ((status == st) && !xstrcmp(descr, de)) 
		return 0;

	/* je�li kto� nam znika, zapami�tajmy kiedy go widziano */
	if (!u->resources && (u->status > EKG_STATUS_NA) && (status <= EKG_STATUS_NA))
		u->last_seen = when ? when : time(NULL);

	/* XXX doda� events_delay */
	
	/* je�li dost�pny lub zaj�ty, dopisz to taba. je�li niedost�pny, usu� */
	if ((status >= EKG_STATUS_AVAIL) && config_completion_notify && u->nickname)
		tabnick_add(u->nickname);
	if ((status > EKG_STATUS_NA) && (status < EKG_STATUS_AVAIL) /* == aways */ && (config_completion_notify & 4) && u->nickname)
		tabnick_add(u->nickname);
	if ((status < EKG_STATUS_NA) && (config_completion_notify & 2) && u->nickname)
		tabnick_remove(u->nickname);


	/* je�li ma�o wa�na zmiana stanu...
	 * XXX someone can tell me what this should do, 'cos I can't understand the way it's written? */
	if ((sess_notify == -1 ? config_display_notify : sess_notify) & 2) {
		/* je�li na zaj�ty, ignorujemy */
		if (st == EKG_STATUS_AWAY)
			goto notify_plugins;

		/* je�li na dost�pny, ignorujemy */
		if (st == EKG_STATUS_AVAIL)
			goto notify_plugins;
	}

	/* ignorowanie statusu - nie wy�wietlamy, ale pluginy niech robi� co chc� */
        if (ignore_status)
		goto notify_plugins;

	/* nie zmieni� si� status, zmieni� si� opis */
	if (ignore_status_descr && (status == st) && xstrcmp(descr, de))
		goto notify_plugins;

	/* daj zna� d�wi�kiem... */
	if (config_beep && config_beep_notify)
		query_emit_id(NULL, UI_BEEP);

	/* ...i muzyczk� */
	if (config_sound_notify_file)
		play_sound(config_sound_notify_file);

        /* wy�wietla� na ekranie? */
	if (!((sess_notify == -1 ? config_display_notify : sess_notify) & 3))
                goto notify_plugins;

	/* poka� */
	if (u->nickname) {
		const char *format = ekg_status_label(status, ignore_status_descr ? NULL : descr, "status_");
		print_window(u->nickname, s, 0, format, format_user(s, uid), (u->first_name) ? u->first_name : u->nickname, session_name(s), descr);
	}

notify_plugins:
	if (st > EKG_STATUS_NA) {
	        u->last_status = st;
	        xfree(u->last_descr);
	        u->last_descr = xstrdup(de);
	}

	if ((st <= EKG_STATUS_NA) && (status > EKG_STATUS_NA) && !ignore_events)
		query_emit_id(NULL, EVENT_ONLINE, __session, __uid);

	if (!ignore_status) {
		if (r) {
			r->status = status;
		}

		if (u->resources) { 		/* get higest prio status */
			u->status = ((ekg_resource_t *) (u->resources->data))->status;
		} else {
			u->status = status;
		}
	}

	if (xstrcasecmp(de, descr) && !ignore_events)
		query_emit_id(NULL, EVENT_DESCR, __session, __uid, __descr);

	if (!ignore_status && !ignore_status_descr) {
		if (r) {
			xfree(r->descr);
			r->descr = xstrdup(descr);
		}

		if (u->resources) { 	/* get highest prio descr */
			xfree(u->descr);
			u->descr = xstrdup( ((ekg_resource_t *) (u->resources->data))->descr);
		} else {
			xfree(u->descr);
			u->descr = xstrdup(descr);
		}

		if (!u->resource || u->resources->data == r) 
			u->status_time = when ? when : time(NULL);
	}
	
	query_emit_id(NULL, USERLIST_CHANGED, __session, __uid);

	/* Currently it behaves like event means grouped statuses,
	 * i.e. EVENT_AVAIL is for avail&ffc
	 * 	EVENT_AWAY for away&xa&dnd
	 * 	... */
	if (!ignore_events) {
		if (status >= EKG_STATUS_AVAIL)
			query_emit_id(NULL, EVENT_AVAIL, __session, __uid);
		else if (status > EKG_STATUS_NA)
			query_emit_id(NULL, EVENT_AWAY, __session, __uid);
		else if (status <= EKG_STATUS_NA)
			query_emit_id(NULL, EVENT_NA, __session, __uid);
	}

	return 0;
}

/*
 * message_print()
 *
 * wy�wietla wiadomo�� w odpowiednim oknie i w odpowiedniej postaci.
 *
 * zwraca target
 */
char *message_print(const char *session, const char *sender, const char **rcpts, const char *__text, const uint32_t *format, time_t sent, int class, const char *seq, int dobeep, int secure)
{
	char *class_str, timestamp[100], *text = xstrdup(__text), *emotted = NULL;
	char *securestr = NULL;
	const char *target = sender, *user;
        time_t now;
	session_t *s = session_find(session);
        struct conference *c = NULL;
	int empty_theme = 0;

	if (class & EKG_NO_THEMEBIT) {
		empty_theme = 1;
		class &= ~EKG_NO_THEMEBIT;
	}

	switch (class) {
		case EKG_MSGCLASS_SENT:
		case EKG_MSGCLASS_SENT_CHAT:
			class_str = "sent";
			target = (rcpts) ? rcpts[0] : NULL;
			break;
		case EKG_MSGCLASS_CHAT:
			class_str = "chat";
			break;
		case EKG_MSGCLASS_SYSTEM:
			class_str = "system";
			target = "__status";
			break;
		default:
			class_str = "message";
	}

	/* dodajemy kolorki do tekstu */
	if (format) {
		string_t s = string_init("");
		const char *attrmap;
		int i;

		if (config_display_color_map && xstrlen(config_display_color_map) == 8)
			attrmap = config_display_color_map;
		else
			attrmap = "nTgGbBrR";

		for (i = 0; text[i]; i++) {
			if (i == 0 || format[i] != format[i - 1]) {
				char attr = 'n';

				if ((format[i] & EKG_FORMAT_COLOR)) {
					attr = color_map(format[i] & 0x0000ff, (format[i] & 0x0000ff00) >> 8, (format[i] & 0x00ff0000) >> 16);
					if (attr == 'k')
						attr = 'n';
				}

				if ((format[i] & 0xfe000000)) {
					uint32_t f = (format[i] & 0xff000000);

					if ((format[i] & EKG_FORMAT_COLOR))
						attr = toupper(attr);
					else
						attr = attrmap[(f >> 25) & 7];
				}

				string_append_c(s, '%');
				string_append_c(s, attr);
			}

			if (text[i] == '%')
				string_append_c(s, '%');
			
			string_append_c(s, text[i]);
		}

		xfree(text);
		text = format_string(s->str);
		string_free(s, 1);
	}

	/* wyznaczamy odst�p czasu mi�dzy wys�aniem wiadomo�ci, a chwil�
	 * obecn�, �eby wybra� odpowiedni format timestampu. */
	{
		char tmp[100], *timestamp_type;
	        struct tm *tm_msg;
		int tm_now_day;			/* it's localtime(&now)->tm_yday */

		now = time(NULL);

		tm_now_day = localtime(&now)->tm_yday;
		tm_msg = localtime(&sent);

		if (sent - config_time_deviation <= now && now <= sent + config_time_deviation)
			timestamp_type = "timestamp_now";
		else if (tm_now_day == tm_msg->tm_yday)
			timestamp_type = "timestamp_today";
		else	timestamp_type = "timestamp";

		snprintf(tmp, sizeof(tmp), "%s_%s", class_str, timestamp_type);
		if (!strftime(timestamp, sizeof(timestamp), format_find(tmp), tm_msg)
				&& xstrlen(format_find(tmp))>0)
			xstrcpy(timestamp, "TOOLONG");
	}

	/* if there is a lot of recipients, conference should be made */
	{
		int recipients_count = array_count((char **) rcpts);

		if (xstrcmp(class_str, "sent") && recipients_count > 0) {
			c = conference_find_by_uids(s, sender, rcpts, recipients_count, 0);

	                if (!c) {
	                        string_t tmp = string_init(NULL);
	                        int first = 0, i;
	
	                        for (i = 0; i < recipients_count; i++) {
	                                if (first++)
	                                        string_append_c(tmp, ',');
	
	                                string_append(tmp, rcpts[i]);
	                        }
	
	                        string_append_c(tmp, ' ');
	                        string_append(tmp, sender);
	
	                        c = conference_create(s, tmp->str);
	
	                        string_free(tmp, 1);
	                }

	                if (c) {
	                        target = c->name;
				class_str = "conference";
			}
		}
	}

	/* daj zna� d�wi�kiem i muzyczk� */
	if (class == EKG_MSGCLASS_CHAT) {

		if (config_beep && config_beep_chat && dobeep)
			query_emit_id(NULL, UI_BEEP);
	
		if (config_sound_chat_file && dobeep)
			play_sound(config_sound_chat_file);

	} else if (class == EKG_MSGCLASS_MESSAGE) {

		if (config_beep && config_beep_msg && dobeep)
			query_emit_id(NULL, UI_BEEP);
		if (config_sound_msg_file && dobeep)
			play_sound(config_sound_msg_file);

	} else if (class == EKG_MSGCLASS_SYSTEM && config_sound_sysmsg_file)
			play_sound(config_sound_sysmsg_file);
	
        if (config_last & 3 && xstrcmp(class_str, "sent")) 
	        last_add(0, sender, now, sent, text);
	
	user = xstrcmp(class_str, "sent") ? format_user(s, sender) : session_format_n(sender);

	if (config_emoticons && text)
		emotted = emoticon_expand(text);

	if (empty_theme)
		class_str = "empty";

	if (secure) 
		securestr = format_string(format_find("secure"));

	print_window(target, s, 
		(class == EKG_MSGCLASS_CHAT || class == EKG_MSGCLASS_SENT_CHAT
			|| (!(config_make_window & 4) && (class == EKG_MSGCLASS_MESSAGE || class == EKG_MSGCLASS_SENT))),
		class_str, 
		user, 
		timestamp, 
		(emotted) ? emotted : text, 
					/* XXX, get_uid() get_nickname() */
		(!xstrcmp(class_str, "sent")) ? session_alias_uid(s) : get_nickname(s, sender), 
		(!xstrcmp(class_str, "sent")) ? s->uid : get_uid(s, sender), 
		(secure) ? securestr : "");

	xfree(text);
	xfree(securestr);
	xfree(emotted);
	return xstrdup(target);
}

/*
 * protocol_message()
 */
static QUERY(protocol_message)
{
	char *session	= *(va_arg(ap, char**));
	char *uid	= *(va_arg(ap, char**));
	char **rcpts	= *(va_arg(ap, char***));
	char *text	= *(va_arg(ap, char**));
	uint32_t *format= *(va_arg(ap, uint32_t**));
	time_t sent	= *(va_arg(ap, time_t*));
	int class	= *(va_arg(ap, int*));
	char *seq	= *(va_arg(ap, char**));
	int dobeep	= *(va_arg(ap, int*));
	int secure	= *(va_arg(ap, int*));

	session_t *session_class = session_find(session);
	userlist_t *userlist = userlist_find(session_class, uid);
	char *target = NULL;
	int empty_theme = 0;
	int our_msg;

	if (ignored_check(session_class, uid) & IGNORE_MSG)
		return -1;

	/* display blinking */
	if (config_display_blinking && userlist && (class < EKG_MSGCLASS_SENT) && (!rcpts || !rcpts[0])) {
		if (config_make_window && xstrcmp(get_uid(session_class, window_current->target), get_uid(session_class, uid))) 
			userlist->xstate |= EKG_XSTATE_BLINK;
		else if (!config_make_window) {
			window_t *w;

			/*
			 * now we are checking if there is some window with query for this
			 * user 
			 */
			w = window_find_s(session_class, uid);

			if (!w && window_current->id != 1)
				userlist->xstate |= EKG_XSTATE_BLINK; 
			if (w && window_current->id != w->id)
				userlist->xstate |= EKG_XSTATE_BLINK;
		}
	}
	
	if (class & EKG_NO_THEMEBIT) {
		class &= ~EKG_NO_THEMEBIT;
		empty_theme = 1;
	}
	our_msg = (class >= EKG_MSGCLASS_SENT);

	/* there is no need to decode our messages */
	if (!our_msg && !empty_theme) {	/* empty_theme + decrpyt? i don't think so... */
                char *___session = xstrdup(session);
                char *___sender = xstrdup(uid);
                char *___message = xstrdup(text);
                int ___decrypted = 0;

                query_emit_id(NULL, MESSAGE_DECRYPT, &___session, &___sender, &___message, &___decrypted, NULL);

                if (___decrypted) {
                        text = ___message;
                        ___message = NULL;
                        secure = 1;
                }

                xfree(___session);
                xfree(___sender);
                xfree(___message);
	}

	if (our_msg)	query_emit_id(NULL, PROTOCOL_MESSAGE_SENT, &session, &(rcpts[0]), &text);
	else		query_emit_id(NULL, PROTOCOL_MESSAGE_RECEIVED, &session, &uid, &rcpts, &text, &format, &sent, &class, &seq, &secure);

	query_emit_id(NULL, PROTOCOL_MESSAGE_POST, &session, &uid, &rcpts, &text, &format, &sent, &class, &seq, &secure);

	/* show it ! */
	if (!(our_msg && !config_display_sent)) {
		if (empty_theme)
			class |= EKG_NO_THEMEBIT;
	        target = message_print(session, uid, (const char**) rcpts, text, format, sent, class, seq, dobeep, secure);
	}

        /* je�eli nie mamy podanego uid'u w li�cie kontakt�w to trzeba go dopisa� do listy dope�nianych */
	if (!userlist) 
		tabnick_add(uid);

        if (!userlist && xstrcasecmp(session_class->uid, uid) && session_int_get(session_class, "auto_find") >= 1) {
                list_t l;
                int do_find = 1, i;

                for (l = autofinds, i = 0; l; l = l->next, i++) {
                        char *d = l->data;

                        if (!xstrcmp(d, uid)) {
                                do_find = 0;
                                break;
                        }
                }

                if (do_find) {
                        if (i == auto_find_limit) {
                                debug("// autofind reached %d limit, removing the oldest uin: %d\n", auto_find_limit, *((char *)autofinds->data));
                                list_remove(&autofinds, autofinds->data, 1);
                        }

                        list_add(&autofinds, (void *) xstrdup(uid), 0);

                        command_exec_format(target, session_class, 0, ("/find %s"), uid);
                }
        }

	xfree(target);
	return 0;
}

/**
 * protocol_message_ack()
 *
 * Handler for <i>PROTOCOL_MESSAGE_ACK</i>
 * When session notify core about receiving acknowledge of message we do here:<br>
 * 	- Remove message with given sequence id (@a seq) from msgqueue @sa msg_queue_remove_seq()<br>
 * 	- If @a config_display_ack variable was set to 1, or @a config_display_ack value match
 * 	  	type of @a __status. Than display notify through UI-plugin
 *
 * @note	About different types of confirmations (@a __status):<br>
 * 			- <i>EKG_ACK_DELIVERED</i> 	- when message was delivered to user<br>
 * 			- <i>EKG_ACK_QUEUED</i>		- when user is unavailable and server confirm accepting message to later delivery<br>
 * 			- <i>EKG_ACK_DROPPED</i>	- when user or server don't want our message [we don't know why] and message was dropped<br>
 * 			- <i>EKG_ACK_UNKNOWN</i>	- when it's not clear what happene to that message<br>
 *
 * @todo 	Should we remove msg from msgqueue only when sequenceid and session and rcpt matches?
 * 		I think it's buggy cause user at jabber can send us acknowledge of message
 * 		which we never send, but if seq match with other message, which wasn't send (because session was for example disconnected)
 * 		we remove that messageid, and than we'll never send it, and we'll never know that we don't send it.
 *
 * @param ap 1st param: <i>(char *) </i><b>session</b>  - session which send this notify
 * @param ap 2nd param: <i>(char *) </i><b>rcpt</b>     - user uid who confirm receiving messages
 * @param ap 3rd param: <i>(char *) </i><b>seq</b>	- sequence id of message
 * @param ap 4th param: <i>(char *) </i><b>__status</b> - type of confirmation one of: [EKG_ACK_DELIVERED, EKG_ACK_QUEUED, EKG_ACK_DROPPED, EKG_ACK_UNKNOWN]
 *
 * @param data NULL
 *
 * @return 0
 */

static QUERY(protocol_message_ack) {
	char *session		= *(va_arg(ap, char **));
	char *rcpt		= *(va_arg(ap, char **));
	char *seq		= *(va_arg(ap, char **));
	char *__status		= *(va_arg(ap, char **));

	session_t *s	= session_find(session);
	userlist_t *u	= userlist_find(s, rcpt);
	const char *target = (u && u->nickname) ? u->nickname : rcpt;

	int display = 0;
	char format[100];

	snprintf(format, sizeof(format), "ack_%s", __status);

	msg_queue_remove_seq(seq);
	
	if (config_display_ack == 1)
		display = 1;

	if (config_display_ack == 2 && !xstrcmp(__status, EKG_ACK_DELIVERED))
		display = 1;

	if (config_display_ack == 3 && !xstrcmp(__status, EKG_ACK_QUEUED))
		display = 1;

	if (display)
		print_window(target, s, 0, format, format_user(s, rcpt));

	return 0;
}

static QUERY(protocol_xstate)
{
	/* state contains xstate bits, which should be set, offstate those, which should be cleared */
	char **__session	= va_arg(ap, char**), *session = *__session;
	char **__uid		= va_arg(ap, char**), *uid = *__uid;
	int  state		= *(va_arg(ap, int*));
	int  offstate		= *(va_arg(ap, int*));

	session_t *s;
	userlist_t *u;
	window_t *w;

	if (!(s = session_find(session)))
		return 0;
	
	if ((w = window_find_s(s, uid))) {
		if (offstate & EKG_XSTATE_TYPING)
			w->act &= ~4;
		else if (state & EKG_XSTATE_TYPING)
			w->act |= 4;
		query_emit_id(NULL, UI_WINDOW_ACT_CHANGED);
	}

	if ((u = userlist_find(s, uid)) || (config_auto_user_add && (u = userlist_add(s, uid, uid)))) {
		if (offstate & EKG_XSTATE_TYPING)
			u->xstate &= ~EKG_XSTATE_TYPING;
		else if (state & EKG_XSTATE_TYPING)
			u->xstate |= EKG_XSTATE_TYPING;
		query_emit_id(NULL, USERLIST_CHANGED, __session, __uid);
	}

	return 0;
}

dcc_t *dcc_add(const char *uid, dcc_type_t type, void *priv)
{
	dcc_t *d = xmalloc(sizeof(dcc_t));
	int id = 1, id_ok = 0;
	list_t l;

	d->uid = xstrdup(uid);
	d->type = type;
	d->priv = priv;
	d->started = time(NULL);

	do {
		id_ok = 1;

		for (l = dccs; l; l = l->next) {
			dcc_t *D = l->data;
	
			if (D->id == id) {
				id++;
				id_ok = 0;
				break;
			}
		}
	} while (!id_ok);

	d->id = id;

	list_add(&dccs, d, 0);

	return d;
}

int dcc_close(dcc_t *d)
{
	if (!d)
		return -1;

	if (d->close_handler)
		d->close_handler(d);

	xfree(d->uid);
	xfree(d->filename);

	list_remove(&dccs, d, 1);

	return 0;
}

PROPERTY_MISC(dcc, close_handler, dcc_close_handler_t, NULL)
PROPERTY_STRING(dcc, filename)
PROPERTY_INT(dcc, offset, int)
PROPERTY_INT(dcc, size, int)
PROPERTY_STRING_GET(dcc, uid)
PROPERTY_INT_GET(dcc, id, int)
PROPERTY_PRIVATE(dcc)
PROPERTY_INT_GET(dcc, started, time_t)
PROPERTY_INT(dcc, active, int)
PROPERTY_INT(dcc, type, dcc_type_t)


/*
 * Local Variables:
 * mode: c
 * c-file-style: "k&r"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 * vim: sts=0 noexpandtab sw=8
 */
