/*
 * Copyright (C) 2011 Martin Willi
 * Copyright (C) 2011 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "xauth.h"

#include <daemon.h>
#include <hydra.h>
#include <encoding/payloads/cp_payload.h>
#include <processing/jobs/adopt_children_job.h>

typedef struct private_xauth_t private_xauth_t;

/**
 * Status types exchanged
 */
typedef enum {
	XAUTH_FAILED = 0,
	XAUTH_OK = 1,
} xauth_status_t;

/**
 * Private members of a xauth_t task.
 */
struct private_xauth_t {

	/**
	 * Public methods and task_t interface.
	 */
	xauth_t public;

	/**
	 * Assigned IKE_SA.
	 */
	ike_sa_t *ike_sa;

	/**
	 * Are we the XAUTH initiator?
	 */
	bool initiator;

	/**
	 * XAuth backend to use
	 */
	xauth_method_t *xauth;

	/**
	 * XAuth username
	 */
	identification_t *user;

	/**
	 * Generated configuration payload
	 */
	cp_payload_t *cp;

	/**
	 * received identifier
	 */
	u_int16_t identifier;

	/**
	 * status of Xauth exchange
	 */
	xauth_status_t status;
};

/**
 * Load XAuth backend
 */
static xauth_method_t *load_method(private_xauth_t* this)
{
	identification_t *server, *peer;
	enumerator_t *enumerator;
	xauth_method_t *xauth;
	xauth_role_t role;
	peer_cfg_t *peer_cfg;
	auth_cfg_t *auth;
	char *name;

	if (this->initiator)
	{
		server = this->ike_sa->get_my_id(this->ike_sa);
		peer = this->ike_sa->get_other_id(this->ike_sa);
		role = XAUTH_SERVER;
	}
	else
	{
		peer = this->ike_sa->get_my_id(this->ike_sa);
		server = this->ike_sa->get_other_id(this->ike_sa);
		role = XAUTH_PEER;
	}
	peer_cfg = this->ike_sa->get_peer_cfg(this->ike_sa);
	enumerator = peer_cfg->create_auth_cfg_enumerator(peer_cfg, !this->initiator);
	if (!enumerator->enumerate(enumerator, &auth) ||
		(uintptr_t)auth->get(auth, AUTH_RULE_AUTH_CLASS) != AUTH_CLASS_XAUTH)
	{
		if (!enumerator->enumerate(enumerator, &auth) ||
			(uintptr_t)auth->get(auth, AUTH_RULE_AUTH_CLASS) != AUTH_CLASS_XAUTH)
		{
			DBG1(DBG_CFG, "no XAuth authentication round found");
			enumerator->destroy(enumerator);
			return NULL;
		}
	}
	name = auth->get(auth, AUTH_RULE_XAUTH_BACKEND);
	this->user = auth->get(auth, AUTH_RULE_XAUTH_IDENTITY);
	if (!this->initiator && this->user)
	{	/* use XAUTH username, if configured */
		peer = this->user;
	}
	enumerator->destroy(enumerator);
	xauth = charon->xauth->create_instance(charon->xauth, name, role,
										   server, peer);
	if (!xauth)
	{
		if (name)
		{
			DBG1(DBG_CFG, "no XAuth method found named '%s'", name);
		}
		else
		{
			DBG1(DBG_CFG, "no XAuth method found");
		}
	}
	return xauth;
}

/**
 * Check if XAuth connection is allowed to succeed
 */
static bool allowed(private_xauth_t *this)
{
	if (!charon->bus->authorize(charon->bus, FALSE))
	{
		DBG1(DBG_IKE, "XAuth authorization hook forbids IKE_SA, cancelling");
		return FALSE;
	}
	if (!charon->bus->authorize(charon->bus, TRUE))
	{
		DBG1(DBG_IKE, "final authorization hook forbids IKE_SA, cancelling");
		return FALSE;
	}
	return TRUE;
}

/**
 * Set IKE_SA to established state
 */
static bool establish(private_xauth_t *this)
{
	DBG0(DBG_IKE, "IKE_SA %s[%d] established between %H[%Y]...%H[%Y]",
		 this->ike_sa->get_name(this->ike_sa),
		 this->ike_sa->get_unique_id(this->ike_sa),
		 this->ike_sa->get_my_host(this->ike_sa),
		 this->ike_sa->get_my_id(this->ike_sa),
		 this->ike_sa->get_other_host(this->ike_sa),
		 this->ike_sa->get_other_id(this->ike_sa));

	this->ike_sa->set_state(this->ike_sa, IKE_ESTABLISHED);
	charon->bus->ike_updown(charon->bus, this->ike_sa, TRUE);

	return TRUE;
}

/**
 * Create auth config after successful authentication
 */
static void add_auth_cfg(private_xauth_t *this, identification_t *id, bool local)
{
	auth_cfg_t *auth;

	auth = auth_cfg_create();
	auth->add(auth, AUTH_RULE_AUTH_CLASS, AUTH_CLASS_XAUTH);
	auth->add(auth, AUTH_RULE_XAUTH_IDENTITY, id->clone(id));

	this->ike_sa->add_auth_cfg(this->ike_sa, local, auth);
}

METHOD(task_t, build_i_status, status_t,
	private_xauth_t *this, message_t *message)
{
	cp_payload_t *cp;

	cp = cp_payload_create_type(CONFIGURATION_V1, CFG_SET);
	cp->add_attribute(cp,
			configuration_attribute_create_value(XAUTH_STATUS, this->status));

	message->add_payload(message, (payload_t *)cp);

	return NEED_MORE;
}

METHOD(task_t, build_i, status_t,
	private_xauth_t *this, message_t *message)
{
	if (!this->xauth)
	{
		cp_payload_t *cp;

		this->xauth = load_method(this);
		if (!this->xauth)
		{
			return FAILED;
		}
		if (this->xauth->initiate(this->xauth, &cp) != NEED_MORE)
		{
			return FAILED;
		}
		message->add_payload(message, (payload_t *)cp);
		return NEED_MORE;
	}

	if (this->cp)
	{	/* send previously generated payload */
		message->add_payload(message, (payload_t *)this->cp);
		this->cp = NULL;
		return NEED_MORE;
	}
	return FAILED;
}

METHOD(task_t, build_r_ack, status_t,
	private_xauth_t *this, message_t *message)
{
	cp_payload_t *cp;

	cp = cp_payload_create_type(CONFIGURATION_V1, CFG_ACK);
	cp->set_identifier(cp, this->identifier);
	cp->add_attribute(cp,
			configuration_attribute_create_chunk(
					CONFIGURATION_ATTRIBUTE_V1, XAUTH_STATUS, chunk_empty));

	message->add_payload(message, (payload_t *)cp);

	if (this->status == XAUTH_OK && allowed(this) && establish(this))
	{
		return SUCCESS;
	}
	return FAILED;
}

METHOD(task_t, process_r, status_t,
	private_xauth_t *this, message_t *message)
{
	cp_payload_t *cp;

	if (!this->xauth)
	{
		this->xauth = load_method(this);
		if (!this->xauth)
		{	/* send empty reply */
			return NEED_MORE;
		}
	}
	cp = (cp_payload_t*)message->get_payload(message, CONFIGURATION_V1);
	if (!cp)
	{
		DBG1(DBG_IKE, "configuration payload missing in XAuth request");
		return FAILED;
	}
	if (cp->get_type(cp) == CFG_REQUEST)
	{
		switch (this->xauth->process(this->xauth, cp, &this->cp))
		{
			case NEED_MORE:
				return NEED_MORE;
			case SUCCESS:
			case FAILED:
			default:
				break;
		}
		this->cp = NULL;
		return NEED_MORE;
	}
	if (cp->get_type(cp) == CFG_SET)
	{
		configuration_attribute_t *attribute;
		enumerator_t *enumerator;

		enumerator = cp->create_attribute_enumerator(cp);
		while (enumerator->enumerate(enumerator, &attribute))
		{
			if (attribute->get_type(attribute) == XAUTH_STATUS)
			{
				this->status = attribute->get_value(attribute);
			}
		}
		enumerator->destroy(enumerator);
		if (this->status == XAUTH_OK)
		{
			DBG1(DBG_IKE, "XAuth authentication of '%Y' (myself) successful",
				 this->xauth->get_identity(this->xauth));
			add_auth_cfg(this, this->xauth->get_identity(this->xauth), TRUE);
		}
		else
		{
			DBG1(DBG_IKE, "XAuth authentication of '%Y' (myself) failed",
				 this->xauth->get_identity(this->xauth));
		}
	}
	this->identifier = cp->get_identifier(cp);
	this->public.task.build = _build_r_ack;
	return NEED_MORE;
}

METHOD(task_t, build_r, status_t,
	private_xauth_t *this, message_t *message)
{
	if (!this->cp)
	{	/* send empty reply if building data failed */
		this->cp = cp_payload_create_type(CONFIGURATION_V1, CFG_REPLY);
	}
	message->add_payload(message, (payload_t *)this->cp);
	this->cp = NULL;
	return NEED_MORE;
}

METHOD(task_t, process_i_status, status_t,
	private_xauth_t *this, message_t *message)
{
	cp_payload_t *cp;

	cp = (cp_payload_t*)message->get_payload(message, CONFIGURATION_V1);
	if (!cp || cp->get_type(cp) != CFG_ACK)
	{
		DBG1(DBG_IKE, "received invalid XAUTH status response");
		return FAILED;
	}
	if (this->status != XAUTH_OK)
	{
		DBG1(DBG_IKE, "destroying IKE_SA after failed XAuth authentication");
		return FAILED;
	}
	if (!establish(this))
	{
		return FAILED;
	}
	this->ike_sa->set_condition(this->ike_sa, COND_XAUTH_AUTHENTICATED, TRUE);
	lib->processor->queue_job(lib->processor, (job_t*)
				adopt_children_job_create(this->ike_sa->get_id(this->ike_sa)));
	return SUCCESS;
}

METHOD(task_t, process_i, status_t,
	private_xauth_t *this, message_t *message)
{
	identification_t *id;
	cp_payload_t *cp;

	cp = (cp_payload_t*)message->get_payload(message, CONFIGURATION_V1);
	if (!cp)
	{
		DBG1(DBG_IKE, "configuration payload missing in XAuth response");
		return FAILED;
	}
	switch (this->xauth->process(this->xauth, cp, &this->cp))
	{
		case NEED_MORE:
			return NEED_MORE;
		case SUCCESS:
			id = this->xauth->get_identity(this->xauth);
			if (this->user && !id->matches(id, this->user))
			{
				DBG1(DBG_IKE, "XAuth username '%Y' does not match to "
					 "configured username '%Y'", id, this->user);
				break;
			}
			DBG1(DBG_IKE, "XAuth authentication of '%Y' successful", id);
			add_auth_cfg(this, id, FALSE);
			if (allowed(this))
			{
				this->status = XAUTH_OK;
			}
			break;
		case FAILED:
			DBG1(DBG_IKE, "XAuth authentication of '%Y' failed",
				 this->xauth->get_identity(this->xauth));
			break;
		default:
			return FAILED;
	}
	this->public.task.build = _build_i_status;
	this->public.task.process = _process_i_status;
	return NEED_MORE;
}

METHOD(task_t, get_type, task_type_t,
	private_xauth_t *this)
{
	return TASK_XAUTH;
}

METHOD(task_t, migrate, void,
	private_xauth_t *this, ike_sa_t *ike_sa)
{
	DESTROY_IF(this->xauth);
	DESTROY_IF(this->cp);

	this->ike_sa = ike_sa;
	this->xauth = NULL;
	this->cp = NULL;
	this->user = NULL;
	this->status = XAUTH_FAILED;

	if (this->initiator)
	{
		this->public.task.build = _build_i;
		this->public.task.process = _process_i;
	}
	else
	{
		this->public.task.build = _build_r;
		this->public.task.process = _process_r;
	}
}

METHOD(task_t, destroy, void,
	private_xauth_t *this)
{
	DESTROY_IF(this->xauth);
	DESTROY_IF(this->cp);
	free(this);
}

/*
 * Described in header.
 */
xauth_t *xauth_create(ike_sa_t *ike_sa, bool initiator)
{
	private_xauth_t *this;

	INIT(this,
		.public = {
			.task = {
				.get_type = _get_type,
				.migrate = _migrate,
				.destroy = _destroy,
			},
		},
		.initiator = initiator,
		.ike_sa = ike_sa,
		.status = XAUTH_FAILED,
	);

	if (initiator)
	{
		this->public.task.build = _build_i;
		this->public.task.process = _process_i;
	}
	else
	{
		this->public.task.build = _build_r;
		this->public.task.process = _process_r;
	}
	return &this->public;
}
